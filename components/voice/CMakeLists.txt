idf_component_register(
    SRCS "voice_control.c"
    INCLUDE_DIRS "include"
    REQUIRES sensors intervention esp_timer esp_driver_uart esp_driver_usb_serial_jtag cloud feishu_agent audio voice_sr llm_intent online_asr debug_control storage
)
