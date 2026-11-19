#include "wifi_mgr.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "WIFI";

#define BIT_CONNECTED BIT0

static EventGroupHandle_t s_events;
static char s_ip[16] = "0.0.0.0";
static bool s_started;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        strcpy(s_ip, "0.0.0.0");
        ESP_LOGW(TAG, "断开，3 秒后重连");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        ESP_LOGI(TAG, "拿到 IP: %s", s_ip);
    }
}

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

    wifi_config_t wcfg = { 0 };
    const char *ssid = CONFIG_VOICE_WIFI_SSID;
    const char *pass = CONFIG_VOICE_WIFI_PASSWORD;
    if (strlen(ssid) == 0) {
        ESP_LOGE(TAG, "未配置 WiFi SSID，请 idf.py menuconfig 填 Voice Terminal 项");
        return ESP_ERR_INVALID_STATE;
    }
    snprintf((char *)wcfg.sta.ssid, sizeof(wcfg.sta.ssid), "%s", ssid);
    snprintf((char *)wcfg.sta.password, sizeof(wcfg.sta.password), "%s", pass);
    wcfg.sta.threshold.authmode = strlen(pass) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_started = true;
    ESP_LOGI(TAG, "STA 启动，目标 SSID: %s", ssid);
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
