#include "time_sync.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"

static const char *TAG = "time_sync";
static bool s_started;
static bool s_synced;
static int64_t s_started_us;

static void sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    ESP_LOGI(TAG, "SNTP time synchronized");
}

esp_err_t time_sync_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_setservername(2, "time.google.com");
    esp_sntp_set_time_sync_notification_cb(sync_notification_cb);
    esp_sntp_init();

    s_started = true;
    s_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "SNTP started timezone=CST-8 servers=pool.ntp.org,cn.pool.ntp.org,time.google.com");
    return ESP_OK;
}

bool time_sync_is_valid(void)
{
    time_t now = 0;
    time(&now);
    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);
    bool valid = (timeinfo.tm_year + 1900) >= 2024;
    if (valid) {
        s_synced = true;
    }
    return valid;
}

bool time_sync_get_local_time(struct tm *timeinfo, time_t *now_out)
{
    time_t now = 0;
    time(&now);
    if (now_out) {
        *now_out = now;
    }
    if (!timeinfo) {
        return time_sync_is_valid();
    }
    memset(timeinfo, 0, sizeof(*timeinfo));
    localtime_r(&now, timeinfo);
    return (timeinfo->tm_year + 1900) >= 2024;
}

const char *time_sync_weekday_name(int tm_wday)
{
    static const char *names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    if (tm_wday < 0 || tm_wday > 6) {
        return "---";
    }
    return names[tm_wday];
}
