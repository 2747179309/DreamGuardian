idf_component_register(
    SRCS "online_chat.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls json nvs_flash spiffs
)
