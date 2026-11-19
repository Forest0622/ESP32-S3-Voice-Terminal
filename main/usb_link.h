#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * USB Serial/JTAG 数据通道。
 *
 * 定位：WiFi 之外的**第二条**控制通道。换网、断网、还没配好 WiFi 的时候，
 * 只要板子插在电脑上，控制台照样能下发指令（配网、音量、播报），板子也能把
 * 状态回传。日志走 UART0，与这里不冲突。
 *
 * 帧格式：以 `@@` 开头的单行 JSON，\n 结尾。`@@` 前缀是为了跟串口里混进来的
 * 调试输出区分，网关侧只认这个前缀。
 */

esp_err_t usb_link_init(void);

/** 通道是否已就绪（driver 装好且任务在跑） */
bool usb_link_ready(void);

/** 发送一条 JSON（自动加 @@ 前缀和换行），失败返回非 ESP_OK */
esp_err_t usb_link_send(const char *json);

#ifdef __cplusplus
}
#endif
