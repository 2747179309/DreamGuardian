#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "massage_config.h"

typedef struct {
    bool connected;
    bool running;
    uint8_t mode;
    uint8_t intensity;
    uint16_t remaining_minutes;
    uint8_t battery;
} massage_device_status_t;

size_t massage_protocol_build_start(uint8_t *buf);
size_t massage_protocol_build_stop(uint8_t *buf);
size_t massage_protocol_build_mode(uint8_t *buf, uint8_t mode);
size_t massage_protocol_build_intensity(uint8_t *buf, uint8_t intensity);
size_t massage_protocol_build_duration(uint8_t *buf, uint16_t minutes);
esp_err_t massage_protocol_parse_notify(const uint8_t *data, size_t len,
                                        massage_device_status_t *status);
