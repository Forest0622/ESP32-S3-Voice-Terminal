#include "usb_link.h"

#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ws_client.h"

static const char *TAG = "USBLINK";

#define LINE_MAX 1100
#define PREFIX "@@"

static volatile bool s_ready;

/* 逐字节拼行：上位机可能把一条 JSON 拆成多个 USB 包发过来，
 * 也可能一次塞好几条，所以按 \n 断句，不按包边界断。 */
static void usb_rx_task(void *arg)
{
    (void)arg;
    char line[LINE_MAX];
    int len = 0;
    uint8_t buf[256];

    while (1) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (len >= 3) {
                    line[len] = '\0';
                    if (line[0] == PREFIX[0] && line[1] == PREFIX[1]) {
                        ESP_LOGI(TAG, "收到上行指令 %d 字节", len);
                        ws_handle_json(line + 2);
                    }
                }
                len = 0;
                continue;
            }
            /* 非前缀开头的内容（可能是 console 回显/日志残留）直接当成噪声，
             * 只要出现非法字符就重置，避免把垃圾拼成一条长行 */
            if (len == 0 && c != PREFIX[0]) {
                continue;
            }
            if (len == 1 && c != PREFIX[1]) {
                len = 0;
                continue;
            }
            if (len < LINE_MAX - 1) {
                line[len++] = c;
            } else {
                len = 0;
            }
        }
    }
}

esp_err_t usb_link_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 2048,
    };
    esp_err_t ret = usb_serial_jtag_driver_install(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_ready = true;
    xTaskCreate(usb_rx_task, "usb_rx", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "USB 数据通道就绪（@@ 前缀 JSON）");
    return ESP_OK;
}

bool usb_link_ready(void)
{
    return s_ready;
}

esp_err_t usb_link_send(const char *json)
{
    if (!s_ready || json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char buf[LINE_MAX + 8];
    int n = snprintf(buf, sizeof(buf), PREFIX "%s\n", json);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        return ESP_FAIL;
    }

    int w = usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(300));
    if (w == n) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "USB 发送失败 %d/%d", w, n);
    return ESP_FAIL;
}
