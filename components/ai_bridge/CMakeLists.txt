idf_component_register(
    SRCS "ai_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls json nvs_flash freertos sleep_assist algorithm
)
