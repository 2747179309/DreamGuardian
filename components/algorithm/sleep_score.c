#include "sleep_score.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static float clampf(float value, float min_value, float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return (uint8_t)value;
}

const char *sleep_state_to_name(sleep_state_t state)
{
    switch (state) {
    case SLEEP_STATE_AWAKE:
        return "awake";
    case SLEEP_STATE_TRANSITION:
        return "transition";
    case SLEEP_STATE_LIGHT_TREND:
        return "light_trend";
    case SLEEP_STATE_DEEP_TREND:
        return "deep_trend";
    case SLEEP_STATE_OUT_OF_BED:
        return "out_of_bed";
    default:
        return "unknown";
    }
}

static int score_cap_for_assist(const sleep_assist_status_t *assist)
{
    if (!assist || !assist->active) {
        return 65;
    }
    switch (assist->state) {
    case SLEEP_ASSIST_BASELINE:
        return 35;
    case SLEEP_ASSIST_SETTLE:
        return 55;
    case SLEEP_ASSIST_ENTRAIN:
        return 78;
    case SLEEP_ASSIST_FADE:
        return 88;
    case SLEEP_ASSIST_SLEEP_LOCK:
        return 96;
    default:
        return 45;
    }
}

static float positive_drop_ratio(float baseline, float current)
{
    if (!isfinite(baseline) || baseline <= 1.0f || !isfinite(current)) {
        return 0.0f;
    }
    return clampf((baseline - current) / baseline, -0.25f, 0.35f);
}

void sleep_score_from_bio(const SleepBioState *bio,
                          const sleep_assist_status_t *assist,
                          sleep_features_t *features,
                          sleep_assessment_t *assessment)
{
    static uint8_t displayed_score;
    static uint8_t displayed_risk = 70;
    static sleep_assist_state_t last_state = SLEEP_ASSIST_OFF;
    static bool last_active;

    memset(features, 0, sizeof(*features));
    memset(assessment, 0, sizeof(*assessment));

    bool active = assist && assist->active;
    sleep_assist_state_t state = assist ? assist->state : SLEEP_ASSIST_OFF;
    if (active && (!last_active || state == SLEEP_ASSIST_BASELINE) && last_state != state) {
        if (displayed_score > 25) {
            displayed_score = 25;
        }
        if (displayed_risk < 60) {
            displayed_risk = 60;
        }
    }
    last_active = active;
    last_state = state;

    if (!bio || !bio->presence) {
        assessment->sleep_score = 0;
        assessment->wake_risk = 95;
        assessment->stable_sleep_index = 0;
        assessment->state = SLEEP_STATE_OUT_OF_BED;
        snprintf(assessment->reason, sizeof(assessment->reason), "out of bed or no human detected");
        displayed_score = assessment->sleep_score;
        displayed_risk = assessment->wake_risk;
        return;
    }

    features->human_present = bio->presence;
    features->breath_rate_bpm = bio->breath_bpm_smooth;
    features->heart_rate_bpm = bio->heart_bpm_smooth;
    features->motion_energy = bio->motion_smooth;
    features->data_quality_ok = bio->valid;
    features->breath_stability = clampf(bio->stability_score * 100.0f, 0.0f, 100.0f);
    features->heart_stability = clampf((1.0f - fabsf(bio->heart_trend) / 12.0f) * 100.0f, 0.0f, 100.0f);

    if (!bio->valid) {
        int target_score = active ? 18 : 25;
        int target_risk = active ? 78 : 70;
        displayed_score = (uint8_t)target_score;
        displayed_risk = (uint8_t)target_risk;
        assessment->sleep_score = displayed_score;
        assessment->wake_risk = displayed_risk;
        assessment->stable_sleep_index = 0;
        assessment->state = SLEEP_STATE_UNKNOWN;
        snprintf(assessment->reason, sizeof(assessment->reason), "filtered bio data is not yet valid");
        return;
    }

    float baseline_breath = assist && assist->baseline_breath > 1.0f ? assist->baseline_breath : bio->breath_bpm_smooth;
    float baseline_heart = assist && assist->baseline_heart > 1.0f ? assist->baseline_heart : bio->heart_bpm_smooth;
    float breath_drop = positive_drop_ratio(baseline_breath, bio->breath_bpm_smooth);
    float heart_drop = positive_drop_ratio(baseline_heart, bio->heart_bpm_smooth);
    float breath_progress = clampf(breath_drop / 0.25f, 0.0f, 1.0f);
    float heart_progress = clampf(heart_drop / 0.10f, 0.0f, 1.0f);
    float stability = clampf(bio->stability_score, 0.0f, 1.0f);
    float low_motion = 1.0f - clampf(bio->motion_smooth / 0.18f, 0.0f, 1.0f);
float candidate_progress = assist ? clampf(assist->sleep_candidate_sec / 240.0f, 0.0f, 1.0f) : 0.0f;

    float rising_penalty = 0.0f;
    if (bio->breath_trend > 0.8f) {
        rising_penalty += clampf(bio->breath_trend * 5.0f, 0.0f, 18.0f);
    }
    if (bio->heart_trend > 3.0f) {
        rising_penalty += clampf((bio->heart_trend - 3.0f) * 2.0f, 0.0f, 16.0f);
    }
    float motion_penalty = clampf((bio->motion_smooth - 0.08f) / 0.20f, 0.0f, 1.0f) * 28.0f;

    int target_score;
    if (active && state == SLEEP_ASSIST_BASELINE) {
        target_score = 20 + (int)(stability * 10.0f) + (int)(low_motion * 5.0f);
    } else if (active && state == SLEEP_ASSIST_SLEEP_LOCK) {
        target_score = 92 + (int)(stability * 6.0f);
    } else {
        target_score = 18 +
                       (int)(stability * 22.0f) +
                       (int)(low_motion * 14.0f) +
                       (int)(breath_progress * 22.0f) +
                       (int)(heart_progress * 14.0f) +
                       (int)(candidate_progress * 18.0f) -
                       (int)(rising_penalty + motion_penalty);
    }

    int cap = score_cap_for_assist(assist);
    target_score = clamp_u8(target_score);
    if (target_score > cap) {
        target_score = cap;
    }

    int target_risk = 100 - target_score + (int)(motion_penalty * 0.8f) + (int)(rising_penalty * 0.9f);
    if (active && state == SLEEP_ASSIST_BASELINE && target_risk < 58) {
        target_risk = 58;
    }
    target_risk = clamp_u8(target_risk);

    int max_score_step = active ? 2 : 5;
    int max_risk_step = active ? 4 : 8;
    int score_delta = target_score - displayed_score;
    if (score_delta > max_score_step) {
        score_delta = max_score_step;
    } else if (score_delta < -max_risk_step) {
        score_delta = -max_risk_step;
    }
    int risk_delta = target_risk - displayed_risk;
    if (risk_delta > max_risk_step) {
        risk_delta = max_risk_step;
    } else if (risk_delta < -max_score_step) {
        risk_delta = -max_score_step;
    }
    displayed_score = clamp_u8((int)displayed_score + score_delta);
    displayed_risk = clamp_u8((int)displayed_risk + risk_delta);

    assessment->sleep_score = displayed_score;
    assessment->wake_risk = displayed_risk;
    assessment->stable_sleep_index = clamp_u8((int)(stability * 55.0f + low_motion * 25.0f + candidate_progress * 20.0f));

    if (active && state == SLEEP_ASSIST_BASELINE) {
        assessment->state = SLEEP_STATE_TRANSITION;
        snprintf(assessment->reason, sizeof(assessment->reason), "baseline collecting; sleep score is intentionally capped");
    } else if (displayed_score >= 82 && displayed_risk <= 25) {
        assessment->state = SLEEP_STATE_DEEP_TREND;
        snprintf(assessment->reason, sizeof(assessment->reason), "low motion with slower stable breath and heart trend");
    } else if (displayed_score >= 58 && displayed_risk <= 48) {
        assessment->state = SLEEP_STATE_LIGHT_TREND;
        snprintf(assessment->reason, sizeof(assessment->reason), "sleep transition improving against baseline");
    } else if (displayed_risk >= 65) {
        assessment->state = SLEEP_STATE_TRANSITION;
        snprintf(assessment->reason, sizeof(assessment->reason), "wake risk elevated by motion or rising vital trend");
    } else {
        assessment->state = SLEEP_STATE_AWAKE;
        snprintf(assessment->reason, sizeof(assessment->reason), "awake or early relaxation phase");
    }
}
void sleep_score_from_radar(const ld6002_snapshot_t *radar,
                            sleep_features_t *features,
                            sleep_assessment_t *assessment)
{
    static float prev_breath;
    static float prev_heart;
    static bool prev_present;
    static uint32_t out_of_bed_events;
    static uint32_t low_quality_windows;

    memset(features, 0, sizeof(*features));
    memset(assessment, 0, sizeof(*assessment));

    features->human_present = radar->human_present;
    features->breath_rate_bpm = radar->breath_rate_bpm;
    features->heart_rate_bpm = radar->heart_rate_bpm;
    features->range_cm = radar->range_cm;

    bool breath_ok = radar->breath_rate_bpm >= 6.0f && radar->breath_rate_bpm <= 35.0f;
    bool heart_ok = radar->heart_rate_bpm >= 35.0f && radar->heart_rate_bpm <= 130.0f;
    features->data_quality_ok = radar->human_present && breath_ok;

    if (!prev_present && radar->human_present) {
        prev_breath = radar->breath_rate_bpm;
        prev_heart = radar->heart_rate_bpm;
    }
    if (prev_present && !radar->human_present) {
        out_of_bed_events++;
    }

    float breath_delta = fabsf(radar->breath_rate_bpm - prev_breath);
    float heart_delta = fabsf(radar->heart_rate_bpm - prev_heart);

    if (features->data_quality_ok) {
        features->breath_stability = clampf(100.0f - breath_delta * 12.0f, 0.0f, 100.0f);
        features->heart_stability = heart_ok ? clampf(100.0f - heart_delta * 4.0f, 0.0f, 100.0f) : 50.0f;
        features->motion_energy = clampf(fabsf(radar->total_phase) * 0.05f, 0.0f, 100.0f);
    } else {
        low_quality_windows++;
        features->breath_stability = 0;
        features->heart_stability = 0;
        features->motion_energy = 80;
    }

    int wake_risk = 0;
    wake_risk += features->data_quality_ok ? 10 : 35;
    wake_risk += (int)clampf(breath_delta * 5.0f, 0.0f, 35.0f);
    wake_risk += heart_ok ? (int)clampf(heart_delta * 2.0f, 0.0f, 25.0f) : 15;
    wake_risk += radar->human_present ? 0 : 30;

    int stable_sleep_index = (int)((features->breath_stability * 0.55f) +
                                   (features->heart_stability * 0.25f) +
                                   (radar->human_present ? 20.0f : 0.0f));
    int sleep_score = stable_sleep_index - (wake_risk / 3) -
                      (int)clampf(out_of_bed_events * 4.0f, 0.0f, 20.0f);

    assessment->wake_risk = clamp_u8(wake_risk);
    assessment->stable_sleep_index = clamp_u8(stable_sleep_index);
    assessment->sleep_score = clamp_u8(sleep_score);

    if (!radar->human_present) {
        assessment->state = SLEEP_STATE_OUT_OF_BED;
        snprintf(assessment->reason, sizeof(assessment->reason), "out of bed or no human detected");
    } else if (!features->data_quality_ok) {
        assessment->state = SLEEP_STATE_UNKNOWN;
        snprintf(assessment->reason, sizeof(assessment->reason), "radar data quality is low");
    } else if (assessment->stable_sleep_index >= 78 && assessment->wake_risk < 35) {
        assessment->state = SLEEP_STATE_DEEP_TREND;
        snprintf(assessment->reason, sizeof(assessment->reason), "stable breathing and heart trend");
    } else if (assessment->stable_sleep_index >= 55) {
        assessment->state = SLEEP_STATE_LIGHT_TREND;
        snprintf(assessment->reason, sizeof(assessment->reason), "stable sleep window with minor fluctuation");
    } else if (assessment->wake_risk >= 65) {
        assessment->state = SLEEP_STATE_TRANSITION;
        snprintf(assessment->reason, sizeof(assessment->reason), "wake risk is rising, use low-stimulus comfort");
    } else {
        assessment->state = SLEEP_STATE_AWAKE;
        snprintf(assessment->reason, sizeof(assessment->reason), "awake or sleep transition trend");
    }

    features->out_of_bed_events = out_of_bed_events;
    features->low_quality_windows = low_quality_windows;
    prev_breath = radar->breath_rate_bpm;
    prev_heart = radar->heart_rate_bpm;
    prev_present = radar->human_present;
}
