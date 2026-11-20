#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* 启动 WebSocket 客户端，内部自动重连 */
esp_err_t ws_client_start(void);

/* 换网关地址并立即重连（device 端会先写 NVS，再调这个）。
 * WS 还没起来时返回 ESP_ERR_INVALID_STATE，但地址已落盘，下次 start 生效 */
esp_err_t ws_client_set_uri(const char *url);

/* 是否已连上网关 */
bool ws_client_connected(void);

/* 处理一条下行 JSON。WiFi 的 WS 和 USB 串口两条通道共用同一套分发逻辑 */
void ws_handle_json(const char *buf);

/* 上报自身状态（固件/芯片/IP/SSID/信号）。WiFi 没连上时 IP 为 0.0.0.0，
 * 但走 USB 一样能让控制台看到设备在 */
void ws_send_hello(void);

/* 发送原始 JSON 文本。WS 不通时自动改走 USB 串口 */
esp_err_t ws_send_json(const char *json);

/* 发送 {"type":<type>} */
esp_err_t ws_send_type(const char *type);

/* 发送 {"type":<type>,"text":<text>} */
esp_err_t ws_send_text(const char *type, const char *text);

/* 发送带字符串字段的 {"type":<type>,<key>:<value>} */
esp_err_t ws_send_kv(const char *type, const char *key, const char *value);

/* 发送 {"type":"key","idx":N,"action":"down|up"} */
esp_err_t ws_send_key(int idx, const char *action);

/* 发送二进制帧：录音 PCM 上行（16bit 单声道裸数据） */
esp_err_t ws_send_bin(const void *data, size_t len);
