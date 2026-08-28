idf_component_register(
    SRCS "voice_sr.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_codec_dev esp-box-3 esp-sr freertos nvs_flash debug_control audio_monitor
)
