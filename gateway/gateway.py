#!/usr/bin/env python3
"""
ESP32-S3 语音终端 - 电脑端网关（第 1 阶段：文本直通 + Ollama 流式）

启动：
    uvicorn gateway:app --host 0.0.0.0 --port 8765

接口：
    WS   /ws                    设备连接
    POST /say {"text": "..."}   把文本推到所有已连接设备
    POST /ask {"text": "..."}   走 Ollama 流式生成，逐段推到设备
    GET  /health                健康检查与已连接设备数

协议见固件 main/protocol.h。
"""

from __future__ import annotations

import asyncio
import collections
import json
import logging
import os
import subprocess
import tempfile
import time
import wave
from typing import Any

import httpx
import numpy as np
from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from faster_whisper import WhisperModel
from pydantic import BaseModel

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s | %(message)s",
)
log = logging.getLogger("gateway")

OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://127.0.0.1:11434")
OLLAMA_MODEL = os.environ.get("OLLAMA_MODEL", "qwen2.5:7b-instruct-q4_K_M")

# ---------------- 控制台可运行时修改的状态 ----------------
current_model = OLLAMA_MODEL
PROMPT_DEFAULT = "你是一个语音助手，回答必须用简体中文，控制在 60 字以内，短句口语化，适合直接朗读。"
system_prompt = PROMPT_DEFAULT
VOLUME = 30                     # 设备喇叭模拟音量 0~33（30 约 0dB）
SPEECH_RATE = 180               # macOS say 语速（词/分钟）
VOICE = "Tingting"              # 当前音色（对应 say -v 的 voice id）

# 实测能真正合成中文的音色。其余 Eddy / Flo / Sandy / Reed / Rocko / Grandpa /
# Grandma / Shelley 只输出 4466 字节空音频，不可用，所以不做成选项。
VOICES = [
    {"id": "Tingting", "name": "婷婷", "desc": "普通话 · 大陆女声"},
    {"id": "Meijia", "name": "美佳", "desc": "普通话 · 台湾女声"},
    {"id": "Sinji", "name": "善怡", "desc": "粤语 · 香港女声"},
]

CONSOLE_HTML = os.path.join(os.path.dirname(os.path.abspath(__file__)), "console.html")
START_TS = time.time()
history: collections.deque = collections.deque(maxlen=50)   # 最近对话
log_lines: collections.deque = collections.deque(maxlen=400)  # 日志环形缓冲
pending: dict[str, asyncio.Future] = {}                      # 请求-响应配对（如 WiFi 扫描）


class _RingLogHandler(logging.Handler):
    """把日志写进内存环形缓冲，控制台按需拉取"""

    def emit(self, record: logging.LogRecord) -> None:
        try:
            log_lines.append(self.format(record))
        except Exception:
            pass


_ring = _RingLogHandler()
_ring.setFormatter(logging.Formatter("%(asctime)s %(levelname)s | %(message)s"))
logging.getLogger().addHandler(_ring)
# 设备端单条下行报文上限 2048 字节；这里是保守的字符数上限
SAY_CHAR_LIMIT = 600
# 上行录音落盘目录（16kHz / 16bit / 单声道）
REC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "recordings")
os.makedirs(REC_DIR, exist_ok=True)

# ASR：faster-whisper（中文用 small 已够，medium 更准但慢一倍）
WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "small")
_whisper: WhisperModel | None = None


def get_whisper() -> WhisperModel:
    global _whisper
    if _whisper is None:
        log.info("加载 Whisper 模型 %s", WHISPER_MODEL)
        _whisper = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
    return _whisper


ASR_PROMPT = "以下是普通话的句子，请使用简体中文输出。"


def transcribe_wav(path: str) -> str:
    """直接读 wav 的 PCM 交给模型，绕开 PyAV 版本兼容问题"""
    w = wave.open(path, "rb")
    frames = w.readframes(w.getnframes())
    w.close()
    audio = np.frombuffer(frames, dtype=np.int16).astype(np.float32) / 32768.0

    # 太短或太安静的录音直接判空：Whisper 对着噪声会编出整句话，
    # 最常见的就是把 initial_prompt 原样吐回来
    if audio.size < 1600:
        log.info("音频过短（%d 采样），判空", audio.size)
        return ""
    rms = float(np.sqrt(np.mean(audio * audio)))
    if rms < 0.002:
        log.info("音频能量过低（rms=%.4f），判空", rms)
        return ""

    segments, _ = get_whisper().transcribe(
        audio,
        language="zh",
        beam_size=1,
        vad_filter=False,   # 录音本来就短，VAD 会把弱电平段误判成静音
        initial_prompt=ASR_PROMPT,
    )
    text = "".join(s.text for s in segments).strip()

    # 幻觉兜底：结果里混进提示词，基本可以确定是空音频编出来的
    for frag in ("请使用简体中文输出", "以下是普通话的句子"):
        if frag in text:
            log.info("识别结果为提示词幻觉，判空：%s", text)
            return ""
    return text


def tts_pcm(text: str, voice: str | None = None) -> bytes:
    """用 macOS say 生成 16kHz / 16bit / 单声道裸 PCM"""
    v = voice or VOICE
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        path = f.name
    try:
        subprocess.run(
            ["say", "-v", v, "-o", path, "--data-format=LEI16@16000",
             "-r", str(SPEECH_RATE), text],
            check=True,
        )
        w = wave.open(path, "rb")
        data = w.readframes(w.getnframes())
        w.close()
    finally:
        if os.path.exists(path):
            os.unlink(path)
    return data

app = FastAPI(title="ESP32-S3 Voice Gateway")
clients: set[WebSocket] = set()
device_info: dict[str, Any] = {}


class SayReq(BaseModel):
    text: str
    final: bool = True


class AskReq(BaseModel):
    text: str


async def broadcast(payload: dict[str, Any]) -> int:
    """向所有设备广播，返回值是成功发出的数量"""
    dead: list[WebSocket] = []
    sent = 0
    for ws in list(clients):
        try:
            await ws.send_json(payload)
            sent += 1
        except Exception:
            dead.append(ws)
    for ws in dead:
        clients.discard(ws)
    return sent


async def broadcast_bytes(data: bytes) -> int:
    """向所有设备广播二进制音频"""
    dead: list[WebSocket] = []
    sent = 0
    for ws in list(clients):
        try:
            await ws.send_bytes(data)
            sent += 1
        except Exception as exc:
            log.warning("音频广播失败: %s", exc)
            dead.append(ws)
    for ws in dead:
        clients.discard(ws)
    return sent


@app.get("/health")
async def health() -> dict[str, Any]:
    return {
        "ok": True,
        "clients": len(clients),
        "model": current_model,
        "device": device_info,
    }


@app.post("/say")
async def say(req: SayReq) -> dict[str, Any]:
    text = req.text[:SAY_CHAR_LIMIT]
    sent = await broadcast({"type": "say", "text": text, "final": req.final})
    log.info("say -> %d 台设备: %s", sent, text)
    return {"sent": sent, "text": text}


@app.post("/tts")
async def tts(req: SayReq) -> dict[str, Any]:
    """跳过 ASR/大模型，直接把文本转成语音广播给设备，用来单独验证喇叭通路"""
    text = req.text[:SAY_CHAR_LIMIT]
    await broadcast({"type": "say", "text": text, "final": True})
    await broadcast({"type": "state", "value": "speaking"})
    pcm = await asyncio.to_thread(tts_pcm, text)
    # 和设备端 2048 采样缓冲对齐：每帧 1600 字节 = 800 采样 = 0.05 秒
    for off in range(0, len(pcm), 1600):
        await broadcast_bytes(pcm[off:off + 1600])
        await asyncio.sleep(0.05)
    await broadcast({"type": "state", "value": "idle"})
    log.info("tts 广播完成 %d 字节 / %.1f 秒", len(pcm), len(pcm) / 32000.0)
    return {"bytes": len(pcm), "seconds": round(len(pcm) / 32000.0, 2), "text": text}


# ==================== 控制台 ====================


@app.get("/", include_in_schema=False)
async def console_page():
    """浏览器打开 http://<mac-ip>:8765/ 即为控制台"""
    if not os.path.exists(CONSOLE_HTML):
        raise HTTPException(500, "console.html 缺失")
    return FileResponse(CONSOLE_HTML, media_type="text/html")


@app.get("/api/status")
async def api_status() -> dict[str, Any]:
    return {
        "ok": True,
        "online": len(clients),
        "device": dict(device_info),
        "model": current_model,
        "prompt": system_prompt,
        "volume": VOLUME,
        "speech_rate": SPEECH_RATE,
        "voice": VOICE,
        "uptime": round(time.time() - START_TS, 1),
    }


@app.get("/api/voices")
async def api_voices() -> dict[str, Any]:
    return {"ok": True, "voices": VOICES, "current": VOICE}


class VoiceReq(BaseModel):
    id: str


@app.post("/api/voice")
async def api_voice(req: VoiceReq) -> dict[str, Any]:
    global VOICE
    if req.id not in [v["id"] for v in VOICES]:
        raise HTTPException(404, f"没有音色 {req.id}")
    VOICE = req.id
    log.info("音色切换 -> %s", VOICE)
    return {"ok": True, "current": VOICE}


@app.get("/api/models")
async def api_models() -> dict[str, Any]:
    """列出本机 Ollama 已装模型"""
    try:
        async with httpx.AsyncClient(timeout=5.0) as http:
            r = await http.get(f"{OLLAMA_URL}/api/tags")
            r.raise_for_status()
            raw = r.json()
    except Exception as exc:
        return {"ok": False, "error": str(exc), "models": [], "current": current_model}
    models = []
    for m in raw.get("models", []):
        size = m.get("size") or 0
        models.append(
            {
                "name": m.get("name"),
                "size": size,
                "size_gb": round(size / 1e9, 2) if size else 0,
                "modified": m.get("modified_at"),
            }
        )
    models.sort(key=lambda x: x["name"] or "")
    return {"ok": True, "models": models, "current": current_model}


class ModelReq(BaseModel):
    name: str


@app.post("/api/model")
async def api_set_model(req: ModelReq) -> dict[str, Any]:
    global current_model
    name = req.name.strip()
    if not name:
        raise HTTPException(400, "模型名不能为空")
    # 确认模型真的存在，避免切到不存在的名字后每次对话都失败
    try:
        async with httpx.AsyncClient(timeout=5.0) as http:
            r = await http.get(f"{OLLAMA_URL}/api/tags")
            r.raise_for_status()
            names = [m.get("name") for m in r.json().get("models", [])]
    except Exception as exc:
        raise HTTPException(502, f"读取 Ollama 模型列表失败: {exc}")
    if name not in names:
        raise HTTPException(404, f"本机没有模型 {name}")
    current_model = name
    log.info("切换模型 -> %s", current_model)
    return {"ok": True, "current": current_model}


@app.post("/api/prompt")
async def api_set_prompt(req: SayReq) -> dict[str, Any]:
    global system_prompt
    system_prompt = req.text.strip() or PROMPT_DEFAULT
    log.info("人设已更新（%d 字）", len(system_prompt))
    return {"ok": True, "prompt": system_prompt}


@app.get("/api/prompt")
async def api_get_prompt() -> dict[str, Any]:
    return {"ok": True, "prompt": system_prompt, "default": PROMPT_DEFAULT}


class VolumeReq(BaseModel):
    value: int


@app.post("/api/volume")
async def api_volume(req: VolumeReq) -> dict[str, Any]:
    global VOLUME
    v = max(0, min(33, req.value))
    sent = await broadcast({"type": "volume.set", "value": v})
    VOLUME = v
    return {"ok": True, "value": v, "sent": sent}


class RateReq(BaseModel):
    value: int


@app.post("/api/rate")
async def api_rate(req: RateReq) -> dict[str, Any]:
    global SPEECH_RATE
    SPEECH_RATE = max(100, min(320, req.value))
    return {"ok": True, "value": SPEECH_RATE}


@app.post("/api/abort")
async def api_abort() -> dict[str, Any]:
    sent = await broadcast({"type": "abort"})
    await broadcast({"type": "state", "value": "idle"})
    return {"ok": True, "sent": sent}


async def request_device(payload: dict[str, Any], expect: str, timeout: float) -> dict[str, Any]:
    """下发一条控制指令并等待设备回指定 type 的消息"""
    sent = await broadcast(payload)
    if sent == 0:
        raise HTTPException(503, "没有在线设备")
    loop = asyncio.get_running_loop()
    fut = loop.create_future()
    pending[expect] = fut
    try:
        return await asyncio.wait_for(fut, timeout)
    except asyncio.TimeoutError:
        raise HTTPException(504, "设备响应超时")
    finally:
        pending.pop(expect, None)


@app.get("/api/wifi/scan")
async def api_wifi_scan() -> dict[str, Any]:
    msg = await request_device({"type": "wifi.scan"}, "wifi.list", timeout=15.0)
    return {"ok": bool(msg.get("ok")), "networks": msg.get("networks", []),
            "current": msg.get("current"), "device_ip": msg.get("ip")}


class WifiReq(BaseModel):
    ssid: str
    password: str = ""


@app.post("/api/wifi")
async def api_wifi(req: WifiReq) -> dict[str, Any]:
    ssid = req.ssid.strip()
    if not ssid:
        raise HTTPException(400, "SSID 不能为空")
    sent = await broadcast({"type": "wifi.set", "ssid": ssid, "password": req.password})
    if sent == 0:
        raise HTTPException(503, "没有在线设备")
    # 切换会断连，设备多半来不及回结果；这里只确认已下发，结果看 status 里的 ip/ssid
    log.info("已下发 WiFi 切换 -> %s", ssid)
    return {"ok": True, "sent": sent, "ssid": ssid,
            "note": "设备重连后若 IP 变化，控制台会在几秒内自动刷新为新的状态"}


class ConsoleTextReq(BaseModel):
    text: str


@app.post("/api/text")
async def api_text(req: ConsoleTextReq) -> dict[str, Any]:
    """控制台打字对话：不走录音和 ASR，直接 Ollama + TTS"""
    text = req.text.strip()
    if not text:
        raise HTTPException(400, "内容不能为空")
    if not clients:
        raise HTTPException(503, "没有在线设备")

    t0 = time.time()
    await broadcast({"type": "state", "value": "thinking"})
    await broadcast({"type": "clear"})
    acc = ""

    async def push(part: str) -> None:
        nonlocal acc
        acc = part
        await broadcast({"type": "say", "text": part[:SAY_CHAR_LIMIT], "final": False})

    try:
        reply = await ollama_stream(text, push)
    except Exception as exc:
        log.error("控制台对话失败: %s", exc)
        raise HTTPException(502, f"Ollama 调用失败: {exc}")

    await broadcast({"type": "say", "text": reply[:SAY_CHAR_LIMIT], "final": True})
    llm_ms = int((time.time() - t0) * 1000)

    await broadcast({"type": "state", "value": "speaking"})
    pcm = await asyncio.to_thread(tts_pcm, reply)
    for off in range(0, len(pcm), 1600):
        await broadcast_bytes(pcm[off:off + 1600])
        await asyncio.sleep(0.05)
    await broadcast({"type": "state", "value": "idle"})

    entry = {
        "ts": time.strftime("%H:%M:%S"),
        "source": "console",
        "user": text,
        "reply": reply,
        "asr_ms": 0,
        "llm_ms": llm_ms,
        "audio_sec": round(len(pcm) / 32000.0, 2),
    }
    history.appendleft(entry)
    log.info("控制台对话完成：%d 字回复 / %.1f 秒语音", len(reply), len(pcm) / 32000.0)
    return {"ok": True, **entry}


@app.get("/api/history")
async def api_history() -> dict[str, Any]:
    return {"ok": True, "items": list(history)}


@app.get("/api/logs")
async def api_logs(limit: int = 120) -> dict[str, Any]:
    n = max(10, min(400, limit))
    return {"ok": True, "lines": list(log_lines)[-n:]}


@app.post("/ask")
async def ask(req: AskReq) -> dict[str, Any]:
    """走 Ollama 流式，边生成边推给设备"""
    sent = 0
    acc = ""
    await broadcast({"type": "state", "value": "thinking"})
    try:
        async with httpx.AsyncClient(timeout=None) as http:
            async with http.stream(
                "POST",
                f"{OLLAMA_URL}/api/chat",
                json={
                    "model": current_model,
                    "messages": [{"role": "user", "content": req.text}],
                    "stream": True,
                    "think": False,
                },
            ) as resp:
                if resp.status_code != 200:
                    body = await resp.aread()
                    log.error("Ollama %s: %.200s", resp.status_code, body)
                    await broadcast(
                        {"type": "say", "text": f"Ollama 返回 {resp.status_code}", "final": True}
                    )
                    return {"sent": 0, "error": f"ollama {resp.status_code}"}

                next_push = 0
                async for line in resp.aiter_lines():
                    if not line.strip():
                        continue
                    try:
                        obj = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    piece = obj.get("message", {}).get("content", "")
                    if piece:
                        acc += piece
                        # 节流：每累积 20 字推一次。设备端每次 set_text 都要重排，
                        # 逐 token 推会把 320×240 的屏刷到卡死。
                        if len(acc) >= next_push:
                            await broadcast(
                                {"type": "say", "text": acc[:SAY_CHAR_LIMIT], "final": False}
                            )
                            sent += 1
                            next_push = len(acc) + 20
                    if obj.get("done"):
                        break
    except httpx.HTTPError as exc:
        log.error("Ollama 连接失败: %s", exc)
        await broadcast(
            {"type": "say", "text": "Ollama 未启动，先执行 ollama serve", "final": True}
        )
        await broadcast({"type": "state", "value": "idle"})
        return {"sent": 0, "error": str(exc)}

    await broadcast({"type": "say", "text": acc[:SAY_CHAR_LIMIT], "final": True})
    await broadcast({"type": "state", "value": "idle"})
    log.info("ask 完成 %d 段，共 %d 字", sent, len(acc))
    return {"sent": sent, "chars": len(acc), "text": acc}


async def ollama_stream(prompt: str, push) -> str:
    """走 Ollama 流式，每累积 20 字调用一次 push(text)，返回完整回复"""
    acc = ""
    async with httpx.AsyncClient(timeout=None) as http:
        async with http.stream(
            "POST",
            f"{OLLAMA_URL}/api/chat",
            json={
                "model": current_model,
                "messages": [
                    {
                        "role": "system",
                        "content": system_prompt,
                    },
                    {"role": "user", "content": prompt},
                ],
                "stream": True,
                "think": False,
            },
        ) as resp:
            if resp.status_code != 200:
                body = await resp.aread()
                raise RuntimeError(f"ollama {resp.status_code}: {body[:200]!r}")
            next_push = 20
            async for line in resp.aiter_lines():
                if not line.strip():
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                piece = obj.get("message", {}).get("content", "")
                if piece:
                    acc += piece
                    if len(acc) >= next_push:
                        await push(acc)
                        next_push = len(acc) + 20
                if obj.get("done"):
                    break
    return acc


async def handle_utterance(ws: WebSocket, wav_path: str) -> None:
    """一段录音的完整闭环：ASR → Ollama → TTS 下发"""
    await ws.send_json({"type": "state", "value": "thinking"})
    await ws.send_json({"type": "say", "text": "识别中…", "final": True})

    text = await asyncio.to_thread(transcribe_wav, wav_path)
    if not text:
        log.info("识别为空")
        await ws.send_json({"type": "say", "text": "没听清，再说一次", "final": True})
        await ws.send_json({"type": "state", "value": "idle"})
        return

    log.info("识别结果: %s", text)
    await ws.send_json({"type": "say", "text": "你：" + text[:SAY_CHAR_LIMIT], "final": True})

    async def push(acc: str) -> None:
        await ws.send_json({"type": "say", "text": acc[:SAY_CHAR_LIMIT], "final": False})

    try:
        reply = await ollama_stream(text, push)
    except Exception as exc:
        log.error("Ollama 失败: %s", exc)
        await ws.send_json({"type": "say", "text": "Ollama 未启动", "final": True})
        await ws.send_json({"type": "state", "value": "idle"})
        return

    await ws.send_json({"type": "say", "text": reply[:SAY_CHAR_LIMIT], "final": True})
    log.info("回复 %d 字", len(reply))

    reply = reply[:SAY_CHAR_LIMIT].strip()
    if not reply:
        await ws.send_json({"type": "state", "value": "idle"})
        return

    await ws.send_json({"type": "state", "value": "speaking"})
    audio_sec = 0.0
    try:
        pcm = await asyncio.to_thread(tts_pcm, reply)
        audio_sec = len(pcm) / 32000.0
        # 每帧 1600 字节（0.05 秒），保证小于设备端下行缓冲 2048
        # 广播给所有在线设备，不限于发起本次录音的那条连接
        recipients = 0
        for i in range(0, len(pcm), 1600):
            n = await broadcast_bytes(pcm[i:i + 1600])
            recipients = max(recipients, n)
        log.info("TTS 广播到 %d 个连接", recipients)
        log.info("TTS 下发 %d 字节 / %.1f 秒", len(pcm), len(pcm) / 32000.0)
    except Exception as exc:
        log.error("TTS 失败: %s", exc)
    await ws.send_json({"type": "state", "value": "idle"})

    history.appendleft(
        {
            "ts": time.strftime("%H:%M:%S"),
            "source": "device",
            "user": text,
            "reply": reply,
            "llm_ms": 0,
            "audio_sec": round(audio_sec, 2),
        }
    )


def handle_upstream(msg: dict[str, Any]) -> None:
    kind = msg.get("type")
    if kind == "hello":
        device_info.update(
            {
                "fw": msg.get("fw"),
                "chip": msg.get("chip"),
                "ip": msg.get("ip"),
                "rssi": msg.get("rssi"),
                "ssid": msg.get("ssid"),
                "uptime": msg.get("uptime"),
                "last_seen": time.strftime("%H:%M:%S"),
            }
        )
        log.info(
            "设备上线 fw=%s ip=%s ssid=%s rssi=%s",
            msg.get("fw"), msg.get("ip"), msg.get("ssid"), msg.get("rssi"),
        )
    elif kind in ("wifi.list", "wifi.result"):
        # 控制台的等待请求在这里被唤醒
        fut = pending.get(kind)
        if fut is not None and not fut.done():
            fut.set_result(msg)
        if kind == "wifi.result":
            log.info("WiFi 切换结果 ok=%s ssid=%s ip=%s",
                     msg.get("ok"), msg.get("ssid"), msg.get("ip"))
            if msg.get("ok"):
                device_info.update({"ip": msg.get("ip"), "ssid": msg.get("ssid")})
    elif kind == "key":
        log.info("按键 idx=%s %s", msg.get("idx"), msg.get("action"))
    elif kind == "listen.start":
        log.info("设备开始聆听（第 1 阶段无音频，等待 /say 或 /ask）")
    elif kind == "listen.stop":
        log.info("设备结束聆听")
    elif kind == "abort":
        log.info("设备请求打断")
    elif kind == "text":
        log.info("设备文本: %s", msg.get("text"))
    else:
        log.info("未知上行: %s", msg)


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket) -> None:
    await ws.accept()
    clients.add(ws)
    peer = ws.client.host if ws.client else "?"
    log.info("设备连接 %s，在线 %d", peer, len(clients))
    await ws.send_json({"type": "state", "value": "idle"})
    await ws.send_json({"type": "say", "text": "网关已连接，按键说话", "final": True})

    rec: wave.Wave_write | None = None
    rec_path: str | None = None
    rec_bytes = 0

    try:
        while True:
            message = await ws.receive()
            if message.get("type") == "websocket.disconnect":
                break

            # 二进制帧 = 一段录音 PCM，直接追加进当前 wav
            chunk = message.get("bytes")
            if chunk:
                if rec is not None:
                    rec.writeframes(chunk)
                    rec_bytes += len(chunk)
                continue

            raw = message.get("text") or ""
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                log.warning("非法 JSON: %.120s", raw)
                continue

            kind = msg.get("type")
            if kind == "listen.start":
                if rec is not None:
                    rec.close()
                rec_path = os.path.join(REC_DIR, time.strftime("rec_%Y%m%d_%H%M%S.wav"))
                rec = wave.open(rec_path, "wb")
                rec.setnchannels(1)
                rec.setsampwidth(2)
                rec.setframerate(16000)
                rec_bytes = 0
                log.info("开始录音 -> %s", rec_path)
                await ws.send_json({"type": "state", "value": "listening"})
                continue
            if kind in ("listen.stop", "abort"):
                finished_path = rec_path
                finished_bytes = rec_bytes
                if rec is not None:
                    rec.close()
                    log.info("录音结束 %s，%d 字节 / %.1f 秒", rec_path, finished_bytes,
                             finished_bytes / 32000.0)
                    rec = None
                await ws.send_json({"type": "state", "value": "idle"})
                # 至少 0.5 秒才送识别，避免误触
                if kind == "listen.stop" and finished_path and finished_bytes >= 16000:
                    asyncio.create_task(handle_utterance(ws, finished_path))
                continue

            handle_upstream(msg)
            if kind == "text":
                await ask(AskReq(text=str(msg.get("text", ""))))
    except WebSocketDisconnect:
        pass
    except Exception as exc:
        log.warning("连接异常: %s", exc)
    finally:
        if rec is not None:
            rec.close()
            log.info("录音中断保存 %s，%d 字节", rec_path, rec_bytes)
            rec = None
        clients.discard(ws)
        log.info("设备断开 %s，在线 %d", peer, len(clients))
