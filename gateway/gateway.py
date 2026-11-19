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
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from faster_whisper import WhisperModel
from pydantic import BaseModel

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s | %(message)s",
)
log = logging.getLogger("gateway")

OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://127.0.0.1:11434")
OLLAMA_MODEL = os.environ.get("OLLAMA_MODEL", "qwen2.5:7b-instruct-q4_K_M")
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


def tts_pcm(text: str) -> bytes:
    """用 macOS say 生成 16kHz / 16bit / 单声道裸 PCM"""
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        path = f.name
    try:
        subprocess.run(["say", "-o", path, "--data-format=LEI16@16000", text], check=True)
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
        "model": OLLAMA_MODEL,
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
                    "model": OLLAMA_MODEL,
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
                "model": OLLAMA_MODEL,
                "messages": [
                    {
                        "role": "system",
                        "content": "你是一个语音助手，回答必须用简体中文，控制在 60 字以内，短句口语化，适合直接朗读。",
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
    try:
        pcm = await asyncio.to_thread(tts_pcm, reply)
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


def handle_upstream(msg: dict[str, Any]) -> None:
    kind = msg.get("type")
    if kind == "hello":
        device_info.update({"fw": msg.get("fw"), "chip": msg.get("chip")})
        log.info("设备上线 fw=%s chip=%s", msg.get("fw"), msg.get("chip"))
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
