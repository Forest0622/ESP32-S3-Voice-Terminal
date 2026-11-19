# CosyVoice2 语音引擎部署（macOS Apple Silicon）

控制台的「语音引擎」可以在两套 TTS 之间随时切换：

| 引擎 | 合成耗时 | 音色 | 拟人度 |
|---|---|---|---|
| macOS 系统语音（`say`） | < 0.5 秒 | 婷婷 / 美佳 / 善怡 | 拼接式，机器味明显 |
| CosyVoice2（本目录） | 8~13 秒（13 字） | 可克隆任意人声，角色数量不限 | 接近真人 |

CosyVoice2 跑在**独立进程**（端口 8770），不塞进网关的 venv —— torch 2.3 和 faster-whisper 的 ctranslate2 混在一起会互相降级 numpy / protobuf。

## 目录位置

```
~/Personal/Embedded-Systems/ESP-S3/Project/cosyvoice-tmp/
├── .venv/                          # 独立环境（python3.12）
├── pretrained_models/
│   └── CosyVoice2-0.5B/            # 5.3 GB
├── voices/                         # 角色库：<名字>.wav + <名字>.txt
├── cosyvoice_server.py             # HTTP 服务
├── start_cv.sh                     # 一键启动
└── cosyvoice/ third_party/         # 官方源码
```

## 启动

```bash
cd ~/Personal/Embedded-Systems/ESP-S3/Project/cosyvoice-tmp
./start_cv.sh          # 或 ./.venv/bin/uvicorn cosyvoice_server:app --host 127.0.0.1 --port 8770
```

首请求要等 6~8 秒加载模型（之后常驻内存）。

## 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/health` | 模型是否已加载、角色数 |
| GET | `/voices` | 角色列表 |
| POST | `/tts` | `{"text","spk","speed"}` → 16kHz/16bit/单声道裸 PCM |
| POST | `/voice/add` | multipart：`name` + `prompt_text` + `file` |
| DELETE | `/voice/<name>` | 删除角色（默认角色不可删） |

网关控制台已经把这些接口包好了，浏览器里点几下就行，不用记路径。

## 新增角色（关键步骤）

1. 准备一段 **3~10 秒**、单人、无背景音的参考音频（wav / flac / ogg）。
2. 在控制台「CosyVoice 角色」卡片里：填角色名 → 填**参考文本（音频里说的原话，必须一字不差）** → 选文件 → 上传建角色。
3. 切成该角色 → 试听。

**参考文本必须和音频真实内容一致**。这是最容易踩的坑：参考文本对不上，模型会输出完全听不懂的乱码（实测转写结果变成"希望此说子自一生…"）。

仓库自带的 `asset/zero_shot_prompt.wav` 真实内容是「希望你以后能够做得比我还好」——用 whisper 转写出来的，不是 README 里写的"收到好友的生日祝福"。默认角色「小雅」用的就是它。

## 安装踩过的坑（macOS + Python 3.12）

官方 `requirements.txt` 在 macOS 上直接装会连环失败，实际做法是**只装最小依赖集**：

```bash
/opt/homebrew/bin/python3.12 -m venv .venv
./.venv/bin/pip install -U pip setuptools wheel
./.venv/bin/pip install openai-whisper            # 先装，否则缺 pkg_resources 会整批回滚
./.venv/bin/pip install torch==2.3.1 torchaudio==2.3.1 transformers==4.51.3 \
    HyperPyYAML==1.2.3 conformer==0.3.2 diffusers==0.29.0 omegaconf==2.3.0 \
    hydra-core==1.3.2 librosa==0.10.2 soundfile==0.12.1 numpy==1.26.4 \
    onnxruntime==1.18.0 modelscope x-transformers inflect==7.3.1 networkx \
    pyarrow==18.1.0 rich wget gdown pyworld
./.venv/bin/pip install lightning==2.2.4 fastapi uvicorn python-multipart
```

| 坑 | 现象 | 解法 |
|---|---|---|
| `openai-whisper` 编译 | `No module named 'pkg_resources'`，整批回滚 | 先 `pip install setuptools wheel` 再装 |
| `lightning` 太新 | `KeyError: '_items'` | 装 `lightning==2.2.4` |
| `matplotlib` 3.11 | 同 `KeyError: '_items'`（读 macOS 27 系统字体时崩） | 降到 `matplotlib==3.7.5` |
| `pyarrow` 26 | `requires NumPy 2.0 or newer, found 1.26.4` | 降到 `pyarrow==18.1.0` |
| `wetext` / `pynini` | macOS 上几乎装不上 | **不用装**，frontend 有三级兜底（ttsfrd → wetext → 无前端），实测无前端完全可用 |
| MPS 加速 | `aten::unfold_backward is not implemented for MPS`，开 fallback 要 48 秒（比 CPU 更慢） | 用 CPU，默认就是 CPU |
| 传 tensor 当参考音频 | `Invalid file: tensor(...)` | 这版 `frontend` 内部会 `load_wav(prompt_wav)`，**必须传文件路径** |

## 性能实测（M1 Pro，CPU）

| 文本 | 字数 | 音频时长 | 合成耗时 |
|---|---|---|---|
| 我是小雅，很高兴认识你。 | 12 | 2.3 秒 | ≈ 8 秒 |
| 你好，我是小雅，现在用神经网络合成的声音在说话。 | 22 | 5.9 秒 | ≈ 15 秒 |

RTF ≈ 2.5~3.5，即**比实时慢 2.5~3.5 倍**。所以 CosyVoice 适合「先想好再问」的场景，按键实时对话建议仍用 macOS 引擎（< 0.5 秒）。

## 相关服务端口

| 端口 | 服务 |
|---|---|
| 8765 | 网关 + 控制台（`http://<Mac-IP>:8765/`） |
| 8770 | CosyVoice2 TTS |
| 11434 | Ollama |
