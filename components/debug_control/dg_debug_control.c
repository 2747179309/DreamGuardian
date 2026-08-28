#include "dg_debug_control.h"

#include "esp_log.h"

static volatile bool s_quiet_enabled;

void dg_debug_quiet_set(bool enabled)
{
    s_quiet_enabled = enabled;
    esp_log_level_set("*", enabled ? ESP_LOG_ERROR : ESP_LOG_INFO);
}

bool dg_debug_quiet_enabled(void)
{
    return s_quiet_enabled;
}
