# ESP32-S3 语音终端

ESP32-S3 只做前端（屏幕、按键、音频 I/O），大模型推理全部放在电脑。
硬件为 **正点原子 ATK-DNESP32S3 V1.3**（ESP32-S3 N16R8：16MB Flash + 8MB Octal PSRAM，2.4" 320×240 ST7789V2，ES8388 Codec）。

当前进度：**语音对话闭环已完成，真机验证通过**（2026-11-18，固件 1.2.0）。
按住 KEY0 说话 → 录音上行 → Whisper 识别 → Ollama 回复 → macOS `say` 合成 → 音频下发播放。

## 架构

```
┌──────────── ESP32-S3 ────────────┐        ┌──────── Mac ────────────────┐
│ ST7789 320×240 + LVGL 9          │        │ FastAPI + WebSocket         │
│ XL9555 按键 KEY0-KEY3            │◄──────►│ /say  文本直推              │
│ ES8388 Codec + I2S0 收发         │  WS    │ /ask  Ollama 流式           │
│ （音频出口走 esp_codec_dev）      │        │ /tts  纯语音广播（调试）     │
│ WiFi STA                         │        │ faster-whisper  ASR         │
│ WebSocket 客户端（自动重连）      │        │ macOS say      TTS          │
└──────────────────────────────────┘        └─────────────────────────────┘
```

## 第 1 阶段做的事

| 模块 | 状态 |
|---|---|
| 板级初始化（XL9555 → SPI2 → ST7789 → 背光） | 完成 |
| LVGL 9 界面：顶栏状态 + 对话区 + 底栏 | 完成 |
| 中文显示 | 自生成 GB2312 点阵字体 `font_cjk16.c`，4bpp 16px |
| WiFi STA 连接与重连 | 完成 |
| WebSocket 连着网关、行列帧重组、断线自动重连 | 完成 |
| KEY0 说 / KEY1 打断 / KEY2 清屏 / KEY3 测试消息 | 完成 |
| 电脑端网关：`/say`、`/ask`（Ollama 流式） | 完成 |
| 音频上行 | **完成（第 2 阶段）** |
| ASR + Ollama + TTS 闭环、音频下行回播 | **完成（第 3 阶段）** |
| AEC 回声消除、流式识别、唤醒词 | 未开始（第 4 阶段，可选） |

## 第 2 阶段：录音上行

按住 KEY0 → ES8388 ADC 上电 → I2S0 每 0.1 秒取 1600 个采样 → 经 WebSocket 二进制帧推给网关 → 网关按 `listen.start` / `listen.stop` 落盘成 wav。

| 模块 | 实现 |
|---|---|
| ES8388 驱动 | `main/es8388.c`，I2C 7 位地址 `0x10`，32 条初始化序列取自 esp_codec_dev 1.6.2 |
| I2S0 接收 | `main/audio.c`，16kHz / 16bit / stereo 读入，MCLK=3 BCLK=46 LRCK=9 DIN=14 |
| 通道处理 | 只取左声道。**实测左右近似反相**（`L-R` 能量大于 `L+R`），求平均会把语音抵消掉 |
| DC 去除 | 一阶高通 `HP_ALPHA = 0.96`，截止约 100Hz |
| 增益 | MIC PGA `0xBB`（+33dB，已拉满）+ 软件 `PCM_GAIN 8`（+18dB）+ 限幅 |
| 上行帧 | 1600 采样 = 3200 字节 / 0.1 秒，约 256 kbps |
| 落盘 | `gateway/recordings/rec_YYYYmmdd_HHMMSS.wav`（16kHz / 16bit / 单声道） |

实测数据（说话 4.8 秒）：

| 指标 | 值 |
|---|---|
| 录音时长 | 4.80 秒（实际按住 4.81 秒） |
| 说话段 RMS | 4000–8500（-18 ~ -12 dBFS） |
| 整段 RMS | 3119（-20.4 dBFS） |
| 近削波样本 | 132 / 76800（0.17%） |
| 启动自测稳态 | RMS 600–900 |

### 调试中确认的坑（改代码前先看）

1. **ES8388 输出的是左右两个 slot**，I2S 配 `I2S_SLOT_MODE_MONO` 时 `i2s_channel_read` 仍按 4 字节/帧返回，但驱动按 2 字节/帧算，导致**样本数翻倍、时长虚报 2 倍**。必须用 STEREO 读。
2. **左右声道反相**。板载 MIC 是差分双输入，左右两路取的是差分对的两端，`(L+R)/2` 会把语音抵消掉，只剩 DC 阶跃。要么只取左声道，要么做 `L-R`。
3. **`ADCCONTROL3 = 0x02` 是 DSR=1，即 `LINPUT1 - RINPUT1` 跨声道差分**，左右同源时相减会把信号减没。本板用 `0x00` 单端。
4. **上电有 DC 阶跃**（实测 17150），不加高通的话 0 秒处会顶满 32768 并持续 1 秒。已用一阶高通去掉。
5. **MCLK 不能停**。`i2s_channel_disable` 会让 MCLK 停止，ES8388 的状态机重新启动很容易起不来。现在 I2S 全程开启，录音只切 `ADCPOWER`。
6. **板载 MIC 灵敏度偏低**，PGA 拉满 +33dB 仍需 8 倍软件增益才能到 -20dBFS。

## 第 3 阶段：ASR + Ollama + TTS 闭环

按住 KEY0 → 上行录音 → 松开 → 网关落盘 wav → faster-whisper 识别 → Ollama 流式回复（屏幕同步刷字）→ macOS `say` 合成 16kHz 单声道 → 每帧 1600 字节二进制下发 → 设备 I2S TX 直接播放。

| 环节 | 实现 | 实测 |
|---|---|---|
| ASR | faster-whisper `small`，CPU int8 | 5.8 秒音频 **0.95 秒**出结果，已知文本逐字正确 |
| 识别语言 | `language="zh"` + `initial_prompt` 强制简体 | 不加 prompt 会输出繁体 |
| VAD | **关闭** | 开启会吞掉 2.8 秒弱电平语音 |
| LLM | Ollama `qwen2.5:7b-instruct-q4_K_M` | system prompt 限制 60 字、简体、口语化 |
| TTS | macOS `say -o x.wav --data-format=LEI16@16000` | 输出 1ch / 16kHz / Int16，与设备格式完全一致 |
| 下发帧 | 1600 字节 = 800 采样 = 0.05 秒 | 必须小于设备端 `RX_MAX` 2048 |
| 播放 | I2S0 TX，单声道复制成 L/R 交织写入 | 与录音 RX 共用同一控制器时钟 |

端到端实测（电脑端模拟一次完整对话）：5.69 秒输入 → 识别逐字正确 → 回复 24 字 → 下发 187,480 字节 / 5.86 秒音频。

### 第 3 阶段的坑

7. **faster-whisper 直接传 wav 路径会崩**：内部 `av.open(..., metadata_errors="ignore")`，PyAV 13+ 已移除该参数，抛 `TypeError`。绕过办法是 `wave` + `numpy` 读成 float32 数组直接喂模型，完全不碰 av。
8. **Whisper 默认输出繁体**：必须传 `initial_prompt="以下是普通话的句子，请使用简体中文输出。"`，否则会原样吐繁体，并带着 Ollama 一起说繁体。
9. **`vad_filter=True` 会吞语音**：板载 MIC 电平偏低，VAD 把 4.8 秒里 2.8 秒判成静音，只剩"拜拜"两个字。按键录制的短音频关掉 VAD 更稳。
10. **`PCM_GAIN 8` 削波**：说话段出现多个 32768 峰值，失真反而拖垮识别。降到 5 后峰值正常。
11. **`audio_init()` 里 enable 了 RX 却没同步 `s_enabled` 标志**：`audio_read_pcm()` 开头就检查该标志，结果每帧都返回 -1，**录音全程 0 字节**；同时 `audio_selftest()` 再 enable 一次会失败并提前 return，串口连 `自测#N` 都不打印，很容易误判成"芯片没工作"。修复：init 里置位 + `audio_enable()` 改成幂等。
12. **`esp_websocket_client` 的 `op_code` 带 FIN 位**：收到二进制帧时是 `0x82` 不是 `0x02`，文本帧是 `0x81` 不是 `0x01`。直接 `== 0x02` 判断会让所有下行音频帧被当成未知类型丢掉，表现就是"文字回复正常、喇叭永远没声"。必须 `(op_code & 0x0F) == 0x02`。
13. **`DACCONTROL26/27` 默认是 `0x00`（-30dB）**：LOUT2/ROUT2 这一路几乎没声音，喇叭如果接的是这组输出就听不见。两路都写成 `0x1E`（0dB）最稳。
14. **DAC 静音没解除**：初始化序列开头 `DACCONTROL3 = 0x04` 是静音（参考实现注释原文就是 `0x04 mute / 0x00 unmute&ramp`），必须在 `DACPOWER = 0x3C` 之后补一条 `DACCONTROL3 = 0x00`。漏掉这一步的现象极具迷惑性：I2S 写入完全正常、串口能打出「播放 800 采样」、DAC 电源也是开的，但喇叭**全程一点声音都没有**。
15. **功放使能不在 P11，在 P02，而且是低电平使能**：喇叭链路是 `ES8388 LOUT2 → SPK_IN → MD8002A 功放 → SPEAKER`，MD8002A 的 `SHUTDOWN` 由 XL9555 控制。早期按原理图猜成 P11，结果 P11 拉高拉低都没声音；最后对照小智 AI 的板级代码发现它对 `SetOutputState(2, 0)`，把 **P02 拉低**，照做后喇叭立刻出声。即 `s_exio_out = 0xFFFB`。这是整个第 3 阶段最耗时的一个坑：寄存器全对、I2S 写入全成功、功放却一直处在关断态。
16. **`esp_codec_dev` 的 I2C 地址要传 8 位值**：`audio_codec_i2c_cfg_t.addr` 内部会做 `(addr >> 1)` 再交给 `i2c_master_bus_add_device`，所以必须传 `0x20`（`ES8388_ADDR << 1`）。传 7 位 `0x10` 会被右移成 `0x08`，表现为 `es8388_open` 里每条写寄存器都失败，串口只打 `Fail to write register` / `Fail to open`。注意这与我们自己的 `es8388_write_reg()` 不同，后者走新版 API 要传 7 位 `0x10`，两套地址约定别混。
17. **`esp_codec_dev_write_reg()` 对 ES8388 是空操作**：它转发给 `codec_if->set_reg`，而 ES8388 驱动没实现该回调，直接返回 `NOT_SUPPORT`，写入被静默丢弃。要给 ES8388 写寄存器，用我们自己的 `es8388_write_reg()`。
18. **`esp_codec_dev_set_out_vol()` 只调数字音量**：ES8388 的 `set_vol` 写的是 `DACCONTROL4/5`（0x1A/0x1B），完全不管模拟输出 `0x2E~0x31`。而驱动 `es8388_open()` 又把 `LOUT2/ROUT2`（0x30/0x31）写成 **0 = -30dB**，这就是"数字链路全通但几乎听不见"的原因。必须在 open 之后用 `es8388_write_reg()` 把 `0x2E~0x31` 全部写 `30`（0dB）。
19. **新旧 I2C 驱动不能混用**：`esp_codec_dev` 在 IDF ≥ 5.3 且未开 `CODEC_I2C_BACKWARD_COMPATIBLE` 时走新版 `i2c_master` 总线；如果 `board.c` 还在用旧版 `i2c_driver_install` 占用同一个端口，启动阶段直接 `abort()`，回溯落在 `check_i2c_driver_conflict`。正解是把板级 I2C 整体迁到新版 `i2c_master`（`i2c_new_master_bus` + `i2c_master_transmit` / `i2c_master_transmit_receive`），并把总线句柄通过 `audio_codec_i2c_cfg_t.bus_handle` 交给 codec。
20. **官方驱动会把 ADC 输入改成差分**：`es8388_open()` 写 `ADCCONTROL3 = 0x02`（DSR=1，LINPUT1-RINPUT1 跨声道差分）。本板板载 MIC 左右同源，差分会把语音抵消掉。走 `esp_codec_dev` 后必须在 init 末尾补一条 `es8388_write_reg(0x0B, 0x00)` 恢复单端输入，否则第 2 阶段调好的录音会退化。

## 编译与烧录

```bash
cd ESP32-S3-Voice-Terminal
export IDF_PYTHON_ENV_PATH=/Users/xiaoyi/.espressif/python_env/idf5.5_py3.9_env
. /Users/xiaoyi/esp/v5.5.6/esp-idf/export.sh

idf.py menuconfig          # Voice Terminal → 填 WiFi SSID/密码/网关地址
idf.py build
idf.py -p /dev/cu.usbmodem21301 flash monitor
```

必须配置的三项（menuconfig → `Voice Terminal`）：

| 配置 | 示例 |
|---|---|
| `VOICE_WIFI_SSID` | 你的 2.4GHz WiFi |
| `VOICE_WIFI_PASSWORD` | 密码（开放网络留空） |
| `VOICE_SERVER_URL` | `ws://192.168.1.100:8765/ws`（填电脑的局域网 IP） |

## 网关启动

```bash
cd gateway
/opt/homebrew/bin/python3.12 -m venv .venv      # 必须 3.12，ctranslate2 还没有 3.14 的 wheel
./.venv/bin/pip install -r requirements.txt faster-whisper
./.venv/bin/uvicorn gateway:app --host 0.0.0.0 --port 8765
```

首次启动会自动下载 Whisper 模型（`small` 约 460MB，缓存到 `~/.cache/huggingface`）。

默认模型 `qwen2.5:7b-instruct-q4_K_M`（实测 M1 Pro 热态 0.58 秒 / 8 token，首帧 0.29 秒），可用环境变量覆盖：

```bash
OLLAMA_MODEL=其他模型 ./.venv/bin/uvicorn gateway:app --host 0.0.0.0 --port 8765
```

**不要用推理模型**：`qwen3:4b` 与 `deepseek-r1:8b` 实测会把思考链当正文吐出来（`think:false` 与 `/no_think` 在该 Ollama 版本下均无效），首句变成"嗯，用户让我……"，且首字延迟 18 秒以上。

## 控制台（v1.4.0 新增）

浏览器打开 `http://<Mac 的局域网 IP>:8765/` 即可，纯单文件 HTML，不依赖任何前端库。

| 功能 | 说明 |
|---|---|
| 设备状态 | 在线数、IP、当前 SSID、RSSI、固件版本、设备运行时长（由设备 `hello` 上报） |
| WiFi 切换 | 扫描周边热点（同 SSID 的多个 AP 已合并并保留最强信号），点选后填密码切换 |
| 模型 | 列出本机 Ollama 已装模型，一键切换；切换前先校验模型确实存在 |
| 对话 | 打字直接走 Ollama + TTS，设备语音播报，显示生成耗时与语音时长 |
| 试听 | 下发任意文本的 TTS，不经过 ASR / LLM |
| 音量 / 语速 | 音量写 ES8388 模拟输出寄存器（0~33），语速改 `say -r` |
| 音色 | 切换 macOS 系统中文音色（婷婷 / 美佳 / 善怡），下一次播报立即生效 |
| 语音引擎 | 在 macOS 系统语音与 CosyVoice2 之间切换；后者可零样本克隆任意人声，角色不限量（部署见 `gateway/COSYVOICE.md`） |
| 人设 | 在线编辑 system prompt，对下一次对话立即生效 |
| 对话记录 | 按键对话与控制台对话统一进历史，含耗时 |
| 网关日志 | 内存环形缓冲 400 行，页面实时刷新 |

**WiFi 凭据写进设备 NVS**：`wifi_mgr` 优先读 NVS，没有才回退到 `menuconfig` 的编译期默认值。所以换网不需要重新编译烧录。

**切换失败会回滚**：新配置 20 秒内连不上，自动恢复旧凭据并重连，设备不会失联。

**切换过程必然断连**：换网卡会断开现有 WebSocket，设备重连后重新 `hello`，控制台轮询 2 秒内就会显示新 IP。

## 协议

见 `main/protocol.h`。文本帧是 UTF-8 JSON，音频帧是**二进制**。

- 上行文本：`hello`（含 `fw` / `chip` / `ip` / `rssi` / `ssid` / `uptime`）、`key`、`listen.start`、`listen.stop`、`abort`、`text`、`wifi.list`、`wifi.result`
- 上行二进制：录音 PCM（16bit 单声道裸数据），只在 `listen.start` 与 `listen.stop` 之间发送
- 下行文本：`say`（`final=false` 表示流式覆盖最后一条）、`state`、`clear`、`wifi.scan`、`wifi.set`（`ssid` + `password`）、`volume.set`（`value` 0~33）
- 下行二进制：**TTS PCM**（16bit 单声道裸数据），设备端拼帧后直接送 I2S TX 播放

## 引脚（已核对）

来源：《DNESP32S3 硬件参考手册》表 1.2.2.1，与本板 V1.3 `board.h` 交叉验证一致。

| 用途 | GPIO |
|---|---|
| LCD MOSI / SCLK / MISO / CS / DC | 11 / 12 / 13 / 21 / 40 |
| I2C0 SDA / SCL（XL9555 + ES8388） | 41 / 42 |
| 板载 LED | 1 |
| **I2S MCLK / BCLK / LRCK / 数据出 / 数据入** | **3 / 46 / 9 / 10 / 14**（第 2 阶段启用） |

禁用：GPIO33–37（Octal PSRAM）、0（BOOT）、19/20（USB）、43/44（UART0）。

## 已知限制

- 内置 LVGL `SourceHanSansSC_16_CJK` 字体**不含汉字**（实测只覆盖 174 个码点：ASCII + 日文假名 + 标点），因此本工程自带 `font_cjk16.c`：由 `lv_font_conv` 以 macOS `Arial Unicode.ttf` 生成，覆盖 GB2312 全部 6763 汉字 + ASCII + 中文标点（`0x2000-0x206F` / `0x3000-0x303F` / `0xFF00-0xFFEF` 三段），合计 7212 字符，占固件约 670KB。
- 字体只覆盖 GB2312 与上述标点区段，GB2312 之外的汉字（生僻字、部分繁体）显示为空白。
- 板载 MIC 灵敏度偏低，需要 PGA 拉满 + 5 倍软件增益；安静环境底噪约 -32dBFS。
- Whisper `small` 对低电平录音仍有识别损失；可用 `WHISPER_MODEL=medium` 环境变量换更大模型（慢约 2 倍）。
- TTS 依赖 macOS `say`，换 Linux 需改成 Piper 或 edge-tts。
- 录音与播放共用 I2S0：说话瞬间若扬声器还在输出，会被 MIC 拾取，目前没有 AEC。
- 录音开头 0.5 秒内高通滤波器还在收敛，会有轻微饱和，正式 ASR 建议从 0.3 秒后开始取用。
- ES8388 的两个 0x35/0x37/0x39 寄存器用于关闭内部 DLL，仅对低采样率生效，未逐项验证。
- OLED/屏幕方向、颜色顺序继承自已验证的 `board.c`，未重新实测。

## 目录

```
ESP32-S3-Voice-Terminal/
├── CMakeLists.txt
├── partitions.csv            # 16MB：app 6MB + spiffs 1MB
├── sdkconfig.defaults
├── main/
│   ├── app_main.c            # 启动、任务、按键轮询、录音自测
│   ├── board.c/.h            # XL9555 + SPI2 + ST7789 + LED
│   ├── wifi_mgr.c/.h         # STA 连接与重连
│   ├── ws_client.c/.h        # WebSocket + 下行 JSON 分发 + 二进制上行
│   ├── ui.c/.h               # LVGL 界面与线程锁
│   ├── es8388.c/.h           # ES8388 寄存器驱动（I2C 0x10）
│   ├── audio.c/.h            # I2S0 录音、高通、增益
│   ├── protocol.h            # 协议定义
│   ├── font_cjk16.c          # GB2312 点阵字体（生成产物，勿手改）
│   └── Kconfig.projbuild     # WiFi / 网关配置项
└── gateway/
    ├── gateway.py            # WS + /say + /ask + 录音落盘
    ├── talk.py               # 命令行测试
    ├── requirements.txt
    └── recordings/           # 上行录音 wav
```
