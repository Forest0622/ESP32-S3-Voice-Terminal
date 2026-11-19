idf_component_register(
    SRCS
        "app_main.c"
        "board.c"
        "wifi_mgr.c"
        "ws_client.c"
        "ui.c"
        "font_cjk16.c"
        "es8388.c"
        "audio.c"
        "usb_link.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        esp_websocket_client
        esp_wifi
        esp_codec_dev
        nvs_flash
        esp_timer
        json
        lvgl
        spi_flash
        driver
)

# lv_font_conv 生成的字体文件默认 #include "lvgl/lvgl.h"，ESP-IDF 的 lvgl 组件只暴露 "lvgl.h"
set_source_files_properties("font_cjk16.c" PROPERTIES COMPILE_DEFINITIONS "LV_LVGL_H_INCLUDE_SIMPLE")
