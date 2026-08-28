idf_component_register(
    SRCS "ld6002_parser.c" "ld6002_task.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos esp_timer debug_control
)
