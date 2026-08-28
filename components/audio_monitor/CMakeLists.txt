idf_component_register(
    SRCS "audio_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_codec_dev esp-box-3 freertos)
