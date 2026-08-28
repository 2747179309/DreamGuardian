#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    MASSAGE_STATE_IDLE = 0, MASSAGE_STATE_SCANNING, MASSAGE_STATE_CONNECTING,
    MASSAGE_STATE_DISCOVERING, MASSAGE_STATE_READY, MASSAGE_STATE_RUNNING,
    MASSAGE_STATE_DISCONNECTED, MASSAGE_STATE_ERROR
} massage_state_t;

esp_err_t massage_init(void);
esp_err_t massage_scan_start(void);
esp_err_t massage_connect(void);
esp_err_t massage_disconnect(void);
esp_err_t massage_start(void);
esp_err_t massage_stop(void);
esp_err_t massage_set_mode(uint8_t mode);
esp_err_t massage_set_intensity(uint8_t intensity);
esp_err_t massage_set_duration(uint16_t minutes);
bool massage_is_connected(void);
massage_state_t massage_get_state(void);
esp_err_t massage_send_raw(const uint8_t *data, size_t len);
esp_err_t massage_send_hex_string(const char *hex_string);
