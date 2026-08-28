#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "intervention_policy.h"
#include "ld6002_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_COMMAND_NONE = 0,
    VOICE_COMMAND_WAKE_WORD,
    VOICE_COMMAND_SLEEP,
    VOICE_COMMAND_WAKE_UP,
    VOICE_COMMAND_SET_ALARM,
    VOICE_COMMAND_STORY,
    VOICE_COMMAND_CHAT,
    VOICE_COMMAND_STOP,
    VOICE_COMMAND_DEVICE_ACTION,
} voice_command_t;

typedef enum {
    VOICE_MODE_IDLE = 0,
    VOICE_MODE_LISTENING,
    VOICE_MODE_SLEEP_RAMP,
    VOICE_MODE_SLEEP_PROMOTE,
    VOICE_MODE_WAKE_UP,
    VOICE_MODE_STORY,
    VOICE_MODE_CHAT,
} voice_mode_t;

typedef struct {
    voice_mode_t mode;
    voice_command_t last_command;
    bool alarm_enabled;
    bool clock_valid;
    uint8_t alarm_hour;
    uint8_t alarm_minute;
    uint8_t current_hour;
    uint8_t current_minute;
    uint32_t command_count;
    char last_reply[96];
} voice_control_status_t;

void voice_control_init(void);
bool voice_control_handle_phrase(const char *phrase);
bool voice_control_handle_touch(const char *button_name);
void voice_control_set_clock(uint8_t hour, uint8_t minute);
void voice_control_console_start(void);
bool voice_control_policy_suppressed(int64_t now_us);
bool voice_control_take_self_test_request(void);
bool voice_control_tick(const ld6002_snapshot_t *radar,
                        int64_t now_us,
                        intervention_decision_t *decision);
void voice_control_get_status(voice_control_status_t *status);

const char *voice_command_to_name(voice_command_t command);
const char *voice_mode_to_name(voice_mode_t mode);

#ifdef __cplusplus
}
#endif
