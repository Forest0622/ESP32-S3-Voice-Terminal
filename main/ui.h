#pragma once

#include <stdbool.h>

/* 初始化 LVGL 与 ST7789 显示，必须在 board_init() 之后调用 */
void ui_init(void);

/* 状态条：idle / listening / thinking / speaking */
void ui_set_state(const char *state);

/* 右上角连接信息 */
void ui_set_conn(bool connected, const char *ip, int rssi);

/* 追加一条完整消息。role 取 "user" / "ai" / "sys" */
void ui_show_message(const char *role, const char *text);

/* 流式追加：final=false 时覆盖最后一条 ai 消息，final=true 时封版 */
void ui_append_stream(const char *text, bool final);

/* 清屏 */
void ui_clear(void);

/* LVGL 主循环任务，由 app_main 创建 */
void ui_task(void *arg);
