#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_commands.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LLM_INTENT_REPLY_MAX 192
#define LLM_INTENT_RESULT_MAX 96

typedef enum {
    LLM_INTENT_SOURCE_NONE = 0,
    LLM_INTENT_SOURCE_LOCAL,
    LLM_INTENT_SOURCE_LLM,
} llm_intent_source_t;

typedef struct {
    mobile_app_command_t command;
    llm_intent_source_t source;
    char command_name[48];
    char reply[LLM_INTENT_REPLY_MAX];
    esp_err_t error;
    int http_status;
} llm_intent_result_t;

typedef struct {
    bool configured;
    uint32_t local_count;
    uint32_t llm_count;
    uint32_t fallback_count;
    esp_err_t last_error;
    int last_http_status;
    char last_result[LLM_INTENT_RESULT_MAX];
} llm_intent_status_t;

mobile_app_command_t llm_intent_command_from_name(const char *name);
const char *llm_intent_command_to_name(mobile_app_command_t command);
const char *llm_intent_command_reply(mobile_app_command_t command, esp_err_t err);
mobile_app_command_t llm_intent_local_match(const char *text);
esp_err_t llm_intent_configure_openai_compatible(const char *api_key, const char *base_url, const char *model);
esp_err_t llm_intent_resolve_text(const char *text, bool allow_cloud, llm_intent_result_t *result);
void llm_intent_get_status(llm_intent_status_t *status);

#ifdef __cplusplus
}
#endif
