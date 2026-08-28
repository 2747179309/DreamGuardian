#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool presence;
    float breath_bpm_raw;
    float heart_bpm_raw;
    int64_t breath_update_us;
    int64_t heart_update_us;
    float motion_raw;
    float confidence;
} SleepBioRaw;

typedef struct {
    bool presence;
    float breath_bpm_smooth;
    float heart_bpm_smooth;
    float breath_bpm_latest;
    float heart_bpm_latest;
    int64_t breath_update_us;
    int64_t heart_update_us;
    float motion_smooth;
    float breath_trend;
    float heart_trend;
    float motion_trend;
    float stability_score;
    bool valid;
} SleepBioState;

typedef struct {
    float breath_alpha;
    float heart_alpha;
    float motion_alpha;
    float min_confidence;
    float max_breath_jump_bpm;
    float max_heart_jump_bpm;
    uint16_t breath_window_sec;
    uint16_t heart_window_sec;
    uint16_t motion_window_sec;
} bio_filter_config_t;

void bio_filter_get_default_config(bio_filter_config_t *config);
void bio_filter_init(const bio_filter_config_t *config);
void bio_filter_reset(void);
bool bio_filter_update(const SleepBioRaw *raw, int64_t now_us, SleepBioState *state_out);
void bio_filter_get_state(SleepBioState *state_out);

#ifdef __cplusplus
}
#endif
