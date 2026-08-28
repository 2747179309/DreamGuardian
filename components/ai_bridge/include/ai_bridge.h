#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sleep_assist.h"
#include "sleep_score.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AI_BRIDGE_WEBHOOK_URL_MAX 256
#define AI_BRIDGE_RESULT_MAX 96

typedef struct {
    bool enabled;
    char feishu_webhook_url[AI_BRIDGE_WEBHOOK_URL_MAX];
    uint32_t min_interval_sec;
} ai_bridge_config_t;

typedef struct {
    bool enabled;
    bool configured;
    bool worker_running;
    uint32_t queued_count;
    uint32_t sent_count;
    uint32_t dropped_count;
    int last_http_status;
    char last_result[AI_BRIDGE_RESULT_MAX];
} ai_bridge_status_t;

typedef struct {
    bool presence;
    bool bio_valid;
    float breath_bpm;
    float heart_bpm;
    float motion;
    float stability;
    uint8_t sleep_score;
    uint8_t wake_risk;
    sleep_state_t sleep_state;
    sleep_assist_state_t assist_state;
    bool sleep_locked;
    const char *action;
    uint32_t uptime_sec;
} ai_bridge_sample_t;

void ai_bridge_get_default_config(ai_bridge_config_t *config);
esp_err_t ai_bridge_init(const ai_bridge_config_t *config);
esp_err_t ai_bridge_get_config(ai_bridge_config_t *config);
esp_err_t ai_bridge_set_config(const ai_bridge_config_t *config);
void ai_bridge_get_status(ai_bridge_status_t *status);
void ai_bridge_update(const ai_bridge_sample_t *sample, int64_t now_us);
esp_err_t ai_bridge_send_text(const char *text);
/* Queue a forced event without blocking the sleep/radar loop on HTTPS. */
esp_err_t ai_bridge_send_queued_text(const char *text);
/* Persist before queueing; failed sends retry and survive a device restart. */
esp_err_t ai_bridge_send_reliable_text(const char *text);
esp_err_t ai_bridge_send_test(const char *text);

#ifdef __cplusplus
}
#endif
