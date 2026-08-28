#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "intervention_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int red_gpio;
    int green_gpio;
    int blue_gpio;
    bool active_low;
    bool ws2812_enabled;
    int ws2812_gpio;
    size_t ws2812_count;
} status_light_config_t;

typedef struct {
    bool ready;
    bool ws2812_enabled;
    int ws2812_gpio;
    size_t ws2812_count;
    uint32_t ws2812_tx_count;
    esp_err_t ws2812_last_error;
    uint8_t last_red;
    uint8_t last_green;
    uint8_t last_blue;
} status_light_diag_t;

esp_err_t status_light_init(const status_light_config_t *config);
esp_err_t status_light_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t status_light_force_off(void);
esp_err_t status_light_sleep_assist_set(uint8_t red, uint8_t green, uint8_t blue, float brightness);
esp_err_t status_light_sleep_assist_set_local(uint8_t red, uint8_t green, uint8_t blue, float brightness);
esp_err_t status_light_ws2812_night_path(float progress, float max_brightness);
void status_light_get_diag(status_light_diag_t *diag);
void status_light_self_test(void);
void status_light_apply_decision(const intervention_decision_t *decision);

#ifdef __cplusplus
}
#endif
