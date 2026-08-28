file(TO_CMAKE_PATH "$ENV{IDF_PATH}" IDF_PATH_CMAKE)

idf_component_register(
    SRCS "massage.c" "massage_ble.c" "massage_protocol.c" "massage_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${IDF_PATH_CMAKE}/components/bt/host/nimble/port/include"
    REQUIRES bt freertos
)
