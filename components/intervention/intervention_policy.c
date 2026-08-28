#include "intervention_policy.h"

#include <stdio.h>
#include <string.h>

const char *intervention_action_to_name(intervention_action_t action)
{
    switch (action) {
    case INTERVENTION_ACTION_PINK_NOISE:
        return "pink_noise";
    case INTERVENTION_ACTION_WARM_LIGHT:
        return "warm_light";
    case INTERVENTION_ACTION_STEREO_BREATHING:
        return "stereo_breathing";
    case INTERVENTION_ACTION_NIGHT_PATH:
        return "night_path";
    case INTERVENTION_ACTION_REDUCE_AROUSAL:
        return "reduce_arousal";
    case INTERVENTION_ACTION_WAKE_MUSIC:
        return "wake_music";
    default:
        return "none";
    }
}

static void set_decision(intervention_decision_t *decision,
                         intervention_action_t action,
                         uint8_t volume,
                         uint8_t brightness,
                         uint16_t duration,
                         const char *skill,
                         const char *reason)
{
    memset(decision, 0, sizeof(*decision));
    decision->action = action;
    decision->volume_percent = volume;
    decision->brightness_percent = brightness;
    decision->duration_sec = duration;
    snprintf(decision->skill_name, sizeof(decision->skill_name), "%s", skill);
    snprintf(decision->reason, sizeof(decision->reason), "%s", reason);
}

void intervention_decide(const sleep_features_t *features,
                         const sleep_assessment_t *assessment,
                         intervention_decision_t *decision)
{
    if (assessment->state == SLEEP_STATE_OUT_OF_BED) {
        set_decision(decision, INTERVENTION_ACTION_NIGHT_PATH, 0, 12, 180,
                     "light.night_path", "out-of-bed event detected");
        return;
    }

    if (!features->data_quality_ok) {
        set_decision(decision, INTERVENTION_ACTION_NONE, 0, 0, 0,
                     "none", "low data quality, observe only");
        return;
    }

    if (assessment->wake_risk >= 75) {
        set_decision(decision, INTERVENTION_ACTION_REDUCE_AROUSAL, 12, 8, 300,
                     "policy.reduce_arousal", "high wake risk, low-stimulus comfort");
        return;
    }

    if (assessment->wake_risk >= 55) {
        set_decision(decision, INTERVENTION_ACTION_STEREO_BREATHING, 22, 10, 240,
                     "audio.stereo_breathing", "breath or heart trend is unstable");
        return;
    }

    if (assessment->wake_risk >= 35) {
        set_decision(decision, INTERVENTION_ACTION_PINK_NOISE, 12, 0, 300,
                     "audio.play_pink_noise", "minor disturbance, mask environmental noise");
        return;
    }

    if (assessment->stable_sleep_index < 55) {
        set_decision(decision, INTERVENTION_ACTION_WARM_LIGHT, 0, 6, 180,
                     "light.warm_breathing", "sleep transition support");
        return;
    }

    set_decision(decision, INTERVENTION_ACTION_NONE, 0, 0, 0,
                 "none", "stable sleep trend, no intervention");
}
