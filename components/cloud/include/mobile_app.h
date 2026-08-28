#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bio_filter.h"
#include "csi_monitor.h"
#include "device_commands.h"
#include "esp_err.h"
#include "intervention_policy.h"
#include "ld6002_types.h"
#include "sleep_assist.h"
#include "sleep_score.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mobile_app_config_saved_cb_t)(void *ctx);

typedef struct {
    const char *ssid;
    const char *password;
    uint8_t channel;
    const char *hostname;
    mobile_app_command_cb_t command_cb;
    void *command_ctx;
    mobile_app_config_saved_cb_t config_saved_cb;
    void *config_saved_ctx;
    bool http_enabled;
} mobile_app_config_t;

typedef struct {
    ld6002_snapshot_t radar;
    sleep_features_t features;
    sleep_assessment_t assessment;
    SleepBioState bio;
    sleep_assist_status_t assist;
    csi_monitor_status_t csi;
    intervention_decision_t decision;
    uint32_t uptime_sec;
    bool wifi_sta_connected;
    char wifi_ip[16];
    char wifi_ssid[33];
} mobile_app_telemetry_t;

typedef struct {
    esp_err_t http_result;
    esp_err_t wifi_result;
    uint16_t http_port;
    int self_tcp_result;
    int self_tcp_errno;
    bool http_task_found;
    uint32_t http_task_stack_free;
    bool server_running;
    bool wifi_started;
    bool sta_connected;
    uint32_t sta_config_version;
    char sta_ip[16];
    char sta_ssid[33];
} mobile_app_diag_t;

void mobile_app_get_default_config(mobile_app_config_t *config);
esp_err_t mobile_app_start(const mobile_app_config_t *config);
esp_err_t mobile_app_configure_wifi(const char *ssid, const char *password);
void mobile_app_update(const mobile_app_telemetry_t *telemetry);
bool mobile_app_is_sta_connected(void);
void mobile_app_get_ip(char *buffer, size_t buffer_size);
void mobile_app_get_sta_ssid(char *buffer, size_t buffer_size);
void mobile_app_get_diag(mobile_app_diag_t *diag);

#ifdef __cplusplus
}
#endif
