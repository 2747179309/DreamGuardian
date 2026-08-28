#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bio_filter.h"
#include "ld6002_types.h"
#include "sleep_assist.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SLEEP_STATE_UNKNOWN = 0,
    SLEEP_STATE_AWAKE,
    SLEEP_STATE_TRANSITION,
    SLEEP_STATE_LIGHT_TREND,
    SLEEP_STATE_DEEP_TREND,
    SLEEP_STATE_OUT_OF_BED,
} sleep_state_t;

typedef struct {
    bool human_present;
    bool data_quality_ok;
    float breath_rate_bpm;
    float heart_rate_bpm;
    float range_cm;
    float breath_stability;
    float heart_stability;
    float motion_energy;
    uint32_t out_of_bed_events;
    uint32_t low_quality_windows;
} sleep_features_t;

typedef struct {
    sleep_state_t state;
    uint8_t sleep_score;
    uint8_t wake_risk;
    uint8_t stable_sleep_index;
    char reason[96];
} sleep_assessment_t;

void sleep_score_from_bio(const SleepBioState *bio,
                          const sleep_assist_status_t *assist,
                          sleep_features_t *features,
                          sleep_assessment_t *assessment);
void sleep_score_from_radar(const ld6002_snapshot_t *radar,
                            sleep_features_t *features,
                            sleep_assessment_t *assessment);

const char *sleep_state_to_name(sleep_state_t state);

#ifdef __cplusplus
}
#endif
