#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 初始化 I2S0 接收通道：16kHz / 16bit / 单声道，ESP32-S3 作主机
 * MCLK=GPIO3  BCLK=GPIO46  LRCK=GPIO9  DIN=GPIO14（ES8388 -> ESP32）
 * 需在 es8388_init() 之后调用。
 */
esp_err_t audio_init(void);

/*
 * 取出 I2S 收发通道句柄，供 esp_codec_dev 的数据接口接管
 * （es8388_init 内部走官方 esp_codec_dev，必须先 audio_init）
 */
esp_err_t audio_get_handles(void **tx_handle, void **rx_handle);

/* 打开/关闭接收通道 */
esp_err_t audio_enable(bool on);

/* 读一段 PCM。返回采样点数（int16），失败返回 -1 */
int audio_read_pcm(int16_t *buf, int max_samples, int timeout_ms);

/* 播放单声道 PCM：内部复制成左右两份写进 I2S TX */
esp_err_t audio_play_mono(const int16_t *data, int samples, int timeout_ms);

#ifdef __cplusplus
}
#endif
