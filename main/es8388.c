#include "es8388.h"

#include "audio.h"
#include "board.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "es8388_codec.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "ES8388";

#define REG_CONTROL1     0x00
#define REG_CONTROL2     0x01
#define REG_CHIPPOWER    0x02
#define REG_ADCPOWER     0x03
#define REG_DACPOWER     0x04

#define REG_MASTERMODE   0x08
#define REG_ADCCONTROL1  0x09
#define REG_ADCCONTROL3  0x0B
#define REG_ADCCONTROL8  0x10
#define REG_ADCCONTROL9  0x11

#define REG_DACCONTROL1  0x17
#define REG_DACCONTROL2  0x18
#define REG_DACCONTROL3  0x19
#define REG_DACCONTROL4  0x1A
#define REG_DACCONTROL5  0x1B
#define REG_DACCONTROL16 0x26
#define REG_DACCONTROL17 0x27
#define REG_DACCONTROL20 0x2A
#define REG_DACCONTROL21 0x2B
#define REG_DACCONTROL23 0x2D
#define REG_DACCONTROL24 0x2E
#define REG_DACCONTROL25 0x2F
#define REG_DACCONTROL26 0x30
#define REG_DACCONTROL27 0x31

/* 走官方 esp_codec_dev 的 ES8388 驱动（与小智 AI 完全同一条实现路径） */
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_out_dev;
static bool s_ready;

static i2c_master_dev_handle_t s_dev;

/* 惰性挂载 ES8388 到 IIC0 总线（地址 0x10，100kHz） */
static i2c_master_dev_handle_t es8388_dev(void)
{
    if (s_dev != NULL) {
        return s_dev;
    }
    void *bus = board_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "IIC0 总线尚未初始化");
        return NULL;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8388_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device((i2c_master_bus_handle_t)bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 挂载到 IIC0 失败");
        return NULL;
    }
    return s_dev;
}

esp_err_t es8388_write_reg(uint8_t reg, uint8_t val)
{
    i2c_master_dev_handle_t dev = es8388_dev();
    if (dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), 50);
}

esp_err_t es8388_read_reg(uint8_t reg, uint8_t *val)
{
    i2c_master_dev_handle_t dev = es8388_dev();
    if (dev == NULL || val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, 50);
}

bool es8388_probe(void)
{
    uint8_t v = 0;
    return es8388_read_reg(REG_CONTROL1, &v) == ESP_OK;
}

esp_err_t es8388_init(void)
{
    /* 幂等：按键录音会重复调用，已开好就直接返回，避免反复重建设备句柄 */
    if (s_ready && s_out_dev != NULL) {
        return ESP_OK;
    }

    /* 依赖 audio_init 已建好 I2S0 收发通道 */
    void *tx = NULL;
    void *rx = NULL;
    esp_err_t ret = audio_get_handles(&tx, &rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "取 I2S 句柄失败: %s（需先调用 audio_init）", esp_err_to_name(ret));
        return ret;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8388_ADDR << 1,        /* 传 8 位地址 0x20：esp_codec_dev 内部会 >>1 还原成 7 位 */
        .bus_handle = board_i2c_bus(),   /* 复用 board.c 建好的 IIC0 主总线（新版 i2c_master） */
        .clock_speed_hz = 100000,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_ctrl_if == NULL) {
        ESP_LOGE(TAG, "创建 I2C 控制接口失败");
        return ESP_FAIL;
    }

    s_gpio_if = audio_codec_new_gpio();

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx,
        .tx_handle = tx,
        .clk_src = 0,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_data_if == NULL) {
        ESP_LOGE(TAG, "创建 I2S 数据接口失败");
        return ESP_FAIL;
    }

    /* 完全对齐小智 AI 的 atk-dnesp32s3 板级配置 */
    es8388_codec_cfg_t cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = true,
        .pa_pin = -1,             /* 本板 PA 由 XL9555 的 SPK_EN 控制，不走 GPIO */
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0,          /* MD8002A 供电 5V */
            .codec_dac_voltage = 3.3,   /* ES8388 DAC 供电 3.3V */
        },
    };
    s_codec_if = es8388_codec_new(&cfg);
    if (s_codec_if == NULL) {
        ESP_LOGE(TAG, "创建 ES8388 codec 接口失败");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_out_dev = esp_codec_dev_new(&dev_cfg);
    if (s_out_dev == NULL) {
        ESP_LOGE(TAG, "创建输出设备失败");
        return ESP_FAIL;
    }

    /* 与 audio.c 的 I2S 配置保持一致：16kHz / 16bit / 左右双声道交织 */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0,
        .sample_rate = 16000,
        .mclk_multiple = 0,
    };
    int rc = esp_codec_dev_open(s_out_dev, &fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open 失败: %d", rc);
        return ESP_FAIL;
    }

    /* 驱动初始化时把 LOUT2/ROUT2（喇叭那一路）写成 0 = -30dB，这里拉到 0dB。
     * 注意必须用我们自己的 es8388_write_reg：esp_codec_dev_write_reg 依赖 codec 实现 set_reg，
     * ES8388 驱动没实现，那条写会被直接丢掉；而 set_out_vol 只调数字音量 0x1A/0x1B。 */
    es8388_write_reg(REG_DACCONTROL24, 30);
    es8388_write_reg(REG_DACCONTROL25, 30);
    es8388_write_reg(REG_DACCONTROL26, 30);
    es8388_write_reg(REG_DACCONTROL27, 30);
    esp_codec_dev_set_out_vol(s_out_dev, 100);
    esp_codec_dev_set_out_mute(s_out_dev, false);

    /* 官方驱动把 ADC 输入写成 0x02（DSR=1，LINPUT1-RINPUT1 跨声道差分）。
     * 本板板载 MIC 左右同源，差分会把语音抵消掉，第 2 阶段已确认必须用 0x00 单端。 */
    es8388_write_reg(REG_ADCCONTROL3, 0x00);

    s_ready = true;
    ESP_LOGI(TAG, "初始化完成（esp_codec_dev 官方链路 / master_mode / hw_gain 5V-3.3V）");
    return ESP_OK;
}

esp_err_t es8388_adc_enable(bool on)
{
    /* ADC 开关现在由 esp_codec_dev 的 open 统一管理（CHIPPOWER=0x00，DAC/ADC 全开）。
     * 录音只切 I2S 通道，这里保持幂等空操作，避免破坏 codec 已建立的通路。 */
    (void)on;
    return ESP_OK;
}

esp_err_t es8388_set_adc_volume(int db)
{
    if (db > 0) {
        db = 0;
    }
    if (db < -96) {
        db = -96;
    }
    uint8_t v = (uint8_t)((-db) << 1);
    esp_err_t ret = es8388_write_reg(REG_ADCCONTROL8, v);
    if (ret == ESP_OK) {
        ret = es8388_write_reg(REG_ADCCONTROL9, v);
    }
    return ret;
}

bool es8388_is_ready(void)
{
    return s_ready;
}

esp_err_t es8388_write_pcm(const int16_t *data, int samples)
{
    if (!s_ready || s_out_dev == NULL || data == NULL || samples <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* channel=2 时 len 是左右两个声道交织后的总字节数 */
    int rc = esp_codec_dev_write(s_out_dev, (void *)data, samples * 2 * (int)sizeof(int16_t));
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_write 失败: %d", rc);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "codec_dev 写入 %d 采样 (%d 字节) OK", samples, samples * 2 * (int)sizeof(int16_t));
    return ESP_OK;
}

esp_err_t es8388_set_output_volume(uint8_t v)
{
    if (v > 33) {
        v = 33;
    }
    esp_err_t ret = es8388_write_reg(REG_DACCONTROL24, v);
    ret |= es8388_write_reg(REG_DACCONTROL25, v);
    ret |= es8388_write_reg(REG_DACCONTROL26, v);
    ret |= es8388_write_reg(REG_DACCONTROL27, v);
    return ret;
}

void es8388_dump(void)
{
    static const struct {
        uint8_t reg;
        const char *name;
    } items[] = {
        { REG_CONTROL1, "CONTROL1" },
        { REG_CHIPPOWER, "CHIPPOWER" },
        { REG_DACPOWER, "DACPOWER" },
        { REG_MASTERMODE, "MASTERMODE" },
        { REG_DACCONTROL1, "DAC1 格式" },
        { REG_DACCONTROL3, "DAC3 静音位" },
        { REG_DACCONTROL17, "DAC17 L混音" },
        { REG_DACCONTROL20, "DAC20 R混音" },
        { REG_DACCONTROL21, "DAC21 LRCK源" },
        { REG_DACCONTROL24, "LOUT1 音量" },
        { REG_DACCONTROL25, "ROUT1 音量" },
        { REG_DACCONTROL26, "LOUT2 音量" },
        { REG_DACCONTROL27, "ROUT2 音量" },
    };
    uint8_t v = 0;
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        if (es8388_read_reg(items[i].reg, &v) == ESP_OK) {
            ESP_LOGI(TAG, "%s (0x%02X) = 0x%02X", items[i].name, items[i].reg, v);
        } else {
            ESP_LOGW(TAG, "%s (0x%02X) 读失败", items[i].name, items[i].reg);
        }
    }
}
