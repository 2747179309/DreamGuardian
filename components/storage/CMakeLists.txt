idf_component_register(
    SRCS "sleep_log.c"
    INCLUDE_DIRS "include"
    REQUIRES sensors algorithm intervention nvs_flash
)
