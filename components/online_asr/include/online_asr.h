#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ONLINE_ASR_TEXT_MAX 256
#define ONLINE_ASR_RESULT_MAX 96
#define ONLINE_ASR_MODEL_MAX 64
#define ONLINE_ASR_BASE_URL_MAX 256

typedef struct {
    bool enabled;
    bool configured;
    bool worker_running;
    bool busy;
    uint32_t submitted_count;
    uint32_t ok_count;
    uint32_t failed_count;
    uint32_t dropped_count;
    esp_err_t last_error;
    int last_http_status;
    char model[ONLINE_ASR_MODEL_MAX];
    char base_url[ONLINE_ASR_BASE_URL_MAX];
    char last_result[ONLINE_ASR_RESULT_MAX];
    char last_text[ONLINE_ASR_TEXT_MAX];
} online_asr_status_t;

esp_err_t online_asr_init(void);
esp_err_t online_asr_configure_openai(const char *api_key);
esp_err_t online_asr_configure_openai_compatible(const char *api_key, const char *base_url, const char *model);
esp_err_t online_asr_configure_base_url(const char *base_url);
esp_err_t online_asr_configure_model(const char *model);
bool online_asr_is_ready(void);
bool online_asr_is_busy(void);
esp_err_t online_asr_submit_pcm16_take(int16_t *samples, uint32_t sample_count, const char *label);
bool online_asr_take_transcript(char *out, size_t out_size);
void online_asr_get_status(online_asr_status_t *status);
void online_asr_print_diag(void);

#ifdef __cplusplus
}
#endif
