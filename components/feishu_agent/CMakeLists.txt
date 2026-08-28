idf_component_register(
    SRCS "feishu_agent.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls esp_event esp_timer esp_websocket_client json nvs_flash freertos device_commands llm_intent storage
)
