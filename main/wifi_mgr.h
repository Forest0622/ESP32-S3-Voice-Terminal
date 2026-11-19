#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 初始化 WiFi（STA）并发起连接，非阻塞。
 * 凭据优先取 NVS（可在控制台在线切换），没有则回退到 menuconfig 的编译期默认值。 */
esp_err_t wifi_mgr_start(void);

/* 是否已拿到 IP */
bool wifi_mgr_connected(void);

/* 阻塞等待拿到 IP，超时返回 false */
bool wifi_mgr_wait_connected(int timeout_ms);

/* 当前 IP 字符串，未连接返回 "0.0.0.0" */
const char *wifi_mgr_ip(void);

/* RSSI，未连接返回 0 */
int wifi_mgr_rssi(void);

/* 当前 SSID，未连接返回空串 */
const char *wifi_mgr_ssid(void);

/* 扫描周边热点。*out 是 wifi_ap_record_t 数组，由本函数分配，调用者负责 free；
 * *count 为数量。会阻塞 2~5 秒，不要在 UI 任务里调用。 */
esp_err_t wifi_mgr_scan(void **out, uint16_t *count);

/* 在线切换 WiFi：写入 NVS 后立即重连。
 * 若新配置在 timeout_ms 内连不上，自动回滚到旧配置，避免设备失联。 */
esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *password, int timeout_ms);

/* 网关地址（ws://<mac>:8765/ws），优先级 NVS > menuconfig。
 * 出厂无配置时可由控制台通过 USB 的 wifi.set 一起下发，否则配好网也连不上 WS。 */
const char *wifi_mgr_server(void);

/* 写入网关地址并持久化 */
esp_err_t wifi_mgr_set_server(const char *url);
