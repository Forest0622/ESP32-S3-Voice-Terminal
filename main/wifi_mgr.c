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
static bool s_started;

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

/* ---------------- 事件 ---------------- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        strcpy(s_ip, "0.0.0.0");
        ESP_LOGW(TAG, "断开，重连中");
        esp_wifi_connect();
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
    if (strlen(s_ssid) == 0) {
        ESP_LOGE(TAG, "未配置 WiFi SSID：可用控制台切换，或在 menuconfig 填 Voice Terminal 项");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(apply_sta(s_ssid, s_pass));
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

    wifi_scan_config_t sc = { 0 };
    sc.show_hidden = false;
    esp_err_t ret = esp_wifi_scan_start(&sc, true); /* 阻塞扫描，约 2~5 秒 */
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "扫描启动失败: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        return ESP_OK;
    }
    wifi_ap_record_t *list = calloc(n, sizeof(wifi_ap_record_t));
    if (list == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ret = esp_wifi_scan_get_ap_records(&n, list);
    if (ret != ESP_OK) {
        free(list);
        return ret;
    }
    *out = list;
    *count = n;
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
