#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    bool running;
    bool initialized;
    bool models_loaded;
    bool wakenet_loaded;
    bool afe_created;
    bool feed_task_running;
    bool detect_task_running;
    bool template_experiment_enabled;
    bool assistant_suppressed;
    bool template_learning;
    esp_err_t last_error;
    uint32_t wake_events;
    uint32_t feed_frames;
    uint32_t fetch_frames;
    uint32_t read_errors;
    uint16_t mic_peak_ch0;
    uint16_t mic_peak_ch1;
    uint32_t i2s_overflow_count;
    uint32_t audio_drop_count;
    uint32_t feed_interval_max_us;
    uint32_t fetch_interval_max_us;
    uint32_t feed_process_max_us;
    uint32_t fetch_process_max_us;
    uint32_t inference_time_max_us;
    uint32_t template_wake_events;
    uint32_t command_events;
    uint32_t template_count;
    uint32_t template_checks;
    uint32_t template_learn_count;
    uint32_t template_learn_target;
    int last_command_id;
    uint32_t feed_chunk;
    uint32_t fetch_chunk;
    float template_best_score;
    float template_threshold;
    float template_confirm_threshold;
    uint32_t template_min_confirmations;
    bool debug_capture_enabled;
    bool debug_capture_active;
    bool debug_capture_write_pending;
    uint32_t debug_capture_seconds;
    uint32_t debug_capture_raw_samples;
    uint32_t debug_capture_afe_samples;
    uint32_t debug_capture_score_frames;
    uint32_t debug_capture_raw_drops;
    uint32_t debug_capture_afe_drops;
    uint32_t debug_capture_score_drops;
    bool command_audio_active;
    bool command_audio_ready;
    uint32_t command_audio_seconds;
    uint32_t command_audio_samples;
    uint32_t command_audio_drops;
    char wake_model[32];
    char command_model[32];
    char template_best_name[24];
    char template_source[24];
    char debug_capture_label[80];
    char debug_capture_raw_path[96];
    char debug_capture_afe_path[96];
    char debug_capture_score_path[96];
    char last_result[64];
} voice_sr_status_t;

typedef enum {
    VOICE_SR_COMMAND_NONE = 0,
    VOICE_SR_COMMAND_SLEEP,
    VOICE_SR_COMMAND_STOP,
    VOICE_SR_COMMAND_LIGHT_ON,
    VOICE_SR_COMMAND_LIGHT_OFF,
    VOICE_SR_COMMAND_LIGHT_BRIGHTER,
    VOICE_SR_COMMAND_LIGHT_DIMMER,
} voice_sr_command_t;

esp_err_t voice_sr_start(void);
void voice_sr_get_status(voice_sr_status_t *status);
bool voice_sr_take_wake_request(void);
bool voice_sr_take_command(voice_sr_command_t *command);
void voice_sr_pause(uint32_t duration_ms);
void voice_sr_set_assistant_suppressed(bool suppressed);
esp_err_t voice_sr_template_experiment_enable(bool enabled, uint32_t duration_sec);
esp_err_t voice_sr_template_match_configure(float threshold, float confirm_threshold,
                                            uint32_t min_confirmations);
esp_err_t voice_sr_template_learning_start(uint32_t target_count);
esp_err_t voice_sr_debug_capture_start(const char *label, uint32_t seconds);
esp_err_t voice_sr_command_audio_start(uint32_t seconds);
void voice_sr_command_audio_cancel(void);
bool voice_sr_take_command_audio(int16_t **samples, uint32_t *sample_count);

#ifdef __cplusplus
}
#endif
