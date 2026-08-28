idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES sensors algorithm intervention audio audio_monitor voice_sr ui storage voice lighting bio_filter sleep_assist voice_prompt cloud csi time_sync ai_bridge feishu_agent llm_intent online_asr online_chat spiffs debug_control
)


