#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_PROMPT_SLEEP_START = 0,
    VOICE_PROMPT_ENTRAIN_START,
    VOICE_PROMPT_FADE_START,
} voice_prompt_id_t;

typedef struct {
    bool enabled;
    float max_volume;
    uint16_t min_interval_sec;
    uint8_t max_prompts_per_session;
} voice_prompt_config_t;

void voice_prompt_get_default_config(voice_prompt_config_t *config);
void voice_prompt_init(const voice_prompt_config_t *config);
void voice_prompt_reset_session(void);
esp_err_t voice_prompt_play(voice_prompt_id_t prompt, float volume, int64_t now_us);
const char *voice_prompt_text(voice_prompt_id_t prompt);

#ifdef __cplusplus
}
#endif