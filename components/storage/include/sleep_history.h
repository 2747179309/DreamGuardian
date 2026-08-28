#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLEEP_LOG_HISTORY_MAX_DAYS 14

typedef struct {
    uint32_t date_key;
    uint16_t total_min;
    uint16_t sleep_min;
    uint16_t deep_min;
    uint16_t light_min;
    uint8_t quality;
    uint8_t max_wake_risk;
    float avg_heart_bpm;
    float avg_breath_bpm;
    float avg_motion;
    uint16_t out_of_bed_events;
    uint16_t interventions;
} sleep_daily_summary_t;

size_t sleep_log_get_recent_days(uint8_t requested_days,
                                 sleep_daily_summary_t *out,
                                 size_t out_capacity);
size_t sleep_log_format_recent_report(uint8_t requested_days,
                                      char *report,
                                      size_t report_size);

/* Competition/demo helper: replaces only the sleep-history NVS blob with a
 * realistic recent trend. It does not touch Wi-Fi, cloud, audio, or any other
 * persisted settings. */
bool sleep_log_seed_demo_history(uint8_t requested_days);

#ifdef __cplusplus
}
#endif
