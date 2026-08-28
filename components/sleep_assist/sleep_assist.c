#include "sleep_assist.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "massage.h"
#include "massage_config.h"

#define PI_F 3.14159265358979323846f
#define US_TO_SEC 0.000001f
#define MASSAGE_ASSIST_RUN_SEC 10

static const char *TAG = "sleep_assist";

typedef struct {
    sleep_assist_config_t cfg;
    sleep_assist_status_t status;
    int64_t state_started_us;
    int64_t session_started_us;
    int64_t last_tick_us;
    int64_t last_presence_us;
    float baseline_breath_sum;
    float baseline_heart_sum;
    float baseline_motion_sum;
    uint32_t baseline_count;
    float phase;
    float breath_at_progress_check;
    float no_breath_drop_sec;
    bool prompted_start;
    bool prompted_entrain;
    bool prompted_fade;
    bool massage_init_attempted;
    bool massage_auto_started;
    bool massage_auto_completed;
    int64_t massage_started_us;
} sleep_assist_context_t;

static sleep_assist_context_t s_assist;

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

static float rate_limit(float current, float target, float max_delta)
{
    if (target > current + max_delta) {
        return current + max_delta;
    }
    if (target < current - max_delta) {
        return current - max_delta;
    }
    return target;
}

static float state_elapsed_sec(int64_t now_us)
{
    return (float)(now_us - s_assist.state_started_us) * US_TO_SEC;
}

static float session_elapsed_sec(int64_t now_us)
{
    return (float)(now_us - s_assist.session_started_us) * US_TO_SEC;
}

void sleep_assist_get_default_config(sleep_assist_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->baseline_sec = 60;
    config->min_total_sec = 5 * 60;
    config->max_total_sec = 30 * 60;
    config->settle_sec = 4 * 60;
    config->entrain_sec = 12 * 60;
    config->fade_sec = 4 * 60;
    config->presence_lost_abort_sec = 60;
    config->sleep_candidate_required_sec = 4 * 60;
    config->no_breath_drop_rollback_sec = 120;
    config->final_target_breath_bpm = 5.0f;
    config->min_target_breath_bpm = 4.5f;
    config->max_target_breath_bpm = 5.5f;
    config->max_breath_target_change_per_sec = 0.006f;
    config->max_led_brightness_change_per_sec = 0.006f;
    config->max_screen_brightness_change_per_sec = 0.04f;
    config->max_audio_volume_change_per_sec = 0.02f;
    config->max_audio_volume = 0.12f;
    config->pan_depth = 0.05f;
    config->voice_prompt_enabled = true;
}

void sleep_assist_init(const sleep_assist_config_t *config)
{
    memset(&s_assist, 0, sizeof(s_assist));
    if (config) {
        s_assist.cfg = *config;
    } else {
        sleep_assist_get_default_config(&s_assist.cfg);
    }
    s_assist.cfg.max_audio_volume = clampf_local(s_assist.cfg.max_audio_volume, 0.0f, 0.12f);
    s_assist.cfg.pan_depth = clampf_local(s_assist.cfg.pan_depth, 0.0f, 0.10f);
    if (s_assist.cfg.max_breath_target_change_per_sec <= 0.0f) {
        s_assist.cfg.max_breath_target_change_per_sec = 0.01f;
    }
    if (s_assist.cfg.sleep_candidate_required_sec < 180) {
        s_assist.cfg.sleep_candidate_required_sec = 180;
    }
    if (s_assist.cfg.sleep_candidate_required_sec > 300) {
        s_assist.cfg.sleep_candidate_required_sec = 300;
    }
    s_assist.status.state = SLEEP_ASSIST_OFF;
    ESP_LOGI(TAG, "ready total=%us settle=%us entrain=%us fade=%us",
             s_assist.cfg.settle_sec + s_assist.cfg.entrain_sec + s_assist.cfg.fade_sec,
             s_assist.cfg.settle_sec,
             s_assist.cfg.entrain_sec,
             s_assist.cfg.fade_sec);
}

esp_err_t sleep_assist_set_breath_mode(const char *mode)
{
    float target = 5.0f;
    float min_target = 4.5f;
    float max_target = 5.5f;
    if (mode && strcmp(mode, "box") == 0) {
        target = 5.5f;
        min_target = 5.0f;
        max_target = 6.0f;
    } else if (mode && strcmp(mode, "deep") == 0) {
        target = 4.0f;
        min_target = 4.0f;
        max_target = 6.0f;
    }
    s_assist.cfg.final_target_breath_bpm = target;
    s_assist.cfg.min_target_breath_bpm = min_target;
    s_assist.cfg.max_target_breath_bpm = max_target;
    if (s_assist.status.active && s_assist.status.target_breath_bpm > 0.1f) {
        s_assist.status.target_breath_bpm = clampf_local(s_assist.status.target_breath_bpm,
                                                         min_target, max_target);
    }
    ESP_LOGI(TAG, "breath mode=%s target=%.1f", mode ? mode : "relax", target);
    return ESP_OK;
}

static void set_state(sleep_assist_state_t state, int64_t now_us)
{
    if (s_assist.status.state == state) {
        return;
    }
    ESP_LOGI(TAG, "state %s -> %s", sleep_assist_state_name(s_assist.status.state), sleep_assist_state_name(state));
    s_assist.status.state = state;
    s_assist.state_started_us = now_us;
}

esp_err_t sleep_assist_start(int64_t now_us)
{
    memset(&s_assist.status, 0, sizeof(s_assist.status));
    s_assist.status.state = SLEEP_ASSIST_BASELINE;
    s_assist.status.active = true;
    s_assist.session_started_us = now_us;
    s_assist.state_started_us = now_us;
    s_assist.last_tick_us = now_us;
    s_assist.last_presence_us = now_us;
    s_assist.baseline_breath_sum = 0.0f;
    s_assist.baseline_heart_sum = 0.0f;
    s_assist.baseline_motion_sum = 0.0f;
    s_assist.baseline_count = 0;
    s_assist.breath_at_progress_check = 0.0f;
    s_assist.no_breath_drop_sec = 0.0f;
    s_assist.prompted_start = false;
    s_assist.prompted_entrain = false;
    s_assist.prompted_fade = false;
    s_assist.massage_auto_started = false;
    s_assist.massage_auto_completed = false;
    s_assist.massage_started_us = 0;
    if (!s_assist.massage_init_attempted) {
        esp_err_t massage_err = massage_init();
        s_assist.massage_init_attempted = true;
        ESP_LOGI(TAG, "massage assist init err=%s", esp_err_to_name(massage_err));
    }
    if (massage_is_connected()) {
        (void)massage_set_intensity(MASSAGE_DEFAULT_INTENSITY);
        if (massage_start() == ESP_OK) {
            s_assist.massage_auto_started = true;
            s_assist.massage_started_us = now_us;
            ESP_LOGI(TAG, "massage assist started for %u seconds", MASSAGE_ASSIST_RUN_SEC);
        }
    } else {
        (void)massage_connect();
        ESP_LOGI(TAG, "massage assist waiting for BLE connection");
    }
    if (s_assist.cfg.voice_prompt_enabled && s_assist.cfg.voice_play) {
        (void)s_assist.cfg.voice_play(VOICE_PROMPT_SLEEP_START, 0.16f, now_us);
        s_assist.prompted_start = true;
    }
    ESP_LOGI(TAG, "started baseline=%us", s_assist.cfg.baseline_sec);
    return ESP_OK;
}

esp_err_t sleep_assist_start_locked(int64_t now_us)
{
    memset(&s_assist.status, 0, sizeof(s_assist.status));
    s_assist.status.state = SLEEP_ASSIST_SLEEP_LOCK;
    s_assist.status.active = true;
    s_assist.status.sleep_locked = true;
    s_assist.status.target_breath_bpm = s_assist.cfg.final_target_breath_bpm;
    s_assist.session_started_us = now_us;
    s_assist.state_started_us = now_us;
    s_assist.last_tick_us = now_us;
    s_assist.last_presence_us = now_us;
    s_assist.baseline_breath_sum = 0.0f;
    s_assist.baseline_heart_sum = 0.0f;
    s_assist.baseline_motion_sum = 0.0f;
    s_assist.baseline_count = 0;
    s_assist.breath_at_progress_check = 0.0f;
    s_assist.no_breath_drop_sec = 0.0f;
    s_assist.prompted_start = true;
    s_assist.prompted_entrain = true;
    s_assist.prompted_fade = true;
    if (s_assist.cfg.audio_set) {
        (void)s_assist.cfg.audio_set(0.0f, s_assist.status.target_breath_bpm, 0.0f, false);
    }
    if (s_assist.cfg.led_set) {
        (void)s_assist.cfg.led_set(0, 0, 0, 0.0f);
    }
    if (s_assist.cfg.screen_set) {
        (void)s_assist.cfg.screen_set(0.0f, 0.0f, false);
    }
    ESP_LOGI(TAG, "started locked sleep monitor");
    return ESP_OK;
}

esp_err_t sleep_assist_stop(int64_t now_us)
{
    (void)now_us;
    if (s_assist.massage_auto_started) {
        (void)massage_stop();
        s_assist.massage_auto_started = false;
        s_assist.massage_auto_completed = true;
        s_assist.massage_started_us = 0;
        ESP_LOGI(TAG, "massage assist stopped with sleep assist");
    }
    s_assist.status.state = SLEEP_ASSIST_OFF;
    s_assist.status.active = false;
    s_assist.status.sleep_locked = false;
    s_assist.status.audio_volume = 0.0f;
    s_assist.status.led_brightness = 0.0f;
    s_assist.status.screen_brightness = 0.0f;
    if (s_assist.cfg.audio_set) {
        (void)s_assist.cfg.audio_set(0.0f, 7.0f, 0.0f, false);
    }
    if (s_assist.cfg.led_set) {
        (void)s_assist.cfg.led_set(0, 0, 0, 0.0f);
    }
    if (s_assist.cfg.screen_set) {
        (void)s_assist.cfg.screen_set(0.0f, 0.0f, false);
    }
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

bool sleep_assist_is_active(void)
{
    return s_assist.status.active;
}

static float indicator_breath_curve(float phase)
{
    if (phase < 0.0f) {
        phase = 0.0f;
    } else if (phase >= 1.0f) {
        phase = fmodf(phase, 1.0f);
    }

    if (phase < 0.34f) {
        float x = phase / 0.34f;
        return 0.5f * (1.0f - cosf(PI_F * x));
    }
    if (phase < 0.40f) {
        return 1.0f;
    }
    if (phase < 0.92f) {
        float x = (phase - 0.40f) / 0.52f;
        return 0.5f * (1.0f + cosf(PI_F * x));
    }
    return 0.0f;
}

static void compute_baseline(const SleepBioState *bio)
{
    if (!bio || !bio->valid || !bio->presence) {
        return;
    }
    s_assist.baseline_breath_sum += bio->breath_bpm_smooth;
    s_assist.baseline_heart_sum += bio->heart_bpm_smooth;
    s_assist.baseline_motion_sum += bio->motion_smooth;
    s_assist.baseline_count++;
}

static void finish_baseline(const SleepBioState *bio, int64_t now_us)
{
    if (s_assist.baseline_count > 0) {
        s_assist.status.baseline_breath = s_assist.baseline_breath_sum / (float)s_assist.baseline_count;
        s_assist.status.baseline_heart = s_assist.baseline_heart_sum / (float)s_assist.baseline_count;
        s_assist.status.baseline_motion = s_assist.baseline_motion_sum / (float)s_assist.baseline_count;
    } else if (bio && bio->valid) {
        s_assist.status.baseline_breath = bio->breath_bpm_smooth;
        s_assist.status.baseline_heart = bio->heart_bpm_smooth;
        s_assist.status.baseline_motion = bio->motion_smooth;
    } else {
        s_assist.status.baseline_breath = 12.0f;
        s_assist.status.baseline_heart = 75.0f;
        s_assist.status.baseline_motion = 0.12f;
    }
    float initial = s_assist.status.baseline_breath * 0.90f;
    s_assist.status.target_breath_bpm = clampf_local(initial,
                                                     s_assist.cfg.min_target_breath_bpm,
                                                     s_assist.cfg.max_target_breath_bpm);
    s_assist.breath_at_progress_check = bio && bio->valid ? bio->breath_bpm_smooth : s_assist.status.baseline_breath;
    set_state(SLEEP_ASSIST_SETTLE, now_us);
}

static bool likely_sleep_condition(const SleepBioState *bio)
{
    if (!bio || !bio->valid || !bio->presence) {
        return false;
    }
    bool low_motion = bio->motion_smooth < 0.08f;
    bool breath_stable = fabsf(bio->breath_trend) < 2.0f;
    bool heart_ok = fabsf(bio->heart_trend) < 6.0f ||
                    (s_assist.status.baseline_heart > 0.0f &&
                     bio->heart_bpm_smooth <= s_assist.status.baseline_heart * 0.97f);
    return low_motion && breath_stable && heart_ok && bio->stability_score >= 0.60f;
}

static void update_sleep_candidate(const SleepBioState *bio, bool user_interaction, float dt_sec)
{
    if (likely_sleep_condition(bio) && !user_interaction) {
        s_assist.status.sleep_candidate_sec += dt_sec;
    } else if (bio && bio->presence && bio->motion_smooth < 0.18f && !user_interaction) {
        s_assist.status.sleep_candidate_sec -= dt_sec * 0.25f;
    } else {
        s_assist.status.sleep_candidate_sec -= dt_sec;
    }
    if (s_assist.status.sleep_candidate_sec < 0.0f) {
        s_assist.status.sleep_candidate_sec = 0.0f;
    }
}

static void update_target_breath(const SleepBioState *bio, float dt_sec)
{
    if (!bio || !bio->valid) {
        return;
    }

    float desired = s_assist.cfg.final_target_breath_bpm;
    if (s_assist.status.target_breath_bpm <= 0.1f) {
        desired = bio->breath_bpm_smooth * 0.90f;
    }

    if (bio->breath_bpm_smooth < s_assist.breath_at_progress_check - 0.3f) {
        s_assist.breath_at_progress_check = bio->breath_bpm_smooth;
        s_assist.no_breath_drop_sec = 0.0f;
    } else {
        s_assist.no_breath_drop_sec += dt_sec;
    }

    if (s_assist.no_breath_drop_sec >= (float)s_assist.cfg.no_breath_drop_rollback_sec) {
        desired = bio->breath_bpm_smooth * 0.90f;
        s_assist.no_breath_drop_sec = 0.0f;
        s_assist.breath_at_progress_check = bio->breath_bpm_smooth;
    }

    desired = clampf_local(desired, s_assist.cfg.min_target_breath_bpm, s_assist.cfg.max_target_breath_bpm);
    float max_down = fminf(s_assist.cfg.max_breath_target_change_per_sec, 0.5f / 60.0f) * dt_sec;
    float max_up = s_assist.cfg.max_breath_target_change_per_sec * dt_sec;
    if (desired < s_assist.status.target_breath_bpm) {
        s_assist.status.target_breath_bpm = rate_limit(s_assist.status.target_breath_bpm, desired, max_down);
    } else {
        s_assist.status.target_breath_bpm = rate_limit(s_assist.status.target_breath_bpm, desired, max_up);
    }
}

static void desired_ranges(sleep_assist_state_t state,
                           float *audio, float *led_base, float *led_amp, float *screen)
{
    switch (state) {
    case SLEEP_ASSIST_BASELINE:
        *audio = 0.060f;
        *led_base = 0.010f;
        *led_amp = 0.020f;
        *screen = 0.24f;
        break;
    case SLEEP_ASSIST_SETTLE:
        *audio = 0.075f;
        *led_base = 0.012f;
        *led_amp = 0.060f;
        *screen = 0.28f;
        break;
    case SLEEP_ASSIST_ENTRAIN:
        *audio = 0.070f;
        *led_base = 0.010f;
        *led_amp = 0.042f;
        *screen = 0.20f;
        break;
    case SLEEP_ASSIST_FADE:
        *audio = 0.025f;
        *led_base = 0.006f;
        *led_amp = 0.018f;
        *screen = 0.08f;
        break;
    case SLEEP_ASSIST_SLEEP_LOCK:
    case SLEEP_ASSIST_ABORT:
    case SLEEP_ASSIST_OFF:
    default:
        *audio = 0.0f;
        *led_base = 0.0f;
        *led_amp = 0.0f;
        *screen = 0.0f;
        break;
    }
}

static void apply_outputs(const SleepBioState *bio, int64_t now_us, float dt_sec)
{
    float target = s_assist.status.target_breath_bpm > 0.1f ?
        s_assist.status.target_breath_bpm : s_assist.cfg.final_target_breath_bpm;
    float period = 60.0f / clampf_local(target, 3.5f, 8.0f);
    float elapsed = session_elapsed_sec(now_us);
    s_assist.phase = fmodf(elapsed, period) / period;
    float visual_curve = indicator_breath_curve(s_assist.phase);

    float desired_audio = 0.0f;
    float led_base = 0.0f;
    float led_amp = 0.0f;
    float desired_screen = 0.0f;
    desired_ranges(s_assist.status.state, &desired_audio, &led_base, &led_amp, &desired_screen);

    if (!bio || !bio->valid || bio->stability_score < 0.35f) {
        desired_audio = fminf(desired_audio, 0.05f);
        led_base = fminf(led_base, 0.010f);
        led_amp = fminf(led_amp, 0.028f);
    }

    if (s_assist.status.state == SLEEP_ASSIST_FADE) {
        float k = 1.0f - clampf_local(state_elapsed_sec(now_us) / (float)s_assist.cfg.fade_sec, 0.0f, 1.0f);
        desired_audio *= k;
        desired_screen *= k;
        led_base *= k;
        led_amp *= k;
    }
    if (s_assist.status.state == SLEEP_ASSIST_SLEEP_LOCK) {
        desired_audio = 0.0f;
        desired_screen = 0.0f;
        led_base = 0.0f;
        led_amp = 0.0f;
        s_assist.status.audio_volume = 0.0f;
        s_assist.status.led_brightness = 0.0f;
        s_assist.status.screen_brightness = 0.0f;
    }

    desired_audio = clampf_local(desired_audio, 0.0f, s_assist.cfg.max_audio_volume);
    float desired_led = clampf_local(led_base + led_amp * visual_curve, 0.0f, 0.075f);
    desired_screen = clampf_local(desired_screen * (0.82f + 0.18f * visual_curve), 0.0f, 0.32f);

    s_assist.status.audio_volume = rate_limit(s_assist.status.audio_volume, desired_audio,
                                              s_assist.cfg.max_audio_volume_change_per_sec * dt_sec);
    s_assist.status.led_brightness = rate_limit(s_assist.status.led_brightness, desired_led,
                                                s_assist.cfg.max_led_brightness_change_per_sec * dt_sec);
    s_assist.status.screen_brightness = rate_limit(s_assist.status.screen_brightness, desired_screen,
                                                   s_assist.cfg.max_screen_brightness_change_per_sec * dt_sec);

    float progress = clampf_local(session_elapsed_sec(now_us) /
                                  (float)(s_assist.cfg.settle_sec + s_assist.cfg.entrain_sec + s_assist.cfg.fade_sec),
                                  0.0f, 1.0f);
    uint8_t red = (uint8_t)(255.0f + (120.0f - 255.0f) * progress);
    uint8_t green = (uint8_t)(70.0f + (18.0f - 70.0f) * progress);
    uint8_t blue = 0;

    if (s_assist.cfg.audio_set) {
        bool rhythm = s_assist.status.state != SLEEP_ASSIST_SLEEP_LOCK && s_assist.status.audio_volume > 0.0f;
        (void)s_assist.cfg.audio_set(s_assist.status.audio_volume, target, s_assist.cfg.pan_depth, rhythm);
    }
    if (s_assist.cfg.led_set && s_assist.status.state != SLEEP_ASSIST_SLEEP_LOCK) {
        (void)s_assist.cfg.led_set(red, green, blue, s_assist.status.led_brightness);
    }
    if (s_assist.cfg.screen_set && s_assist.status.state != SLEEP_ASSIST_SLEEP_LOCK) {
        bool text_visible = session_elapsed_sec(now_us) < 30.0f;
        (void)s_assist.cfg.screen_set(s_assist.phase, s_assist.status.screen_brightness, text_visible);
    }
}

void sleep_assist_tick(const SleepBioState *bio, bool user_interaction, int64_t now_us)
{
    if (!s_assist.status.active) {
        return;
    }

    float dt_sec = (float)(now_us - s_assist.last_tick_us) * US_TO_SEC;
    if (dt_sec <= 0.0f || dt_sec > 10.0f) {
        dt_sec = 1.0f;
    }
    s_assist.last_tick_us = now_us;

    /* Additive safety hook: it does not alter any sleep-state decision. */
    if (!s_assist.massage_auto_started && !s_assist.massage_auto_completed && massage_is_connected() &&
        s_assist.status.state != SLEEP_ASSIST_SLEEP_LOCK) {
        if (massage_set_intensity(MASSAGE_DEFAULT_INTENSITY) == ESP_OK &&
            massage_start() == ESP_OK) {
            s_assist.massage_auto_started = true;
            s_assist.massage_started_us = now_us;
            ESP_LOGI(TAG, "massage assist connected and started for %u seconds", MASSAGE_ASSIST_RUN_SEC);
        }
    }
    if (s_assist.massage_auto_started &&
        now_us - s_assist.massage_started_us >= (int64_t)MASSAGE_ASSIST_RUN_SEC * 1000000LL) {
        (void)massage_stop();
        s_assist.massage_auto_started = false;
        s_assist.massage_auto_completed = true;
        s_assist.massage_started_us = 0;
        ESP_LOGI(TAG, "massage assist 10-second interval complete");
    }

    if (user_interaction) {
        set_state(SLEEP_ASSIST_ABORT, now_us);
    }

    if (bio && bio->presence) {
        s_assist.last_presence_us = now_us;
    } else if (s_assist.status.state != SLEEP_ASSIST_SLEEP_LOCK &&
               now_us - s_assist.last_presence_us > (int64_t)s_assist.cfg.presence_lost_abort_sec * 1000000LL) {
        set_state(SLEEP_ASSIST_ABORT, now_us);
    }

    switch (s_assist.status.state) {
    case SLEEP_ASSIST_BASELINE:
        compute_baseline(bio);
        if (state_elapsed_sec(now_us) >= (float)s_assist.cfg.baseline_sec) {
            finish_baseline(bio, now_us);
        }
        break;
    case SLEEP_ASSIST_SETTLE:
        update_target_breath(bio, dt_sec);
        update_sleep_candidate(bio, user_interaction, dt_sec);
        if (state_elapsed_sec(now_us) >= (float)s_assist.cfg.settle_sec) {
            set_state(SLEEP_ASSIST_ENTRAIN, now_us);
            if (s_assist.cfg.voice_prompt_enabled && !s_assist.prompted_entrain && s_assist.cfg.voice_play) {
                (void)s_assist.cfg.voice_play(VOICE_PROMPT_ENTRAIN_START, 0.14f, now_us);
                s_assist.prompted_entrain = true;
            }
        }
        break;
    case SLEEP_ASSIST_ENTRAIN:
        update_target_breath(bio, dt_sec);
        update_sleep_candidate(bio, user_interaction, dt_sec);
        if (state_elapsed_sec(now_us) >= (float)s_assist.cfg.entrain_sec) {
            set_state(SLEEP_ASSIST_FADE, now_us);
            if (s_assist.cfg.voice_prompt_enabled && !s_assist.prompted_fade && s_assist.cfg.voice_play) {
                (void)s_assist.cfg.voice_play(VOICE_PROMPT_FADE_START, 0.12f, now_us);
                s_assist.prompted_fade = true;
            }
        }
        break;
    case SLEEP_ASSIST_FADE:
        update_sleep_candidate(bio, user_interaction, dt_sec);
        if (state_elapsed_sec(now_us) >= (float)s_assist.cfg.fade_sec) {
            set_state(SLEEP_ASSIST_SLEEP_LOCK, now_us);
        }
        break;
    case SLEEP_ASSIST_SLEEP_LOCK:
        s_assist.status.sleep_locked = true;
        break;
    case SLEEP_ASSIST_ABORT:
        apply_outputs(bio, now_us, dt_sec);
        (void)sleep_assist_stop(now_us);
        return;
    case SLEEP_ASSIST_OFF:
    default:
        return;
    }

    if (s_assist.status.state == SLEEP_ASSIST_SETTLE ||
        s_assist.status.state == SLEEP_ASSIST_ENTRAIN ||
        s_assist.status.state == SLEEP_ASSIST_FADE) {
        if (s_assist.status.sleep_candidate_sec >= (float)s_assist.cfg.sleep_candidate_required_sec) {
            set_state(SLEEP_ASSIST_SLEEP_LOCK, now_us);
            s_assist.status.sleep_locked = true;
        }
    }

    if (session_elapsed_sec(now_us) > (float)s_assist.cfg.max_total_sec &&
        s_assist.status.state != SLEEP_ASSIST_SLEEP_LOCK) {
        set_state(SLEEP_ASSIST_SLEEP_LOCK, now_us);
    }

    apply_outputs(bio, now_us, dt_sec);
}

void sleep_assist_get_status(sleep_assist_status_t *status)
{
    if (status) {
        *status = s_assist.status;
    }
}

const char *sleep_assist_state_name(sleep_assist_state_t state)
{
    switch (state) {
    case SLEEP_ASSIST_OFF: return "off";
    case SLEEP_ASSIST_BASELINE: return "baseline";
    case SLEEP_ASSIST_SETTLE: return "settle";
    case SLEEP_ASSIST_ENTRAIN: return "entrain";
    case SLEEP_ASSIST_FADE: return "fade";
    case SLEEP_ASSIST_SLEEP_LOCK: return "sleep_lock";
    case SLEEP_ASSIST_ABORT: return "abort";
    default: return "unknown";
    }
}
