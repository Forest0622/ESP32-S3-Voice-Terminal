#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "protocol.h"

static const char *TAG = "UI";

/* GB2312 全字库，由 lv_font_conv 从 Arial Unicode 生成，见 main/font_cjk16.c */
LV_FONT_DECLARE(font_cjk16);

#define SCREEN_W 320
#define SCREEN_H 240
#define BAR_H    26
#define FOOT_H   28
#define BUF_LINES 40
#define MSG_MAX  8
#define MSG_CAP  620

static SemaphoreHandle_t s_lock;
static lv_obj_t *s_state_label;
static lv_obj_t *s_conn_label;
static lv_obj_t *s_chat;
static lv_obj_t *s_msgs[MSG_MAX];
static int s_msg_count;
static bool s_stream_open;

static void ui_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void ui_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/* ---------------- 显示驱动 ---------------- */

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = board_panel();
    if (panel != NULL) {
        esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
        /* board.c 的 on_color_trans_done 会给一次信号量；同步等待，避免下一帧改写仍在发送的缓冲 */
        board_lcd_wait_idle(200);
    }
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ---------------- 样式 ---------------- */

static void apply_cjk_font(lv_obj_t *obj)
{
    lv_obj_set_style_text_font(obj, &font_cjk16, 0);
}

void ui_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(SCREEN_W, SCREEN_H);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lv_display_create 失败");
        return;
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, flush_cb);

    /* 两个半屏宽度的 DrawBuffer 放内部 RAM，SPI DMA 不能安全读 PSRAM */
    size_t buf_px = SCREEN_W * 40;
    size_t buf_sz = buf_px * sizeof(lv_color16_t);
    void *b1 = heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    void *b2 = heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (b1 == NULL || b2 == NULL) {
        ESP_LOGE(TAG, "DrawBuffer 分配失败，退回单缓冲");
        if (b1 == NULL) {
            b1 = heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL);
        }
        lv_display_set_buffers(disp, b1, NULL, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    } else {
        lv_display_set_buffers(disp, b1, b2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    }
    /* 注册默认显示，供 lv_screen_active() 使用 */
    lv_display_set_default(disp);

    /* ---- 顶栏 ---- */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F1116), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, SCREEN_W, BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1A2430), 0);
    lv_obj_set_style_pad_left(bar, 8, 0);
    lv_obj_set_style_pad_right(bar, 8, 0);
    lv_obj_set_style_pad_top(bar, 3, 0);
    lv_obj_set_style_pad_bottom(bar, 3, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "语音终端");
    apply_cjk_font(title);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8FB8DE), 0);

    s_state_label = lv_label_create(bar);
    lv_label_set_text(s_state_label, "空闲");
    apply_cjk_font(s_state_label);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0x7FD68A), 0);

    /* ---- 对话区 ---- */
    s_chat = lv_obj_create(scr);
    lv_obj_set_size(s_chat, SCREEN_W, SCREEN_H - BAR_H - FOOT_H);
    lv_obj_set_pos(s_chat, 0, BAR_H);
    lv_obj_set_style_radius(s_chat, 0, 0);
    lv_obj_set_style_border_width(s_chat, 0, 0);
    lv_obj_set_style_bg_opa(s_chat, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(s_chat, 8, 0);
    lv_obj_set_style_pad_right(s_chat, 8, 0);
    lv_obj_set_style_pad_top(s_chat, 6, 0);
    lv_obj_set_style_pad_row(s_chat, 6, 0);
    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chat, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(s_chat, LV_SCROLLBAR_MODE_OFF);

    /* ---- 底栏 ---- */
    lv_obj_t *foot = lv_obj_create(scr);
    lv_obj_set_size(foot, SCREEN_W, FOOT_H);
    lv_obj_set_pos(foot, 0, SCREEN_H - FOOT_H);
    lv_obj_set_style_radius(foot, 0, 0);
    lv_obj_set_style_border_width(foot, 0, 0);
    lv_obj_set_style_bg_color(foot, lv_color_hex(0x161C24), 0);
    lv_obj_set_style_pad_left(foot, 8, 0);
    lv_obj_set_style_pad_right(foot, 8, 0);
    lv_obj_set_style_pad_top(foot, 4, 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *hint = lv_label_create(foot);
    lv_label_set_text(hint, "K0 说话   K1 打断");
    apply_cjk_font(hint);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x6C7A8A), 0);

    s_conn_label = lv_label_create(foot);
    lv_label_set_text(s_conn_label, "未连接");
    apply_cjk_font(s_conn_label);
    lv_obj_set_style_text_color(s_conn_label, lv_color_hex(0x6C7A8A), 0);

    board_backlight(true);
    ESP_LOGI(TAG, "LVGL 界面就绪 %dx%d", SCREEN_W, SCREEN_H);
}

/* ---------------- 对外操作 ---------------- */

void ui_set_state(const char *state)
{
    if (s_state_label == NULL) {
        return;
    }
    const char *txt = "空闲";
    uint32_t color = 0x7FD68A;
    if (state == NULL) {
        state = ST_IDLE;
    }
    if (strcmp(state, ST_LISTENING) == 0) {
        txt = "聆听中";
        color = 0x69B7FF;
    } else if (strcmp(state, ST_THINKING) == 0) {
        txt = "思考中";
        color = 0xF0B44C;
    } else if (strcmp(state, ST_SPEAKING) == 0) {
        txt = "回答中";
        color = 0xB68CFF;
    }
    ui_lock();
    lv_label_set_text(s_state_label, txt);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(color), 0);
    ui_unlock();
}

void ui_set_conn(bool connected, const char *ip, int rssi)
{
    if (s_conn_label == NULL) {
        return;
    }
    char buf[48];
    if (connected) {
        snprintf(buf, sizeof(buf), "%s %ddBm", ip ? ip : "?", rssi);
    } else {
        snprintf(buf, sizeof(buf), "未连接");
    }
    ui_lock();
    lv_label_set_text(s_conn_label, buf);
    lv_obj_set_style_text_color(s_conn_label,
                                lv_color_hex(connected ? 0x7FD68A : 0x6C7A8A), 0);
    ui_unlock();
}

static void trim_oldest(void)
{
    if (s_msg_count < MSG_MAX) {
        return;
    }
    lv_obj_delete(s_msgs[0]);
    for (int i = 1; i < s_msg_count; i++) {
        s_msgs[i - 1] = s_msgs[i];
    }
    s_msg_count--;
    for (int i = s_msg_count; i < MSG_MAX; i++) {
        s_msgs[i] = NULL;
    }
}

static lv_obj_t *new_msg_label(uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(s_chat);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    apply_cjk_font(lbl);
    s_msgs[s_msg_count++] = lbl;
    lv_obj_scroll_to_view(lbl, LV_ANIM_OFF);
    return lbl;
}

void ui_show_message(const char *role, const char *text)
{
    if (s_chat == NULL || text == NULL) {
        return;
    }
    uint32_t color = 0xE6E8EC;
    if (role != NULL && strcmp(role, "user") == 0) {
        color = 0x69B7FF;
    } else if (role != NULL && strcmp(role, "sys") == 0) {
        color = 0x8A96A6;
    }

    ui_lock();
    s_stream_open = false;
    trim_oldest();
    lv_obj_t *lbl = new_msg_label(color);
    lv_label_set_text(lbl, text);
    ui_unlock();
}

void ui_append_stream(const char *text, bool final)
{
    if (s_chat == NULL) {
        return;
    }
    ui_lock();
    lv_obj_t *lbl = NULL;
    if (s_stream_open && s_msg_count > 0) {
        lbl = s_msgs[s_msg_count - 1];
    } else {
        trim_oldest();
        lbl = new_msg_label(0xE6E8EC);
        s_stream_open = true;
    }
    if (text != NULL) {
        char buf[MSG_CAP];
        snprintf(buf, sizeof(buf), "%s", text);
        lv_label_set_text(lbl, buf);
    }
    lv_obj_scroll_to_view(lbl, LV_ANIM_OFF);
    if (final) {
        s_stream_open = false;
    }
    ui_unlock();
}

void ui_clear(void)
{
    if (s_chat == NULL) {
        return;
    }
    ui_lock();
    for (int i = 0; i < s_msg_count; i++) {
        if (s_msgs[i] != NULL) {
            lv_obj_delete(s_msgs[i]);
            s_msgs[i] = NULL;
        }
    }
    s_msg_count = 0;
    s_stream_open = false;
    ui_unlock();
}

void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LVGL 任务启动");
    while (1) {
        ui_lock();
        uint32_t need = lv_timer_handler();
        ui_unlock();
        if (need > 20) {
            need = 20;
        }
        vTaskDelay(pdMS_TO_TICKS(need > 0 ? need : 1));
    }
}
