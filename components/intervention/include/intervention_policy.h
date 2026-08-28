#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sleep_score.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INTERVENTION_ACTION_NONE = 0,
    INTERVENTION_ACTION_PINK_NOISE,
    INTERVENTION_ACTION_WARM_LIGHT,
    INTERVENTION_ACTION_STEREO_BREATHING,
    INTERVENTION_ACTION_NIGHT_PATH,
    INTERVENTION_ACTION_REDUCE_AROUSAL,
    INTERVENTION_ACTION_WAKE_MUSIC,
} intervention_action_t;

typedef struct {
    intervention_action_t action;
    uint8_t volume_percent;
    uint8_t brightness_percent;
    uint16_t duration_sec;
    char skill_name[40];
    char reason[96];
} intervention_decision_t;

void intervention_decide(const sleep_features_t *features,
                         const sleep_assessment_t *assessment,
                         intervention_decision_t *decision);

const char *intervention_action_to_name(intervention_action_t action);

#ifdef __cplusplus
}
#endif
