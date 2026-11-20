#include "ws_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "audio.h"
#include "es8388.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol.h"
#include "sdkconfig.h"
#include "ui.h"
#include "usb_link.h"
#include "wifi_mgr.h"

static const char *TAG = "WS";

#define RX_MAX        2048
#define AUDIO_MAX     2048
#define SEND_TIMEOUT  2000

static esp_websocket_client_handle_t s_client;
static volatile bool s_connected;
static char s_rx[RX_MAX];
static int s_rx_len;
static int16_t s_audio[AUDIO_MAX / 2];
static int s_audio_len;

static const char *state_text_of(const char *value)
{
    if (value == NULL) {
        return ST_IDLE;
    }
    if (strcmp(value, ST_LISTENING) == 0 || strcmp(value, ST_THINKING) == 0 ||
        strcmp(value, ST_SPEAKING) == 0 || strcmp(value, ST_IDLE) == 0) {
        return value;
    }
    return ST_IDLE;
}

/* ---- 控制台下发的设备管理指令 ----
 * 扫描和切换 WiFi 都会阻塞数秒甚至二十秒，必须放独立任务，不能卡住 WS 收发 */

typedef struct {
    char ssid[33];
    char pass[65];
    char server[96];   /* 可选的网关地址，出厂无配置时由控制台一起下发 */
} wifi_req_t;

static void wifi_scan_task(void *arg)
{
    (void)arg;
    void *list = NULL;
    uint16_t n = 0;
    esp_err_t ret = wifi_mgr_scan(&list, &n);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "wifi.list");
    cJSON_AddBoolToObject(root, "ok", ret == ESP_OK);
    if (ret != ESP_OK) {
        /* 把错误码带回控制台：现场没有 UART 时这是唯一能看到失败原因的地方 */
        cJSON_AddNumberToObject(root, "err", (int)ret);
        cJSON_AddStringToObject(root, "errname", esp_err_to_name(ret));
    }
    cJSON_AddStringToObject(root, "current", wifi_mgr_ssid());
    cJSON_AddStringToObject(root, "ip", wifi_mgr_ip());
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    wifi_ap_record_t *ap = (wifi_ap_record_t *)list;
    if (list != NULL) {
        /* 同一个 SSID 往往有多个 AP（同网多 BSSID），合并成一条并保留最强信号，
         * 否则控制台列表里会出现十几条同名项，没法选 */
        uint16_t added = 0;
        for (uint16_t i = 0; i < n; i++) {
            if (ap[i].ssid[0] == 0) {
                continue;
            }
            int found = -1;
            for (uint16_t j = 0; j < added; j++) {
                if (strcmp((const char *)ap[i].ssid, (const char *)ap[j].ssid) == 0) {
                    found = j;
                    break;
                }
            }
            if (found >= 0) {
                if (ap[i].rssi > ap[found].rssi) {
                    ap[found] = ap[i];
                }
            } else {
                ap[added++] = ap[i];
            }
        }
        n = added;
    }
    for (uint16_t i = 0; list != NULL && i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)ap[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", ap[i].rssi);
        cJSON_AddNumberToObject(o, "auth", ap[i].authmode);
        cJSON_AddItemToArray(arr, o);
    }
    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        ws_send_json(json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
    free(list);
    vTaskDelete(NULL);
}

static void wifi_set_task(void *arg)
{
    wifi_req_t *req = (wifi_req_t *)arg;

    /* 先把网关地址落盘：出厂状态下 CONFIG_VOICE_SERVER_URL 是空的，
     * 不写这一步就算 WiFi 连上了，ws_client_start 也会因为空 URI 起不来。 */
    if (strlen(req->server) > 0) {
        esp_err_t sret = wifi_mgr_set_server(req->server);
        if (sret != ESP_OK) {
            ESP_LOGW(TAG, "写入网关地址失败: %s", esp_err_to_name(sret));
        }
    }

    /* 45 秒而不是 20 秒：B4-101 这类路由器给 IP 经常要 25~40 秒，
     * 20 秒会把"其实能连上"的网判成失败并回滚 */
    esp_err_t ret = wifi_mgr_set_credentials(req->ssid, req->pass, 45000);

    /* 首次配网成功时 link_task 可能还卡在 wait_connected 上，
     * WS 还没起来；这里主动补一次启动，出厂态下才会用到（重复调用是幂等的）。 */
    if (ret == ESP_OK) {
        ws_client_start();
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "wifi.result");
    cJSON_AddBoolToObject(root, "ok", ret == ESP_OK);
    cJSON_AddStringToObject(root, "ssid", wifi_mgr_ssid());
    cJSON_AddStringToObject(root, "ip", wifi_mgr_ip());
    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        /* 切换过程中 WS 会断开，这条多半发不出去；重连后 hello 会带上新的 IP */
        ws_send_json(json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
    free(req);
    vTaskDelete(NULL);
}

/* 下行 JSON 分发入口。WS 和 USB 两条通道都调这里，避免同一套指令写两遍 */
void ws_handle_json(const char *buf)
{
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "收到非法 JSON: %.80s", buf);
        return;
    }

    cJSON *jtype = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(jtype)) {
        cJSON_Delete(root);
        return;
    }
    const char *type = jtype->valuestring;

    if (strcmp(type, MSG_SAY) == 0) {
        cJSON *jtext = cJSON_GetObjectItem(root, "text");
        cJSON *jfinal = cJSON_GetObjectItem(root, "final");
        bool final = !cJSON_IsBool(jfinal) || cJSON_IsTrue(jfinal);
        if (cJSON_IsString(jtext)) {
            ui_append_stream(jtext->valuestring, final);
        }
    } else if (strcmp(type, MSG_STATE) == 0) {
        cJSON *jvalue = cJSON_GetObjectItem(root, "value");
        ui_set_state(state_text_of(cJSON_IsString(jvalue) ? jvalue->valuestring : NULL));
    } else if (strcmp(type, MSG_CLEAR) == 0) {
        ui_clear();
    } else if (strcmp(type, MSG_WIFI_SCAN) == 0) {
        ESP_LOGI(TAG, "收到 wifi.scan，开始扫描");
        xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL);
    } else if (strcmp(type, MSG_WIFI_SET) == 0) {
        cJSON *jssid = cJSON_GetObjectItem(root, "ssid");
        cJSON *jpass = cJSON_GetObjectItem(root, "password");
        if (!cJSON_IsString(jssid) || strlen(jssid->valuestring) == 0) {
            ESP_LOGW(TAG, "wifi.set 缺少 ssid");
        } else {
            wifi_req_t *req = calloc(1, sizeof(wifi_req_t));
            if (req == NULL) {
                ESP_LOGE(TAG, "wifi.set 申请内存失败");
            } else {
                snprintf(req->ssid, sizeof(req->ssid), "%s", jssid->valuestring);
                snprintf(req->pass, sizeof(req->pass), "%s",
                         cJSON_IsString(jpass) ? jpass->valuestring : "");
                cJSON *jsrv = cJSON_GetObjectItem(root, "server");
                if (cJSON_IsString(jsrv)) {
                    snprintf(req->server, sizeof(req->server), "%s", jsrv->valuestring);
                }
                ESP_LOGI(TAG, "收到 wifi.set -> %s（server=%s）", req->ssid,
                         strlen(req->server) > 0 ? req->server : "未指定");
                xTaskCreate(wifi_set_task, "wifi_set", 4096, req, 5, NULL);
            }
        }
    } else if (strcmp(type, MSG_SERVER_SET) == 0) {
        cJSON *jurl = cJSON_GetObjectItem(root, "url");
        if (!cJSON_IsString(jurl) || strlen(jurl->valuestring) == 0) {
            ESP_LOGW(TAG, "server.set 缺少 url");
        } else {
            esp_err_t sret = wifi_mgr_set_server(jurl->valuestring);
            if (sret == ESP_OK) {
                ws_client_set_uri(jurl->valuestring);
            } else {
                ESP_LOGW(TAG, "写入网关地址失败: %s", esp_err_to_name(sret));
            }
        }
    } else if (strcmp(type, MSG_VOLUME) == 0) {
        cJSON *jval = cJSON_GetObjectItem(root, "value");
        if (cJSON_IsNumber(jval)) {
            int v = jval->valueint;
            if (v < 0) {
                v = 0;
            }
            if (v > 33) {
                v = 33;
            }
            es8388_set_output_volume((uint8_t)v);
            ESP_LOGI(TAG, "喇叭音量设为 %d", v);
        }
    } else {
        ESP_LOGD(TAG, "忽略下行 type=%s", type);
    }

    cJSON_Delete(root);
}

/* 连上之后上报自身状态：固件、IP、SSID、信号、运行时长。
 * 不能直接在事件回调里调 send，容易和 WS 内部锁打架，所以放独立任务。 */
void ws_send_hello(void)
{
    char mac[24] = { 0 };
    uint8_t m[6] = { 0 };
    if (esp_read_mac(m, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
    }

    char hello[256];
    snprintf(hello, sizeof(hello),
             "{\"type\":\"hello\",\"fw\":\"%s\",\"chip\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"ssid\":\"%s\",\"uptime\":%lu}",
             VOICE_FW_VERSION, mac, wifi_mgr_ip(), wifi_mgr_rssi(), wifi_mgr_ssid(),
             (unsigned long)(esp_timer_get_time() / 1000000LL));
    ws_send_json(hello);
    ESP_LOGI(TAG, "上报 hello: ip=%s ssid=%s rssi=%d", wifi_mgr_ip(), wifi_mgr_ssid(),
             wifi_mgr_rssi());
}

static void hello_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(150));
    ws_send_hello();
    vTaskDelete(NULL);
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        s_rx_len = 0;
        ESP_LOGI(TAG, "已连上网关");
        ui_show_message("sys", "已连接网关");
        xTaskCreate(hello_task, "hello", 3072, NULL, 4, NULL);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        s_connected = false;
        s_rx_len = 0;
        ESP_LOGW(TAG, "网关断开");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (ev == NULL || ev->data_ptr == NULL || ev->data_len <= 0) {
            break;
        }
        ESP_LOGI(TAG, "数据事件 op=0x%02X len=%d off=%d plen=%d", ev->op_code, ev->data_len,
                 ev->payload_offset, ev->payload_len);
        /* 二进制帧 = 下行 TTS 音频，按 payload 边界拼好后直接送 I2S。
         * 注意 op_code 带 FIN 位（0x82），必须掩掉低 4 位再比较 */
        if ((ev->op_code & 0x0F) == 0x02) {
            if (ev->payload_offset == 0) {
                s_audio_len = 0;
            }
            if (s_audio_len + ev->data_len <= AUDIO_MAX) {
                memcpy((uint8_t *)s_audio + s_audio_len, ev->data_ptr, ev->data_len);
                s_audio_len += ev->data_len;
            } else {
                ESP_LOGW(TAG, "音频帧超过 %d 字节，丢弃", AUDIO_MAX);
                s_audio_len = 0;
                break;
            }
            if (s_audio_len >= ev->payload_len) {
                int samples = s_audio_len / (int)sizeof(int16_t);
                esp_err_t r = audio_play_mono(s_audio, samples, 2000);
                if (r != ESP_OK) {
                    ESP_LOGW(TAG, "播放失败 %d 采样: %s", samples, esp_err_to_name(r));
                } else {
                    ESP_LOGI(TAG, "播放 %d 采样", samples);
                }
                s_audio_len = 0;
            }
            break;
        }
        /* 文本帧：分片重组 */
        if (ev->payload_offset == 0) {
            s_rx_len = 0;
        }
        if (s_rx_len + ev->data_len < RX_MAX) {
            memcpy(s_rx + s_rx_len, ev->data_ptr, ev->data_len);
            s_rx_len += ev->data_len;
            s_rx[s_rx_len] = '\0';
        } else {
            ESP_LOGW(TAG, "下行报文超过 %d 字节，丢弃", RX_MAX);
            s_rx_len = 0;
            break;
        }
        if (s_rx_len >= ev->payload_len && (ev->op_code & 0x0F) == 0x01) {
            ws_handle_json(s_rx);
            s_rx_len = 0;
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WebSocket 错误，自动重连中");
        break;

    default:
        break;
    }
}

esp_err_t ws_client_start(void)
{
    const char *uri = wifi_mgr_server();
    if (uri == NULL || strlen(uri) == 0) {
        ESP_LOGW(TAG, "网关地址为空，跳过 WS 启动（出厂态请先用控制台配网并下发 server）");
        return ESP_ERR_INVALID_ARG;
    }

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .buffer_size = RX_MAX,
        .task_stack = 6144,
        .disable_auto_reconnect = false,
    };

    if (s_client == NULL) {
        s_client = esp_websocket_client_init(&cfg);
        if (s_client == NULL) {
            ESP_LOGE(TAG, "websocket init 失败");
            return ESP_FAIL;
        }
        esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    }

    esp_err_t ret = esp_websocket_client_start(s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "websocket start 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "连接 %s", uri);
    /* hello 由 CONNECTED 事件里的 hello_task 统一上报，此处不重复发 */
    return ESP_OK;
}

esp_err_t ws_client_set_uri(const char *url)
{
    if (url == NULL || strlen(url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client == NULL) {
        /* WS 还没起来（例如出厂态刚配完网），NVS 已经写好，等 ws_client_start 用新地址 */
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_websocket_client_set_uri(s_client, url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置 URI 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_websocket_client_stop(s_client);   /* 已停止时会返回错误，忽略 */
    vTaskDelay(pdMS_TO_TICKS(400));
    ret = esp_websocket_client_start(s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "用新地址重启 WS 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "WS 已切到 %s", url);
    return ESP_OK;
}

bool ws_client_connected(void)
{
    return s_connected;
}

esp_err_t ws_send_json(const char *json)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client != NULL && s_connected) {
        int len = (int)strlen(json);
        if (esp_websocket_client_send_text(s_client, json, len, pdMS_TO_TICKS(SEND_TIMEOUT)) >=
            0) {
            return ESP_OK;
        }
    }
    /* WiFi 通道不通（换网、刚断线、还没配好 WiFi）时改走 USB，
     * 保证板子插在电脑上时控制台照样能指挥 */
    return usb_link_send(json);
}

esp_err_t ws_send_type(const char *type)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\"}", type);
    return ws_send_json(buf);
}

esp_err_t ws_send_text(const char *type, const char *text)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"text\":\"%s\"}", type, text ? text : "");
    return ws_send_json(buf);
}

esp_err_t ws_send_kv(const char *type, const char *key, const char *value)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"%s\":\"%s\"}", type, key, value ? value : "");
    return ws_send_json(buf);
}

esp_err_t ws_send_key(int idx, const char *action)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"type\":\"key\",\"idx\":%d,\"action\":\"%s\"}", idx,
             action ? action : "down");
    return ws_send_json(buf);
}

esp_err_t ws_send_bin(const void *data, size_t len)
{
    if (s_client == NULL || !s_connected || data == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    int ret = esp_websocket_client_send_bin(s_client, (const char *)data, (int)len,
                                            pdMS_TO_TICKS(SEND_TIMEOUT));
    return ret >= 0 ? ESP_OK : ESP_FAIL;
}
