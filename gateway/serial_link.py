"""USB 串口控制通道。

定位：WiFi 之外的**第二条**链路。板子插在电脑上时，不管板子和 Mac 在不在同一个
WiFi 网段、板子有没有配好网，控制台都能通过这根线给它下发指令（配网、音量、
播报文字），板子也能把状态回传。

帧格式与固件约定一致：以 `@@` 开头的单行 JSON，`\\n` 结尾。`@@` 前缀用来跟
UART 上混进来的调试日志区分——固件 console 走 UART0，USB 口是独立的。

音频（PCM）不走这条路，仍走 WiFi：USB 只做控制，避免把二进制解析塞进串口。
"""

from __future__ import annotations

import asyncio
import json
import logging
from typing import Any, Callable

import serial
from serial.tools import list_ports

log = logging.getLogger("usb")

PREFIX = b"@@"
PORT_HINTS = ("usbmodem", "usb jtag", "usb serial")


class SerialLink:
    def __init__(self) -> None:
        self.ser: serial.Serial | None = None
        self.port: str | None = None
        self.on_json: Callable[[dict[str, Any]], None] | None = None
        self._task: asyncio.Task | None = None
        self._want_stop = False

    # ---- 对外状态 ----

    @property
    def connected(self) -> bool:
        return self.ser is not None and self.ser.is_open

    def status(self) -> dict[str, Any]:
        return {"connected": self.connected, "port": self.port or ""}

    # ---- 生命周期 ----

    def start(self, on_json: Callable[[dict[str, Any]], None]) -> None:
        self.on_json = on_json
        if self._task is None or self._task.done():
            self._task = asyncio.create_task(self._supervise())

    def stop(self) -> None:
        self._want_stop = True
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass

    @staticmethod
    def _pick_port() -> str | None:
        candidates: list[str] = []
        for p in list_ports.comports():
            desc = (p.description or "").lower()
            dev = (p.device or "").lower()
            if any(h in dev or h in desc for h in PORT_HINTS):
                candidates.append(p.device)
        # 多个候选时优先 usbmodem（ESP32-S3 原生 USB 的典型命名）
        candidates.sort(key=lambda d: 0 if "usbmodem" in d.lower() else 1)
        return candidates[0] if candidates else None

    async def _supervise(self) -> None:
        while not self._want_stop:
            if not self.connected:
                port = self._pick_port()
                if port is None:
                    await asyncio.sleep(3)
                    continue
                try:
                    # 虚拟串口，波特率只是占位；timeout 给 readline 兜底
                    self.ser = serial.Serial(port, 115200, timeout=0.2, write_timeout=2)
                    self.port = port
                    log.info("USB 串口已连接 %s", port)
                    await self._read_loop()
                except Exception as exc:
                    log.warning("USB 串口不可用 %s: %s", port, exc)
                finally:
                    if self.ser is not None:
                        try:
                            self.ser.close()
                        except Exception:
                            pass
                    self.ser = None
                await asyncio.sleep(2)
            else:
                await asyncio.sleep(0.5)

    async def _read_loop(self) -> None:
        buf = b""
        while self.ser is not None and self.ser.is_open:
            try:
                chunk = await asyncio.to_thread(self.ser.read, 512)
            except Exception as exc:
                log.warning("USB 串口读取失败: %s", exc)
                return
            if not chunk:
                continue
            buf += chunk
            # 单条上限保护，防止乱码把缓冲撑爆
            if len(buf) > 8192:
                buf = buf[-2048:]
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                raw = raw.strip(b"\r")
                if not raw.startswith(PREFIX):
                    continue
                text = raw[len(PREFIX):].decode("utf-8", "ignore").strip()
                if not text:
                    continue
                try:
                    msg = json.loads(text)
                except json.JSONDecodeError:
                    log.warning("USB 上行非法 JSON: %.100s", text)
                    continue
                if self.on_json is not None:
                    try:
                        self.on_json(msg)
                    except Exception as exc:
                        log.warning("处理 USB 上行失败: %s", exc)

    # ---- 下发 ----

    def send_json(self, payload: dict[str, Any]) -> bool:
        """把一条下行消息写进串口。含 audio 字段的直接跳过（音频不走 USB）。"""
        if not self.connected or "audio" in payload:
            return False
        try:
            text = json.dumps(payload, ensure_ascii=False)
            self.ser.write(PREFIX + text.encode("utf-8") + b"\n")
            return True
        except Exception as exc:
            log.warning("USB 下发失败: %s", exc)
            return False


serial_link = SerialLink()
