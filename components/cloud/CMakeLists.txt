idf_component_register(SRCS "mobile_app.c" INCLUDE_DIRS "include" REQUIRES esp_wifi esp_event esp_netif esp_http_server esp_http_client esp-tls json nvs_flash sensors algorithm intervention audio bio_filter sleep_assist csi time_sync ai_bridge feishu_agent device_commands audio_monitor voice_sr llm_intent online_asr spiffs lighting ui storage)

