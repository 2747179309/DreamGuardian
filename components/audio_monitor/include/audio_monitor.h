#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    uint32_t sample_rate_hz;
    uint16_t frame_ms;
    float input_gain_db;
} audio_monitor_config_t;

typedef struct {
    bool enabled;
    bool running;
    bool initialized;
    esp_err_t last_error;
    uint32_t sample_rate_hz;
    uint16_t frame_ms;
    uint32_t frames;
    uint32_t read_errors;
    float rms;
    float peak;
    float level_dbfs;
    float noise_floor_dbfs;
    float instant_above_floor_db;
    float phrase_peak;
    float phrase_above_floor_db;
    uint32_t phrase_ms;
    float environment_score;
    float volume_gain;
    bool noise_high;
    bool voice_activity;
    bool wake_listening;
    bool wake_word_configured;
    bool snore_active;
    uint32_t wake_events;
    uint32_t snore_events;
    uint32_t snore_rejected_events;
    uint16_t snore_last_duration_ms;
    float snore_last_peak_dbfs;
    float snore_last_peak_above_floor_db;
    float snore_last_zcr;
    uint32_t apnea_candidates;
    uint32_t quiet_seconds;
} audio_monitor_status_t;

void audio_monitor_get_default_config(audio_monitor_config_t *config);
esp_err_t audio_monitor_start(const audio_monitor_config_t *config);
esp_err_t audio_monitor_shared_stream_init(uint32_t sample_rate_hz, uint16_t frame_ms);
void audio_monitor_shared_stream_set_state(bool sleep_analysis_active, bool playback_active);
void audio_monitor_shared_stream_set_sleep_context(bool person_present,
                                                   bool breath_valid,
                                                   float breath_bpm);
void audio_monitor_process_shared_samples(const int16_t *samples, size_t sample_count,
                                          bool afe_speech);
void audio_monitor_get_status(audio_monitor_status_t *status);
float audio_monitor_volume_gain(void);
bool audio_monitor_voice_activity(void);
bool audio_monitor_take_wake_request(void);

#ifdef __cplusplus
}
#endif
