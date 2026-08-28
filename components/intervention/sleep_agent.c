#include "sleep_agent.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "sleep_agent";

static const sleep_agent_skill_t s_skills[] = {
    {"audio.play_pink_noise", 20, 0, 900},
    {"audio.stereo_breathing", 40, 65, 600},
    {"light.warm_breathing", 0, 18, 600},
    {"light.night_path", 0, 20, 300},
    {"policy.reduce_arousal", 15, 12, 600},
    {"voice.sleep_ramp", 40, 65, 30},
    {"voice.sleep_promote", 15, 8, 900},
    {"voice.wake_music", 45, 60, 300},
    {"voice.night_light", 0, 20, 300},
    {"voice.stop", 0, 0, 1},
    {"voice.wake_complete", 0, 0, 1},
    {"report.sleep_summary", 0, 0, 30},
};

void sleep_agent_init(void)
{
    ESP_LOGI(TAG, "Sleep Agent initialized with %u skills", (unsigned)(sizeof(s_skills) / sizeof(s_skills[0])));
}

static const sleep_agent_skill_t *find_skill(const char *name)
{
    for (size_t i = 0; i < sizeof(s_skills) / sizeof(s_skills[0]); ++i) {
        if (strcmp(name, s_skills[i].name) == 0) {
            return &s_skills[i];
        }
    }
    return NULL;
}

bool sleep_agent_handle_decision(const intervention_decision_t *decision)
{
    if (!decision || decision->action == INTERVENTION_ACTION_NONE) {
        return true;
    }

    const sleep_agent_skill_t *skill = find_skill(decision->skill_name);
    if (!skill) {
        ESP_LOGW(TAG, "reject unknown skill: %s", decision->skill_name);
        return false;
    }

    if (decision->volume_percent > skill->max_volume_percent) {
        ESP_LOGW(TAG, "reject %s: volume %u > %u", decision->skill_name,
                 decision->volume_percent, skill->max_volume_percent);
        return false;
    }
    if (decision->brightness_percent > skill->max_brightness_percent) {
        ESP_LOGW(TAG, "reject %s: brightness %u > %u", decision->skill_name,
                 decision->brightness_percent, skill->max_brightness_percent);
        return false;
    }
    if (decision->duration_sec > skill->max_duration_sec) {
        ESP_LOGW(TAG, "reject %s: duration %u > %u", decision->skill_name,
                 decision->duration_sec, skill->max_duration_sec);
        return false;
    }

    ESP_LOGI(TAG, "execute skill=%s action=%s volume=%u brightness=%u duration=%us reason=%s",
             decision->skill_name,
             intervention_action_to_name(decision->action),
             decision->volume_percent,
             decision->brightness_percent,
             decision->duration_sec,
             decision->reason);
    return true;
}
