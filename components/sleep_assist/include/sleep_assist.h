#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bio_filter.h"
#include "esp_err.h"
#include "voice_prompt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SLEEP_ASSIST_OFF = 0,
    SLEEP_ASSIST_BASELINE,
    SLEEP_ASSIST_SETTLE,
    SLEEP_ASSIST_ENTRAIN,
    SLEEP_ASSIST_FADE,
    SLEEP_ASSIST_SLEEP_LOCK,
    SLEEP_ASSIST_ABORT,
} sleep_assist_state_t;

typedef esp_err_t (*sleep_assist_led_fn_t)(uint8_t red, uint8_t green, uint8_t blue, float brightness);
typedef esp_err_t (*sleep_assist_screen_fn_t)(float phase, float brightness, bool text_visible);
typedef esp_err_t (*sleep_assist_audio_fn_t)(float volume, float target_breath_bpm, float pan_depth, bool rhythm_enabled);
typedef esp_err_t (*sleep_assist_voice_fn_t)(voice_prompt_id_t prompt, float volume, int64_t now_us);

typedef struct {
    uint16_t baseline_sec;
    uint16_t min_total_sec;
    uint16_t max_total_sec;
    uint16_t settle_sec;
    uint16_t entrain_sec;
    uint16_t fade_sec;
    uint16_t presence_lost_abort_sec;
    uint16_t sleep_candidate_required_sec;
    uint16_t no_breath_drop_rollback_sec;
    float final_target_breath_bpm;
    float min_target_breath_bpm;
    float max_target_breath_bpm;
    float max_breath_target_change_per_sec;
    float max_led_brightness_change_per_sec;
    float max_screen_brightness_change_per_sec;
    float max_audio_volume_change_per_sec;
    float max_audio_volume;
    float pan_depth;
    bool voice_prompt_enabled;
    sleep_assist_led_fn_t led_set;
    sleep_assist_screen_fn_t screen_set;
    sleep_assist_audio_fn_t audio_set;
    sleep_assist_voice_fn_t voice_play;
} sleep_assist_config_t;

typedef struct {
    sleep_assist_state_t state;
    float baseline_breath;
    float baseline_heart;
    float baseline_motion;
    float target_breath_bpm;
    float sleep_candidate_sec;
    float led_brightness;
    float screen_brightness;
    float audio_volume;
    bool active;
    bool sleep_locked;
} sleep_assist_status_t;

void sleep_assist_get_default_config(sleep_assist_config_t *config);
void sleep_assist_init(const sleep_assist_config_t *config);
esp_err_t sleep_assist_set_breath_mode(const char *mode);
esp_err_t sleep_assist_start(int64_t now_us);
esp_err_t sleep_assist_start_locked(int64_t now_us);
esp_err_t sleep_assist_stop(int64_t now_us);
bool sleep_assist_is_active(void);
void sleep_assist_tick(const SleepBioState *bio, bool user_interaction, int64_t now_us);
void sleep_assist_get_status(sleep_assist_status_t *status);
const char *sleep_assist_state_name(sleep_assist_state_t state);

#ifdef __cplusplus
}
#endif
