#include "wifi_mgr.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "WIFI";

#define BIT_CONNECTED BIT0
#define NVS_NS        "wifi_cfg"

static EventGroupHandle_t s_events;
static char s_ip[16] = "0.0.0.0";
static char s_ssid[33] = "";
static char s_pass[65] = "";
/* 网关地址（ws://<mac>:8765/ws）。也走 NVS：出厂无任何配置时，
 * 控制台通过 USB 下发 wifi.set 可以顺带把它写进来，否则配好网也连不上 WS。 */
static char s_server[96] = "";
static bool s_started;

/* 扫描期间必须掐掉自动重连：STA_DISCONNECTED → esp_wifi_connect() 会形成死循环，
 * STA 始终停在 connecting，esp_wifi_scan_start 每次报 ESP_ERR_WIFI_STATE。
 * 配网入口（wifi_mgr_set_credentials）会先把它清零，所以不影响 wifi.set 的重连。 */
static volatile bool s_scanning;

/* ---------------- NVS 凭据存取 ---------------- */

static bool load_creds(char *ssid, size_t sl, char *pass, size_t pl)
{
    if (ssid == NULL || sl == 0) {
        return false;
    }
    ssid[0] = 0;
    if (pass != NULL && pl > 0) {
        pass[0] = 0;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sl;
    bool ok = (nvs_get_str(h, "ssid", ssid, &len) == ESP_OK) && strlen(ssid) > 0;
    if (ok && pass != NULL && pl > 0) {
        len = pl;
        if (nvs_get_str(h, "pass", pass, &len) != ESP_OK) {
            pass[0] = 0;
        }
    }
    nvs_close(h);
    return ok;
}

static esp_err_t save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_str(h, "ssid", ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(h, "pass", pass);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    return ret;
}

/* ---------------- 网关地址存取 ---------------- */

static bool load_server(char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return false;
    }
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t l = len;
    bool ok = (nvs_get_str(h, "srv", out, &l) == ESP_OK) && strlen(out) > 0;
    nvs_close(h);
    return ok;
}

esp_err_t wifi_mgr_set_server(const char *url)
{
    if (url == NULL || strlen(url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_str(h, "srv", url);
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    if (ret == ESP_OK) {
        snprintf(s_server, sizeof(s_server), "%s", url);
        ESP_LOGI(TAG, "网关地址已更新: %s", s_server);
    }
    return ret;
}

const char *wifi_mgr_server(void)
{
    if (strlen(s_server) > 0) {
        return s_server;
    }
    return CONFIG_VOICE_SERVER_URL;
}

/* ---------------- 事件 ---------------- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    /* 没有 SSID 时不要 connect：esp_wifi_connect 返回 ESP_ERR_WIFI_SSID，
     * 而 DISCONNECTED 事件又再次触发 connect，形成空转刷屏 */
    bool has_ssid = (strlen(s_ssid) > 0);

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_scanning && has_ssid) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        strcpy(s_ip, "0.0.0.0");
        ESP_LOGW(TAG, "断开，重连中");
        if (!s_scanning && has_ssid) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        ESP_LOGI(TAG, "拿到 IP: %s", s_ip);
    }
}

static esp_err_t apply_sta(const char *ssid, const char *pass)
{
    wifi_config_t wcfg = { 0 };
    snprintf((char *)wcfg.sta.ssid, sizeof(wcfg.sta.ssid), "%s", ssid);
    snprintf((char *)wcfg.sta.password, sizeof(wcfg.sta.password), "%s", pass);
    wcfg.sta.threshold.authmode = strlen(pass) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    return esp_wifi_set_config(WIFI_IF_STA, &wcfg);
}

/* ---------------- 对外接口 ---------------- */

esp_err_t wifi_mgr_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL, NULL));

    /* 凭据优先级：NVS（控制台在线切换过） > menuconfig 编译期默认 */
    if (!load_creds(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass))) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", CONFIG_VOICE_WIFI_SSID);
        snprintf(s_pass, sizeof(s_pass), "%s", CONFIG_VOICE_WIFI_PASSWORD);
    }
    if (!load_server(s_server, sizeof(s_server))) {
        snprintf(s_server, sizeof(s_server), "%s", CONFIG_VOICE_SERVER_URL);
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (strlen(s_ssid) == 0) {
        /* 出厂 / 清空 NVS 后的状态：没有 SSID 也必须把 STA 栈起起来。
         * 否则 esp_wifi_scan_start 报 ESP_ERR_INVALID_STATE，
         * 控制台通过 USB 下发的 wifi.set 也无处落地——也就是"没网就再也配不了网"的死锁。 */
        ESP_LOGW(TAG, "未配置 WiFi SSID：STA 栈已启动，等待控制台下发 wifi.set");
    } else {
        ESP_ERROR_CHECK(apply_sta(s_ssid, s_pass));
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    s_started = true;
    ESP_LOGI(TAG, "STA 启动，目标 SSID: %s", s_ssid);
    return ESP_OK;
}

bool wifi_mgr_connected(void)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}

bool wifi_mgr_wait_connected(int timeout_ms)
{
    if (s_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_CONNECTED) != 0;
}

const char *wifi_mgr_ip(void)
{
    return s_ip;
}

int wifi_mgr_rssi(void)
{
    if (!wifi_mgr_connected()) {
        return 0;
    }
    wifi_ap_record_t ap = { 0 };
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

const char *wifi_mgr_ssid(void)
{
    return s_ssid;
}

esp_err_t wifi_mgr_scan(void **out, uint16_t *count)
{
    if (!s_started || out == NULL || count == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = NULL;
    *count = 0;

    /* connected 状态下 STA 会一直占着信道和状态机，scan 要么被拒、要么秒回空表。
     * 先关闸门再断开，等状态机落停后扫描，扫完恢复重连。 */
    s_scanning = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));

    wifi_scan_config_t sc = { 0 };
    sc.show_hidden = false;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    /* 显式给每个信道留够驻留时间，否则在 connected 场景下会秒回空结果 */
    sc.scan_time.active.min = 200;
    sc.scan_time.active.max = 400;

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        ret = esp_wifi_scan_start(&sc, true); /* 阻塞扫描，全信道约 2~5 秒 */
        if (ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "扫描第 %d 次被拒: %s", attempt + 1, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "扫描启动失败: %s", esp_err_to_name(ret));
        s_scanning = false;
        esp_wifi_connect();
        return ret;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        s_scanning = false;
        esp_wifi_connect();
        return ESP_OK;
    }
    wifi_ap_record_t *list = calloc(n, sizeof(wifi_ap_record_t));
    if (list == NULL) {
        s_scanning = false;
        esp_wifi_connect();
        return ESP_ERR_NO_MEM;
    }
    ret = esp_wifi_scan_get_ap_records(&n, list);
    if (ret != ESP_OK) {
        free(list);
        s_scanning = false;
        esp_wifi_connect();
        return ret;
    }
    *out = list;
    *count = n;
    s_scanning = false;
    esp_wifi_connect(); /* 闸门关掉，恢复自动重连 */
    return ESP_OK;
}

esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *password, int timeout_ms)
{
    if (!s_started || ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password == NULL) {
        password = "";
    }
    if (timeout_ms <= 0) {
        timeout_ms = 15000;
    }

    /* 记住旧配置，连不上要回滚，否则设备彻底失联只能重烧 */
    char old_ssid[33] = { 0 };
    char old_pass[65] = { 0 };
    snprintf(old_ssid, sizeof(old_ssid), "%s", s_ssid);
    snprintf(old_pass, sizeof(old_pass), "%s", s_pass);
    bool had_old = strlen(old_ssid) > 0;

    esp_err_t ret = save_creds(ssid, password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "写入 NVS 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", password);

    ESP_LOGI(TAG, "切换 WiFi -> %s，开始连接", ssid);
    xEventGroupClearBits(s_events, BIT_CONNECTED);
    /* 关键：配网路径必须把扫描闸门清零，否则后面的自动重连会被挡住 */
    s_scanning = false;
    apply_sta(ssid, password);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_wifi_connect();

    if (wifi_mgr_wait_connected(timeout_ms)) {
        ESP_LOGI(TAG, "切换成功，IP %s", s_ip);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "新 WiFi %d 秒内未连上，回滚到 %s", timeout_ms / 1000, old_ssid);
    if (!had_old) {
        return ESP_FAIL;
    }
    save_creds(old_ssid, old_pass);
    snprintf(s_ssid, sizeof(s_ssid), "%s", old_ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", old_pass);
    xEventGroupClearBits(s_events, BIT_CONNECTED);
    apply_sta(old_ssid, old_pass);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_wifi_connect();
    if (wifi_mgr_wait_connected(20000)) {
        ESP_LOGI(TAG, "已回滚，IP %s", s_ip);
        return ESP_FAIL; /* 回滚成功但切换失败，回报失败原因 */
    }
    ESP_LOGE(TAG, "回滚后仍未连上，设备当前离线");
    return ESP_FAIL;
}
