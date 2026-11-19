#!/usr/bin/env python3
"""命令行测试：往设备推一句话，或走 Ollama 生成"""

import argparse
import json
import sys

import httpx

BASE = "http://127.0.0.1:8765"


def post(path: str, payload: dict) -> None:
    with httpx.Client(timeout=120) as c:
        r = c.post(BASE + path, json=payload)
        print(f"{r.status_code} {json.dumps(r.json(), ensure_ascii=False)}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ask", action="store_true", help="走 Ollama 生成")
    ap.add_argument("--health", action="store_true", help="查网关状态")
    ap.add_argument("text", nargs="*", help="要发送的文本")
    args = ap.parse_args()

    if args.health:
        with httpx.Client(timeout=10) as c:
            print(json.dumps(c.get(BASE + "/health").json(), ensure_ascii=False, indent=2))
        return 0

    text = " ".join(args.text).strip()
    if not text:
        print("用法：python talk.py 你好   或   python talk.py --ask 介绍一下你自己")
        return 1

    post("/ask" if args.ask else "/say", {"text": text})
    return 0


if __name__ == "__main__":
    sys.exit(main())
