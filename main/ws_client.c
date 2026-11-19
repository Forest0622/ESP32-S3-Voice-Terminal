#include "ws_client.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "audio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"
#include "protocol.h"
#include "sdkconfig.h"
#include "ui.h"

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

static void handle_json(const char *buf)
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
    } else {
        ESP_LOGD(TAG, "忽略下行 type=%s", type);
    }

    cJSON_Delete(root);
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
            handle_json(s_rx);
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
    char mac[24] = { 0 };
    uint8_t m[6] = { 0 };
    if (esp_read_mac(m, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3],
                 m[4], m[5]);
    }

    esp_websocket_client_config_t cfg = {
        .uri = CONFIG_VOICE_SERVER_URL,
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

    ESP_LOGI(TAG, "连接 %s", CONFIG_VOICE_SERVER_URL);

    char hello[160];
    snprintf(hello, sizeof(hello),
             "{\"type\":\"hello\",\"fw\":\"%s\",\"chip\":\"%s\"}", VOICE_FW_VERSION, mac);
    /* 连上之前发送会失败，交给事件回调触发后补发；这里尝试一次，失败不致命 */
    ws_send_json(hello);
    return ESP_OK;
}

bool ws_client_connected(void)
{
    return s_connected;
}

esp_err_t ws_send_json(const char *json)
{
    if (s_client == NULL || !s_connected || json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int len = (int)strlen(json);
    return esp_websocket_client_send_text(s_client, json, len, pdMS_TO_TICKS(SEND_TIMEOUT)) >= 0
               ? ESP_OK
               : ESP_FAIL;
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
