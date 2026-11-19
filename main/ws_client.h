#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* 启动 WebSocket 客户端，内部自动重连 */
esp_err_t ws_client_start(void);

/* 是否已连上网关 */
bool ws_client_connected(void);

/* 发送原始 JSON 文本 */
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
