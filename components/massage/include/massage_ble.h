#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t massage_ble_init(void);
esp_err_t massage_ble_scan_start(void);
esp_err_t massage_ble_connect(void);
esp_err_t massage_ble_disconnect(void);
bool massage_ble_is_connected(void);
esp_err_t massage_ble_write(const uint8_t *data, size_t len);
void massage_hex_dump(const char *tag, const uint8_t *data, size_t len);
