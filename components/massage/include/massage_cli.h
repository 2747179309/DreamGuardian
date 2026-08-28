#pragma once
#include "esp_err.h"
/* Optional line parser; existing UART/console task can call this without coupling
 * itself to BLE or UUIDs. Input excludes the trailing newline. */
esp_err_t massage_cli_execute(const char *line);
