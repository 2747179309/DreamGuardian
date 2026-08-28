#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ONLINE_CHAT_TEXT_MAX 384
#define ONLINE_CHAT_RESULT_MAX 96
#define ONLINE_CHAT_MODEL_MAX 64
#define ONLINE_CHAT_BASE_URL_MAX 256
#define ONLINE_CHAT_AUDIO_PATH_MAX 96

typedef struct {
    esp_err_t error;
    int http_status;
    char result[ONLINE_CHAT_RESULT_MAX];
    char text[ONLINE_CHAT_TEXT_MAX];
    char audio_path[ONLINE_CHAT_AUDIO_PATH_MAX];
    uint32_t audio_bytes;
    uint32_t duration_ms;
} online_chat_result_t;

typedef struct {
    bool configured;
    bool worker_running;
    bool busy;
    uint32_t submitted_count;
    uint32_t ok_count;
    uint32_t failed_count;
    uint32_t dropped_count;
    esp_err_t last_error;
    int last_http_status;
    char model[ONLINE_CHAT_MODEL_MAX];
    char base_url[ONLINE_CHAT_BASE_URL_MAX];
    char last_result[ONLINE_CHAT_RESULT_MAX];
    char last_text[ONLINE_CHAT_TEXT_MAX];
} online_chat_status_t;

esp_err_t online_chat_init(void);
bool online_chat_is_ready(void);
bool online_chat_is_busy(void);
esp_err_t online_chat_submit_text(const char *text);
bool online_chat_take_result(online_chat_result_t *out);
void online_chat_get_status(online_chat_status_t *status);

#ifdef __cplusplus
}
#endif
