idf_component_register(
    SRCS "sleep_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES sensors algorithm intervention voice bsp time_sync device_commands csi esp_lcd_touch esp_lvgl_port espressif__esp_lcd_touch_gt911 espressif__esp_lcd_touch_tt21100
)
