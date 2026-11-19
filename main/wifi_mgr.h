#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* 初始化 WiFi（STA）并发起连接，非阻塞 */
esp_err_t wifi_mgr_start(void);

/* 是否已拿到 IP */
bool wifi_mgr_connected(void);

/* 阻塞等待拿到 IP，超时返回 false */
bool wifi_mgr_wait_connected(int timeout_ms);

/* 当前 IP 字符串，未连接返回 "0.0.0.0" */
const char *wifi_mgr_ip(void);

/* RSSI，未连接返回 0 */
int wifi_mgr_rssi(void);
