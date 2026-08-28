idf_component_register(
    SRCS "status_light.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_ledc esp_driver_rmt intervention
)
