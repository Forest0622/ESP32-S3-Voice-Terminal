#include <stdio.h>
#include <string.h>
#include <math.h>

#include "audio.h"
#include "board.h"
#include "es8388.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "protocol.h"
#include "ui.h"
#include "usb_link.h"
#include "wifi_mgr.h"
#include "ws_client.h"

static const char *TAG = "MAIN";

/* KEY0 按住期间为 true，audio_task 据此循环取 PCM 上传 */
static volatile bool s_recording;

/* ---------------- 音频通路自测：探测 ES8388 → 配置 I2S → 录音 0.5 秒算电平 ---------------- */

static void audio_selftest(void)
{
    if (!es8388_probe()) {
        ESP_LOGE(TAG, "ES8388 无应答（I2C 0x10）");
        ui_show_message("sys", "音频芯片未应答");
        return;
    }
    ESP_LOGI(TAG, "ES8388 在线（I2C 0x10）");

    if (audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2S 初始化失败");
        return;
    }
    if (es8388_init() != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 初始化失败");
        ui_show_message("sys", "音频芯片初始化失败");
        return;
    }
    if (audio_enable(true) != ESP_OK) {
        ESP_LOGE(TAG, "I2S 使能失败");
        return;
    }
    es8388_adc_enable(true);
    vTaskDelay(pdMS_TO_TICKS(300));

    int16_t *buf = heap_caps_malloc(2048 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "PSRAM 分配失败");
    } else {
        /* 连续采 5 秒，每 0.25 秒打一行，用来判断 ADC 是一直在采还是只有开头一下 */
        for (int round = 0; round < 10; round++) {
            int n = audio_read_pcm(buf, 2048, 500);
            if (n > 0) {
                int32_t peak = 0;
                int64_t sum_sq = 0;
                for (int i = 0; i < n; i++) {
                    int32_t v = buf[i];
                    int32_t a = v < 0 ? -v : v;
                    if (a > peak) {
                        peak = a;
                    }
                    sum_sq += (int64_t)v * v;
                }
                int rms = (int)sqrt((double)(sum_sq / n));
                ESP_LOGI(TAG, "自测#%d n=%d peak=%ld rms=%d 首4=%d,%d,%d,%d", round, n,
                         (long)peak, rms, buf[0], buf[1], buf[2], buf[3]);
            } else {
                ESP_LOGW(TAG, "自测#%d 读取失败", round);
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        free(buf);
    }

    es8388_adc_enable(false);
    es8388_dump();
    /* I2S 保持开启：MCLK 一旦中断，ES8388 的 ADC 要重新起状态机很容易起不来 */
}

/* ---------------- 录音上行：按住 KEY0 期间，每 0.1 秒把一帧 PCM 推到网关 ---------------- */

static void audio_task(void *arg)
{
    (void)arg;
    const int CHUNK = 1600; /* 16000Hz × 0.1s */
    int16_t *buf = heap_caps_malloc(CHUNK * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "录音缓冲区分配失败");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (s_recording) {
            int n = audio_read_pcm(buf, CHUNK, 300);
            if (n > 0) {
                ws_send_bin(buf, (size_t)n * sizeof(int16_t));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
}

/* ---------------- 按键：KEY0 说话 / KEY1 打断 / KEY2 清屏 / KEY3 重连 ---------------- */

static void key_task(void *arg)
{
    (void)arg;
    uint16_t prev = board_keys_raw();
    vTaskDelay(pdMS_TO_TICKS(100));

    while (1) {
        uint16_t now = board_keys_raw();
        uint16_t down = (uint16_t)(prev & ~now);
        uint16_t up = (uint16_t)(~prev & now);

        if (down & EXIO_KEY0_IO) {
            ESP_LOGI(TAG, "KEY0 按下：开始录音");
            ws_send_key(KEY_IDX_0, "down");
            ui_set_state(ST_LISTENING);
            /* MCLK 全程不断，只切 ADC 电源 */
            es8388_init();
            es8388_adc_enable(true);
            vTaskDelay(pdMS_TO_TICKS(150));
            /* 清掉 DMA 里积压的旧数据 */
            int16_t drop[256];
            int guard = 0;
            while (audio_read_pcm(drop, 256, 20) > 0 && guard++ < 96) {
            }
            s_recording = true;
            ws_send_type("listen.start");
            uint8_t pga = 0, pwr = 0, ctl21 = 0;
            es8388_read_reg(0x09, &pga);
            es8388_read_reg(0x03, &pwr);
            es8388_read_reg(0x2B, &ctl21);
            ESP_LOGI(TAG, "ES8388 状态 PGA=0x%02X ADCPOWER=0x%02X DACCONTROL21=0x%02X",
                     pga, pwr, ctl21);
        }
        if (up & EXIO_KEY0_IO) {
            ESP_LOGI(TAG, "KEY0 松开：停止录音");
            s_recording = false;
            ws_send_key(KEY_IDX_0, "up");
            ws_send_type("listen.stop");
            es8388_adc_enable(false);
            ui_set_state(ST_IDLE);
        }
        if (down & EXIO_KEY1_IO) {
            ESP_LOGI(TAG, "KEY1 按下：打断");
            ws_send_key(KEY_IDX_1, "down");
            ws_send_type("abort");
            ui_set_state(ST_IDLE);
        }
        if (down & EXIO_KEY2_IO) {
            ESP_LOGI(TAG, "KEY2 按下：清屏");
            ws_send_key(KEY_IDX_2, "down");
            ui_clear();
        }
        if (down & EXIO_KEY3_IO) {
            ESP_LOGI(TAG, "KEY3 按下：重发 hello");
            ws_send_key(KEY_IDX_3, "down");
            ws_send_text("text", "[KEY3] 本地测试消息");
        }

        prev = now;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* ---------------- 连接管理：WiFi → WebSocket ---------------- */

static void link_task(void *arg)
{
    (void)arg;

    if (wifi_mgr_start() != ESP_OK) {
        ui_show_message("sys", "WiFi 未配置，请改 menuconfig");
    }

    int waited = 0;
    while (!wifi_mgr_wait_connected(1000)) {
        waited++;
        /* 连不上网的这段时间，每 10 秒往 USB 报一次自身状态。
         * 开机那一刻网关往往还没起来，hello 会丢；周期性重报才能让控制台一直看得见设备。 */
        if (waited % 10 == 0) {
            ws_send_hello();
        }
        if (waited == 5) {
            ui_show_message("sys", "WiFi 连接中…");
        }
        if (waited > 60) {
            ui_show_message("sys", "WiFi 连接超时，检查 SSID/密码");
            waited = 0;
        }
    }

    ui_set_conn(true, wifi_mgr_ip(), wifi_mgr_rssi());
    ui_show_message("sys", "WiFi 已连接");
    vTaskDelay(pdMS_TO_TICKS(2000));

    ws_client_start();

    bool last_ws = false;
    while (1) {
        bool ws = ws_client_connected();
        if (ws != last_ws) {
            last_ws = ws;
            ui_set_conn(wifi_mgr_connected(), wifi_mgr_ip(), wifi_mgr_rssi());
            ui_set_state(ST_IDLE);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(board_init());
    board_dump_info();
    board_dump_exio();

    /* USB 通道要在 UI 和网络之前起来：没网的时候这是唯一的控制入口 */
    usb_link_init();

    /* WiFi 栈提前初始化。原先只在 link_task 里初始化，而 link_task 排在
     * audio_selftest 之后，导致刚开机那几秒控制台一插 USB 就发指令时
     * wifi_mgr_scan 会因为 !s_started 直接失败。 */
    wifi_mgr_start();

    /* 开机先把自身信息报一次。此时 WiFi 往往还没连上（IP 是 0.0.0.0），
     * 但走 USB 也能让控制台立刻看到固件版本、芯片和配置的目标 SSID */
    ws_send_hello();

    ui_init();
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 5, NULL, 1);

    audio_selftest();

    xTaskCreate(link_task, "link", 6144, NULL, 4, NULL);
    xTaskCreate(key_task, "key", 4096, NULL, 3, NULL);
    xTaskCreate(audio_task, "audio", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "语音终端启动完成，固件 %s", VOICE_FW_VERSION);
    ui_show_message("sys", "语音终端 " VOICE_FW_VERSION " 启动");
}
