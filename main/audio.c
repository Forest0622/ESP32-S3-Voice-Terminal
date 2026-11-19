#include "audio.h"
#include "es8388.h"

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "AUDIO";

#define PIN_MCLK 3
#define PIN_BCLK 46
#define PIN_LRCK 9
#define PIN_DOUT 10  /* ESP32 -> ES8388 DSDIN（官方引脚表：IO10 = I2S_SDIN） */
#define PIN_DIN  14  /* ES8388 ASDOUT -> ESP32（官方引脚表：IO14 = I2S_SDOUT） */

#define SAMPLE_RATE 16000
#define MAX_FRAMES  2048
/* 板载 MIC 实测偏低 20dB 以上，PGA 已拉满，这里再补 +18dB 软件增益 */
#define PCM_GAIN 5
/* 一阶高通系数，截止约 100Hz，用来去掉上电时的 DC 偏置 */
#define HP_ALPHA 0.96f

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static int16_t *s_stereo; /* 原始左右交织缓冲，放 PSRAM */
static int16_t *s_play;   /* 播放用的左右交织缓冲，放 PSRAM */
static bool s_enabled;
static float s_hp_x1;
static float s_hp_y1;

esp_err_t audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 512;
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        /* ES8388 始终输出左右两个 slot；开 STEREO 才能读到完整帧，再在 read 里混成单声道 */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_MCLK,
            .bclk = PIN_BCLK,
            .ws = PIN_LRCK,
            .dout = PIN_DOUT,
            .din = PIN_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode(tx): %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode(rx): %s", esp_err_to_name(ret));
        return ret;
    }

    s_stereo = heap_caps_malloc(MAX_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_play = heap_caps_malloc(MAX_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_stereo == NULL || s_play == NULL) {
        ESP_LOGE(TAG, "音频缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }

    i2s_channel_enable(s_tx);
    i2s_channel_enable(s_rx);
    s_enabled = true;   /* init 里已经开了 RX，状态必须同步，否则 audio_read_pcm 会一直认为没开 */

    ESP_LOGI(TAG, "I2S 就绪：%d Hz / 16bit / 收发共用时钟，MCLK=%d BCLK=%d LRCK=%d DIN=%d DOUT=%d",
             SAMPLE_RATE, PIN_MCLK, PIN_BCLK, PIN_LRCK, PIN_DIN, PIN_DOUT);
    return ESP_OK;
}

esp_err_t audio_get_handles(void **tx_handle, void **rx_handle)
{
    if (tx_handle == NULL || rx_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx == NULL || s_rx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *tx_handle = (void *)s_tx;
    *rx_handle = (void *)s_rx;
    return ESP_OK;
}

esp_err_t audio_enable(bool on)
{
    if (s_rx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on == s_enabled) {
        return ESP_OK; /* 幂等：已经是目标状态就不重复动硬件 */
    }
    esp_err_t ret = on ? i2s_channel_enable(s_rx) : i2s_channel_disable(s_rx);
    if (ret == ESP_OK) {
        s_enabled = on;
    }
    return ret;
}

int audio_read_pcm(int16_t *buf, int max_samples, int timeout_ms)
{
    if (s_rx == NULL || s_stereo == NULL || buf == NULL || max_samples <= 0) {
        return -1;
    }
    if (!s_enabled) {
        return -1;
    }
    if (max_samples > MAX_FRAMES) {
        max_samples = MAX_FRAMES;
    }

    size_t want = (size_t)max_samples * 2 * sizeof(int16_t);
    size_t got = 0;
    esp_err_t ret = i2s_channel_read(s_rx, s_stereo, want, &got, pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_read: %s", esp_err_to_name(ret));
        return -1;
    }

    int frames = (int)(got / (2 * sizeof(int16_t)));
    /* 实测左右近似反相，混平均会把语音抵消掉，只取左声道；
     * 再串一阶高通去 DC，最后加软件增益并限幅 */
    for (int i = 0; i < frames; i++) {
        float x = (float)s_stereo[i * 2];
        float y = HP_ALPHA * (s_hp_y1 + x - s_hp_x1);
        s_hp_x1 = x;
        s_hp_y1 = y;
        int32_t v = (int32_t)(y * PCM_GAIN);
        if (v > 32767) {
            v = 32767;
        }
        if (v < -32768) {
            v = -32768;
        }
        buf[i] = (int16_t)v;
    }
    return frames;
}

esp_err_t audio_play_mono(const int16_t *data, int samples, int timeout_ms)
{
    if (s_tx == NULL || s_play == NULL || data == NULL || samples <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples > MAX_FRAMES) {
        samples = MAX_FRAMES;
    }
    /* 单声道复制成左右交织，DAC 两个声道同时出声 */
    for (int i = 0; i < samples; i++) {
        s_play[i * 2] = data[i];
        s_play[i * 2 + 1] = data[i];
    }

    /* 优先走官方 esp_codec_dev 输出通路（与小智 AI 一致） */
    if (es8388_is_ready()) {
        esp_err_t ret = es8388_write_pcm(s_play, samples);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "codec_dev 写入失败，回退直写 I2S");
    }

    size_t written = 0;
    esp_err_t ret = i2s_channel_write(s_tx, s_play, (size_t)samples * 2 * sizeof(int16_t), &written,
                                      pdMS_TO_TICKS(timeout_ms));
    ESP_LOGI(TAG, "播放 %d 采样 → 写入 %u/%u 字节 ret=%s", samples, (unsigned)written,
             (unsigned)(samples * 2 * sizeof(int16_t)), esp_err_to_name(ret));
    return ret;
}
