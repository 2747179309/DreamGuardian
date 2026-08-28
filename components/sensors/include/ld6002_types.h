#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LD6002_TYPE_HUMAN_PRESENT 0x0F09
#define LD6002_TYPE_POSITION      0x0A04
#define LD6002_TYPE_PHASE         0x0A13
#define LD6002_TYPE_BREATH_RATE   0x0A14
#define LD6002_TYPE_HEART_RATE    0x0A15
#define LD6002_TYPE_RANGE         0x0A16
#define LD6002_TYPE_TRACK_POS     0x0A17

typedef struct {
    uint16_t id;
    uint16_t type;
    uint16_t len;
    const uint8_t *data;
} ld6002_frame_t;

typedef struct {
    bool human_present;
    bool range_valid;
    float breath_rate_bpm;
    float heart_rate_bpm;
    float total_phase;
    float breath_phase;
    float heart_phase;
    float range_cm;
    float x_m;
    float y_m;
    float z_m;
    int64_t last_byte_update_us;
    int64_t last_update_us;
    int64_t presence_update_us;
    int64_t phase_update_us;
    int64_t breath_update_us;
    int64_t heart_update_us;
    int64_t range_update_us;
    int64_t track_update_us;
    uint32_t uart_bytes;
    uint8_t last_byte;
    uint32_t raw_frames;
    uint32_t frames;
    uint32_t unknown_frames;
    uint32_t checksum_errors;
    uint32_t parse_errors;
    uint16_t last_type;
    uint16_t last_len;
} ld6002_snapshot_t;

typedef struct {
    uart_port_t uart_num;
    int tx_gpio;
    int rx_gpio;
    int baud_rate;
} ld6002_task_config_t;

#ifdef __cplusplus
}
#endif
