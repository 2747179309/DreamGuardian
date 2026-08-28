idf_component_register(
    SRCS "llm_intent.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls json nvs_flash device_commands
)
