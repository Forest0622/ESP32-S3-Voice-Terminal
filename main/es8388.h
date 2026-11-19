#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 7 位 I2C 地址；esp_codec_dev 里写的是 8 位形式 0x20 */
#define ES8388_ADDR 0x10

esp_err_t es8388_write_reg(uint8_t reg, uint8_t val);
esp_err_t es8388_read_reg(uint8_t reg, uint8_t *val);

/* 读一次寄存器，能在 0x10 上应答即认为在线 */
bool es8388_probe(void);

/*
 * 完整初始化：走官方 esp_codec_dev 的 ES8388 驱动（与小智 AI 同一实现路径）
 * 内部会创建 ctrl/data/gpio 接口并 open 输出设备，必须在 audio_init() 之后调用
 */
esp_err_t es8388_init(void);

/* codec 是否已成功打开 */
bool es8388_is_ready(void);

/* 通过 esp_codec_dev 写一段已交织好的左右声道 PCM，samples 为每声道采样数 */
esp_err_t es8388_write_pcm(const int16_t *data, int samples);

/* 上电/下电 ADC（录音通路） */
esp_err_t es8388_adc_enable(bool on);

/* ADC 数字音量，db 取值 0 ~ -96，0 为最大 */
esp_err_t es8388_set_adc_volume(int db);

/* 打印 DAC 相关寄存器实际值，用来确认配置真的写进去了 */
void es8388_dump(void);

#ifdef __cplusplus
}
#endif
