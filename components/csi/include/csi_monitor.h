#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    bool breath_proxy_valid;
    uint32_t packet_count;
    int8_t last_rssi;
    float amplitude_ema;
    float motion_index;
    float stability_score;
    float breath_proxy_bpm;
} csi_monitor_status_t;

esp_err_t csi_monitor_start(void);
void csi_monitor_get_status(csi_monitor_status_t *status);
void csi_monitor_reset(void);

#ifdef __cplusplus
}
#endif