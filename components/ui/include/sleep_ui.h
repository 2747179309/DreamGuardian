#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "device_commands.h"
#include "csi_monitor.h"
#include "intervention_policy.h"
#include "ld6002_types.h"
#include "sleep_score.h"
#include "voice_control.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*sleep_ui_command_cb_t)(mobile_app_command_t command, void *ctx);

typedef struct {
    bool display_ready;
    bool touch_ready;
    bool touch_indev_ready;
    int last_error;
    uint32_t press_count;
    uint32_t command_count;
    int16_t last_x;
    int16_t last_y;
    mobile_app_command_t last_command;
    char last_target[16];
} sleep_ui_status_t;

void sleep_ui_init(void);
void sleep_ui_register_command_callback(sleep_ui_command_cb_t cb, void *ctx);
void sleep_ui_set_sleep_mode_active(bool active);
void sleep_ui_get_status(sleep_ui_status_t *status);
void sleep_ui_self_test(void);
esp_err_t sleep_ui_set_scene_preset(const char *preset);
esp_err_t sleep_ui_sleep_assist_show(float phase, float brightness, bool text_visible);
esp_err_t sleep_ui_show_solid_color(uint32_t rgb, uint8_t brightness_percent, const char *label);
esp_err_t sleep_ui_show_clock_mode(void);
esp_err_t sleep_ui_show_sleep_locked_clock(void);
void sleep_ui_update_sensor_status(const ld6002_snapshot_t *radar,
                                   const csi_monitor_status_t *csi);
void sleep_ui_update(const ld6002_snapshot_t *radar,
                     const sleep_features_t *features,
                     const sleep_assessment_t *assessment,
                     const intervention_decision_t *decision);

#ifdef __cplusplus
}
#endif
