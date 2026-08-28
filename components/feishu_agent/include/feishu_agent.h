#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_commands.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mobile_app_command_cb_t command_cb;
    void *command_ctx;
} feishu_agent_config_t;

#define FEISHU_AGENT_LAST_RESULT_LEN 256

typedef struct {
    bool running;
    bool configured;
    bool ws_connected;
    bool ws_ever_connected;
    uint32_t ws_data_count;
    uint32_t ws_binary_count;
    uint32_t ws_text_count;
    uint32_t last_ws_opcode;
    uint32_t last_ws_data_len;
    uint32_t last_ws_payload_len;
    uint32_t frame_count;
    uint32_t event_count;
    uint32_t message_event_count;
    uint32_t parse_fail_count;
    uint32_t received_count;
    uint32_t command_count;
    uint32_t reply_count;
    int last_http_status;
    char last_event_type[64];
    char last_result[FEISHU_AGENT_LAST_RESULT_LEN];
} feishu_agent_status_t;

esp_err_t feishu_agent_start(const feishu_agent_config_t *config);
esp_err_t feishu_agent_restart(void);
void feishu_agent_get_status(feishu_agent_status_t *status);

#ifdef __cplusplus
}
#endif
