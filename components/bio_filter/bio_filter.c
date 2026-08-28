#include "bio_filter.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

#define BIO_FILTER_MAX_SAMPLES 90
#define SECONDS_TO_US 1000000LL
#define MOTION_SAMPLE_INTERVAL_US 500000LL
#define ABSENCE_CLEAR_INTERVAL_US 1500000LL

static const char *TAG = "bio_filter";

typedef struct {
    int64_t t_us;
    float breath;
    float heart;
    float motion;
    bool breath_valid;
    bool heart_valid;
    bool motion_valid;
} bio_sample_t;

typedef struct {
    bio_filter_config_t cfg;
    bio_sample_t samples[BIO_FILTER_MAX_SAMPLES];
    uint16_t next;
    uint16_t count;
    SleepBioState state;
    float prev_breath;
    float prev_heart;
    float prev_motion;
    bool have_ema;
    int64_t last_motion_sample_us;
    int64_t absence_since_us;
    bool absence_cleared;
} bio_filter_context_t;

static bio_filter_context_t s_filter;

static float clampf_local(float value, float lo, float hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

void bio_filter_get_default_config(bio_filter_config_t *config)
{
    if (!config) {
        return;
    }
    config->breath_alpha = 0.08f;
    config->heart_alpha = 0.05f;
    config->motion_alpha = 0.20f;
    config->min_confidence = 0.55f;
    config->max_breath_jump_bpm = 5.0f;
    config->max_heart_jump_bpm = 18.0f;
    config->breath_window_sec = 25;
    config->heart_window_sec = 45;
    config->motion_window_sec = 8;
}

void bio_filter_init(const bio_filter_config_t *config)
{
    memset(&s_filter, 0, sizeof(s_filter));
    if (config) {
        s_filter.cfg = *config;
    } else {
        bio_filter_get_default_config(&s_filter.cfg);
    }
    if (s_filter.cfg.breath_alpha <= 0.0f) {
        s_filter.cfg.breath_alpha = 0.08f;
    }
    if (s_filter.cfg.heart_alpha <= 0.0f) {
        s_filter.cfg.heart_alpha = 0.05f;
    }
    if (s_filter.cfg.motion_alpha <= 0.0f) {
        s_filter.cfg.motion_alpha = 0.20f;
    }
    ESP_LOGI(TAG, "ready breath_window=%us heart_window=%us motion_window=%us",
             s_filter.cfg.breath_window_sec,
             s_filter.cfg.heart_window_sec,
             s_filter.cfg.motion_window_sec);
}

void bio_filter_reset(void)
{
    bio_filter_config_t cfg = s_filter.cfg;
    memset(&s_filter, 0, sizeof(s_filter));
    s_filter.cfg = cfg;
}

static bool value_in_range(float value, float lo, float hi)
{
    return isfinite(value) && value >= lo && value <= hi;
}

static bool jump_ok(float value, float previous, float max_jump, bool have_previous)
{
    if (!have_previous) {
        return true;
    }
    return fabsf(value - previous) <= max_jump;
}

static void push_sample(const bio_sample_t *sample)
{
    s_filter.samples[s_filter.next] = *sample;
    s_filter.next = (uint16_t)((s_filter.next + 1U) % BIO_FILTER_MAX_SAMPLES);
    if (s_filter.count < BIO_FILTER_MAX_SAMPLES) {
        s_filter.count++;
    }
}

static void sort_values(float *values, uint16_t n)
{
    for (uint16_t i = 1; i < n; ++i) {
        float key = values[i];
        int j = (int)i - 1;
        while (j >= 0 && values[j] > key) {
            values[j + 1] = values[j];
            j--;
        }
        values[j + 1] = key;
    }
}

static bool window_stats(int64_t now_us, uint16_t window_sec, int field,
                         float *center_out, float *std_out, float *oldest_out)
{
    float values[BIO_FILTER_MAX_SAMPLES];
    float sum = 0.0f;
    float sum2 = 0.0f;
    float oldest = NAN;
    int64_t oldest_t = INT64_MAX;
    uint16_t n = 0;
    int64_t min_t = now_us - (int64_t)window_sec * SECONDS_TO_US;

    for (uint16_t i = 0; i < s_filter.count; ++i) {
        const bio_sample_t *s = &s_filter.samples[i];
        if (s->t_us < min_t) {
            continue;
        }
        bool ok = false;
        float v = 0.0f;
        if (field == 0) {
            ok = s->breath_valid;
            v = s->breath;
        } else if (field == 1) {
            ok = s->heart_valid;
            v = s->heart;
        } else {
            ok = s->motion_valid;
            v = s->motion;
        }
        if (!ok || !isfinite(v) || n >= BIO_FILTER_MAX_SAMPLES) {
            continue;
        }
        values[n++] = v;
        sum += v;
        sum2 += v * v;
        if (s->t_us < oldest_t) {
            oldest_t = s->t_us;
            oldest = v;
        }
    }

    uint16_t min_samples = (field == 2) ? 2U : 4U;
    if (n < min_samples) {
        return false;
    }

    float mean = sum / (float)n;
    float var = (sum2 / (float)n) - mean * mean;
    if (var < 0.0f) {
        var = 0.0f;
    }

    sort_values(values, n);
    float center = values[n / 2U];
    if ((n & 1U) == 0U) {
        center = 0.5f * (values[n / 2U - 1U] + values[n / 2U]);
    }

    *center_out = center;
    *std_out = sqrtf(var);
    *oldest_out = oldest;
    return true;
}
static float ema(float previous, float input, float alpha, bool have_previous)
{
    return have_previous ? previous + alpha * (input - previous) : input;
}

bool bio_filter_update(const SleepBioRaw *raw, int64_t now_us, SleepBioState *state_out)
{
    if (!raw) {
        return false;
    }

    if (!raw->presence) {
        if (s_filter.absence_since_us == 0) {
            s_filter.absence_since_us = now_us;
        }
        if (!s_filter.absence_cleared &&
            now_us - s_filter.absence_since_us >= ABSENCE_CLEAR_INTERVAL_US) {
            memset(s_filter.samples, 0, sizeof(s_filter.samples));
            s_filter.next = 0;
            s_filter.count = 0;
            memset(&s_filter.state, 0, sizeof(s_filter.state));
            s_filter.prev_breath = 0.0f;
            s_filter.prev_heart = 0.0f;
            s_filter.prev_motion = 0.0f;
            s_filter.have_ema = false;
            s_filter.last_motion_sample_us = 0;
            s_filter.absence_cleared = true;
        }
        s_filter.state.presence = false;
        s_filter.state.valid = false;
        s_filter.state.stability_score = 0.0f;
        if (state_out) {
            *state_out = s_filter.state;
        }
        return false;
    }
    s_filter.absence_since_us = 0;
    s_filter.absence_cleared = false;

    bool confidence_ok = raw->confidence >= s_filter.cfg.min_confidence;
    bool breath_new = raw->breath_update_us <= 0 ||
                      raw->breath_update_us != s_filter.state.breath_update_us;
    bool heart_new = raw->heart_update_us <= 0 ||
                     raw->heart_update_us != s_filter.state.heart_update_us;
    bool breath_ok = breath_new && raw->presence && confidence_ok &&
                     value_in_range(raw->breath_bpm_raw, 4.0f, 30.0f) &&
                     jump_ok(raw->breath_bpm_raw, s_filter.state.breath_bpm_smooth,
                             s_filter.cfg.max_breath_jump_bpm, s_filter.have_ema);
    bool heart_ok = heart_new && raw->presence && confidence_ok &&
                    value_in_range(raw->heart_bpm_raw, 40.0f, 140.0f) &&
                    jump_ok(raw->heart_bpm_raw, s_filter.state.heart_bpm_smooth,
                            s_filter.cfg.max_heart_jump_bpm, s_filter.have_ema);
    bool motion_ok = raw->presence && confidence_ok && isfinite(raw->motion_raw) && raw->motion_raw >= 0.0f;

    if (breath_ok) {
        s_filter.state.breath_bpm_latest = raw->breath_bpm_raw;
        s_filter.state.breath_update_us = raw->breath_update_us > 0 ?
            raw->breath_update_us : now_us;
    }
    if (heart_ok) {
        s_filter.state.heart_bpm_latest = raw->heart_bpm_raw;
        s_filter.state.heart_update_us = raw->heart_update_us > 0 ?
            raw->heart_update_us : now_us;
    }

    bool sample_motion = motion_ok &&
        (s_filter.last_motion_sample_us == 0 ||
         now_us - s_filter.last_motion_sample_us >= MOTION_SAMPLE_INTERVAL_US);

    bio_sample_t sample = {
        .t_us = now_us,
        .breath = raw->breath_bpm_raw,
        .heart = raw->heart_bpm_raw,
        .motion = clampf_local(raw->motion_raw, 0.0f, 2.0f),
        .breath_valid = breath_ok,
        .heart_valid = heart_ok,
        .motion_valid = sample_motion,
    };
    if (breath_ok || heart_ok || sample_motion) {
        push_sample(&sample);
        if (sample_motion) {
            s_filter.last_motion_sample_us = now_us;
        }
    }

    float breath_mean = 0.0f;
    float breath_std = 99.0f;
    float breath_old = NAN;
    float heart_mean = 0.0f;
    float heart_std = 99.0f;
    float heart_old = NAN;
    float motion_mean = 0.0f;
    float motion_std = 99.0f;
    float motion_old = NAN;

    bool have_breath = window_stats(now_us, s_filter.cfg.breath_window_sec, 0, &breath_mean, &breath_std, &breath_old);
    bool have_heart = window_stats(now_us, s_filter.cfg.heart_window_sec, 1, &heart_mean, &heart_std, &heart_old);
    bool have_motion = window_stats(now_us, s_filter.cfg.motion_window_sec, 2, &motion_mean, &motion_std, &motion_old);

    if (raw->presence && confidence_ok && have_breath) {
        s_filter.state.breath_bpm_smooth = ema(s_filter.state.breath_bpm_smooth, breath_mean,
                                               s_filter.cfg.breath_alpha, s_filter.have_ema);
    }
    if (raw->presence && confidence_ok && have_heart) {
        s_filter.state.heart_bpm_smooth = ema(s_filter.state.heart_bpm_smooth, heart_mean,
                                              s_filter.cfg.heart_alpha, s_filter.have_ema);
    }
    if (raw->presence && confidence_ok && have_motion) {
        s_filter.state.motion_smooth = ema(s_filter.state.motion_smooth, motion_mean,
                                           s_filter.cfg.motion_alpha, s_filter.have_ema);
    }

    bool has_core = have_breath && have_heart && have_motion;
    if (raw->presence && confidence_ok && has_core) {
        s_filter.state.breath_trend = isfinite(breath_old) ? s_filter.state.breath_bpm_smooth - breath_old : 0.0f;
        s_filter.state.heart_trend = isfinite(heart_old) ? s_filter.state.heart_bpm_smooth - heart_old : 0.0f;
        s_filter.state.motion_trend = isfinite(motion_old) ? s_filter.state.motion_smooth - motion_old : 0.0f;
        float breath_stability = 1.0f - clampf_local(breath_std / 4.0f, 0.0f, 1.0f);
        float heart_stability = 1.0f - clampf_local(heart_std / 12.0f, 0.0f, 1.0f);
        float motion_stability = 1.0f - clampf_local(s_filter.state.motion_smooth / 0.25f, 0.0f, 1.0f);
        s_filter.state.stability_score = clampf_local((0.45f * breath_stability) +
                                                      (0.35f * heart_stability) +
                                                      (0.20f * motion_stability), 0.0f, 1.0f);
        s_filter.state.valid = true;
        s_filter.have_ema = true;
    } else if (!raw->presence) {
        s_filter.state.valid = false;
        s_filter.state.stability_score = 0.0f;
    } else {
        s_filter.state.valid = s_filter.have_ema;
        s_filter.state.stability_score *= 0.95f;
    }

    s_filter.state.presence = raw->presence;
    s_filter.prev_breath = s_filter.state.breath_bpm_smooth;
    s_filter.prev_heart = s_filter.state.heart_bpm_smooth;
    s_filter.prev_motion = s_filter.state.motion_smooth;

    if (state_out) {
        *state_out = s_filter.state;
    }
    return s_filter.state.valid;
}

void bio_filter_get_state(SleepBioState *state_out)
{
    if (state_out) {
        *state_out = s_filter.state;
    }
}
