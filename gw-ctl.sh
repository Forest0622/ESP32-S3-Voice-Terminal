#!/bin/bash
# ESP32-S3 语音终端 - 网关控制
#
# 用途：网关由 launchd 托管（开机自启 + 崩溃自动拉起）。
#       烧录固件前必须先 stop，否则 launchd 会把网关拉回来占住串口，导致烧录失败。
#
# 用法：
#   ./gw-ctl.sh start     启动（已加载时等于拉起）
#   ./gw-ctl.sh stop      停止（真正的停止，不会被自动拉起）
#   ./gw-ctl.sh restart   重启
#   ./gw-ctl.sh status    查看进程与设备状态
#   ./gw-ctl.sh logs      跟踪日志

set -u

LABEL="com.xiaoyi.esp32voice.gateway"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/esp32-voice-gateway.log"
UID_NUM="$(id -u)"

case "${1:-status}" in
  start)
    if launchctl print "gui/$UID_NUM/$LABEL" >/dev/null 2>&1; then
      launchctl kickstart -k "gui/$UID_NUM/$LABEL"
      echo "已拉起（服务本来就加载着）"
    else
      launchctl bootstrap "gui/$UID_NUM" "$PLIST"
      echo "已加载并启动"
    fi
    ;;

  stop)
    # bootout 会同时取消 KeepAlive 的自动重启，烧录前必须用这个
    if launchctl print "gui/$UID_NUM/$LABEL" >/dev/null 2>&1; then
      launchctl bootout "gui/$UID_NUM/$LABEL"
      echo "已停止（不会被自动拉起，可以烧录了）"
    else
      echo "服务未加载"
    fi
    # 兜底：清掉可能残留的手动进程
    PID=$(lsof -nP -iTCP:8765 -sTCP:LISTEN -t 2>/dev/null | head -1)
    if [ -n "${PID:-}" ]; then
      kill -9 "$PID" 2>/dev/null
      echo "已清理残留进程 $PID"
    fi
    ;;

  restart)
    launchctl kickstart -k "gui/$UID_NUM/$LABEL"
    echo "已重启"
    ;;

  status)
    echo "--- launchd ---"
    if launchctl print "gui/$UID_NUM/$LABEL" >/dev/null 2>&1; then
      echo "已加载（开机自启生效）"
    else
      echo "未加载（开机不会自启，崩溃也不会自动拉起）"
    fi

    echo "--- 端口 ---"
    lsof -nP -iTCP:8765 -sTCP:LISTEN 2>/dev/null | tail -1 || echo "8765 无监听"

    echo "--- 设备 ---"
    RESP=$(curl -s -m 8 http://127.0.0.1:8765/api/status 2>/dev/null)
    if [ -z "$RESP" ]; then
      echo "网关不可达"
    else
      echo "$RESP" | python3 -c '
import sys, json
d = json.load(sys.stdin)
dev = d.get("device") or {}
usb = d.get("usb") or {}
print("online:", d.get("online"), "| usb:", usb.get("connected"), "| port:", usb.get("port"))
print("fw:", dev.get("fw"), "| ip:", dev.get("ip"), "| ssid:", dev.get("ssid"), "| rssi:", dev.get("rssi"))
print("server_url:", d.get("server_url"))
'
    fi
    ;;

  logs)
    echo "跟踪 $LOG（Ctrl+C 退出）"
    tail -f "$LOG"
    ;;

  *)
    echo "用法: $0 {start|stop|restart|status|logs}"
    exit 1
    ;;
esac
