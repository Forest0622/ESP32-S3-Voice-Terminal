#pragma once

/*
 * 设备 <-> 网关 WebSocket 协议（第 1 阶段：纯文本）
 *
 * 传输：ws://<mac>:8765/ws ，两个方向都用 UTF-8 JSON 文本帧。
 * 音频帧留到第 2 阶段，届时用二进制帧承载 PCM，与控制帧交错发送。
 *
 * 上行（设备 -> 网关）
 *   {"type":"hello","fw":"1.0.0","chip":"dc:b4:d9:16:b4:48"}
 *   {"type":"key","idx":0,"action":"down"}
 *   {"type":"key","idx":0,"action":"up"}
 *   {"type":"listen.start"}
 *   {"type":"listen.stop"}
 *   {"type":"abort"}
 *
 * 下行（网关 -> 设备）
 *   {"type":"say","text":"...","final":true}      final=false 表示流式追加
 *   {"type":"state","value":"idle|listening|thinking|speaking"}
 *   {"type":"clear"}
 *   {"type":"pong"}
 */

#define VOICE_FW_VERSION "1.5.3"

/* 下行 type 取值 */
#define MSG_SAY         "say"
#define MSG_STATE       "state"
#define MSG_CLEAR       "clear"
#define MSG_WIFI_SCAN   "wifi.scan"   /* 请求扫描周边热点 */
#define MSG_WIFI_SET    "wifi.set"    /* {ssid,password} 切换 WiFi 并写 NVS */
#define MSG_VOLUME      "volume.set"  /* {value:0~33} 设置喇叭模拟音量 */

/* 上行 type 取值（设备主动上报） */
#define MSG_WIFI_LIST   "wifi.list"
#define MSG_WIFI_RESULT "wifi.result"

/* 状态机取值，与 UI 状态条一一对应 */
#define ST_IDLE      "idle"
#define ST_LISTENING "listening"
#define ST_THINKING  "thinking"
#define ST_SPEAKING  "speaking"

/* 按键索引，和 board_keys_raw() 的 P17..P14 对应 */
#define KEY_IDX_0 0
#define KEY_IDX_1 1
#define KEY_IDX_2 2
#define KEY_IDX_3 3
