#include "voice_prompt.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "voice_prompt";

typedef struct {
    voice_prompt_config_t cfg;
    int64_t last_prompt_us;
    uint8_t played_count;
} voice_prompt_context_t;

static voice_prompt_context_t s_prompt;

void voice_prompt_get_default_config(voice_prompt_config_t *config)
{
    if (!config) {
        return;
    }
    config->enabled = true;
    config->max_volume = 0.18f;
    config->min_interval_sec = 180;
    config->max_prompts_per_session = 3;
}

void voice_prompt_init(const voice_prompt_config_t *config)
{
    memset(&s_prompt, 0, sizeof(s_prompt));
    if (config) {
        s_prompt.cfg = *config;
    } else {
        voice_prompt_get_default_config(&s_prompt.cfg);
    }
    if (s_prompt.cfg.max_volume > 0.18f) {
        s_prompt.cfg.max_volume = 0.18f;
    }
    s_prompt.last_prompt_us = -((int64_t)s_prompt.cfg.min_interval_sec * 1000000LL);
    ESP_LOGI(TAG, "ready enabled=%d max_volume=%.2f interval=%us max_count=%u",
             s_prompt.cfg.enabled,
             (double)s_prompt.cfg.max_volume,
             s_prompt.cfg.min_interval_sec,
             s_prompt.cfg.max_prompts_per_session);
}

void voice_prompt_reset_session(void)
{
    s_prompt.played_count = 0;
    s_prompt.last_prompt_us = -((int64_t)s_prompt.cfg.min_interval_sec * 1000000LL);
}

const char *voice_prompt_text(voice_prompt_id_t prompt)
{
    switch (prompt) {
    case VOICE_PROMPT_SLEEP_START:
        return "sleep assist enabled, adjusting the sleep environment";
    case VOICE_PROMPT_ENTRAIN_START:
        return "follow the gentle light and relax your breathing";
    case VOICE_PROMPT_FADE_START:
        return "you are relaxing; sound and light will fade out";
    default:
        return "unknown prompt";
    }
}

esp_err_t voice_prompt_play(voice_prompt_id_t prompt, float volume, int64_t now_us)
{
    if (!s_prompt.cfg.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_prompt.played_count >= s_prompt.cfg.max_prompts_per_session) {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t min_gap_us = (int64_t)s_prompt.cfg.min_interval_sec * 1000000LL;
    if (now_us - s_prompt.last_prompt_us < min_gap_us) {
        return ESP_ERR_TIMEOUT;
    }
    if (volume > s_prompt.cfg.max_volume) {
        volume = s_prompt.cfg.max_volume;
    }

    s_prompt.last_prompt_us = now_us;
    s_prompt.played_count++;

    /* Local prerecorded PCM/WAV playback should be connected here.  We intentionally
       do not use cloud TTS in the sleep path to avoid latency and sudden speech. */
    ESP_LOGI(TAG, "local_prompt id=%d volume=%.2f text=%s",
             prompt, (double)volume, voice_prompt_text(prompt));
    return ESP_OK;
}