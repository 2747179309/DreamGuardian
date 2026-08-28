#pragma once

#include <stdbool.h>

#include "intervention_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    uint8_t max_volume_percent;
    uint8_t max_brightness_percent;
    uint16_t max_duration_sec;
} sleep_agent_skill_t;

void sleep_agent_init(void);
bool sleep_agent_handle_decision(const intervention_decision_t *decision);

#ifdef __cplusplus
}
#endif
