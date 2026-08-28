#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t time_sync_start(void);
bool time_sync_is_valid(void);
bool time_sync_get_local_time(struct tm *timeinfo, time_t *now_out);
const char *time_sync_weekday_name(int tm_wday);

#ifdef __cplusplus
}
#endif
