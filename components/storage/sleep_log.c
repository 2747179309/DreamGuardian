#include "sleep_log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "sleep_log";

#define SLEEP_HISTORY_NVS_NS "dg_sleep_hist"
#define SLEEP_HISTORY_NVS_KEY "days"
#define SLEEP_HISTORY_MAGIC 0x44475348U
#define SLEEP_HISTORY_VERSION 1U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    sleep_daily_summary_t days[SLEEP_LOG_HISTORY_MAX_DAYS];
} sleep_history_store_t;

typedef struct {
    bool active;
    int64_t started_us;
    int64_t last_update_us;
    int64_t sleep_onset_us;
    sleep_state_t last_state;
    intervention_action_t last_action;
    int64_t last_intervention_us;
    uint32_t samples;
    uint32_t valid_samples;
    uint32_t high_risk_samples;
    uint32_t out_of_bed_events;
    uint32_t interventions;
    uint32_t night_path_events;
    uint32_t return_to_bed_events;
    uint32_t assist_sec;
    uint32_t sleep_locked_sec;
    uint32_t state_sec[SLEEP_STATE_OUT_OF_BED + 1];
    double score_sum;
    double risk_sum;
    double breath_sum;
    double heart_sum;
    double motion_sum;
    double stability_sum;
    uint8_t max_wake_risk;
} sleep_session_t;

static uint32_t s_samples;
static sleep_session_t s_session;
static sleep_history_store_t s_history;

static void history_load(void)
{
    memset(&s_history, 0, sizeof(s_history));
    s_history.magic = SLEEP_HISTORY_MAGIC;
    s_history.version = SLEEP_HISTORY_VERSION;

    nvs_handle_t nvs;
    if (nvs_open(SLEEP_HISTORY_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    sleep_history_store_t saved = {0};
    size_t size = sizeof(saved);
    esp_err_t err = nvs_get_blob(nvs, SLEEP_HISTORY_NVS_KEY, &saved, &size);
    nvs_close(nvs);
    if (err == ESP_OK && size == sizeof(saved) &&
        saved.magic == SLEEP_HISTORY_MAGIC &&
        saved.version == SLEEP_HISTORY_VERSION &&
        saved.count <= SLEEP_LOG_HISTORY_MAX_DAYS) {
        s_history = saved;
    }
}

static esp_err_t history_save(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SLEEP_HISTORY_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open history NVS failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(nvs, SLEEP_HISTORY_NVS_KEY, &s_history, sizeof(s_history));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save history failed: %s", esp_err_to_name(err));
    }
    return err;
}

static uint32_t current_date_key(void)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        return 0;
    }
    struct tm local = {0};
    localtime_r(&now, &local);
    return (uint32_t)(local.tm_year + 1900) * 10000U +
           (uint32_t)(local.tm_mon + 1) * 100U + (uint32_t)local.tm_mday;
}

bool sleep_log_seed_demo_history(uint8_t requested_days)
{
    static const sleep_daily_summary_t templates[7] = {
        {.total_min = 465, .sleep_min = 420, .deep_min = 105, .light_min = 315,
         .quality = 74, .max_wake_risk = 62, .avg_heart_bpm = 69.4f,
         .avg_breath_bpm = 15.8f, .avg_motion = 0.18f, .out_of_bed_events = 2,
         .interventions = 3},
        {.total_min = 478, .sleep_min = 438, .deep_min = 126, .light_min = 312,
         .quality = 78, .max_wake_risk = 55, .avg_heart_bpm = 67.8f,
         .avg_breath_bpm = 15.2f, .avg_motion = 0.15f, .out_of_bed_events = 1,
         .interventions = 2},
        {.total_min = 442, .sleep_min = 390, .deep_min = 92, .light_min = 298,
         .quality = 71, .max_wake_risk = 68, .avg_heart_bpm = 71.2f,
         .avg_breath_bpm = 16.1f, .avg_motion = 0.22f, .out_of_bed_events = 2,
         .interventions = 4},
        {.total_min = 490, .sleep_min = 452, .deep_min = 138, .light_min = 314,
         .quality = 82, .max_wake_risk = 48, .avg_heart_bpm = 66.7f,
         .avg_breath_bpm = 14.9f, .avg_motion = 0.12f, .out_of_bed_events = 1,
         .interventions = 2},
        {.total_min = 471, .sleep_min = 432, .deep_min = 121, .light_min = 311,
         .quality = 79, .max_wake_risk = 53, .avg_heart_bpm = 68.1f,
         .avg_breath_bpm = 15.3f, .avg_motion = 0.14f, .out_of_bed_events = 1,
         .interventions = 2},
        {.total_min = 500, .sleep_min = 463, .deep_min = 149, .light_min = 314,
         .quality = 86, .max_wake_risk = 42, .avg_heart_bpm = 65.9f,
         .avg_breath_bpm = 14.6f, .avg_motion = 0.10f, .out_of_bed_events = 0,
         .interventions = 1},
        {.total_min = 486, .sleep_min = 448, .deep_min = 141, .light_min = 307,
         .quality = 84, .max_wake_risk = 45, .avg_heart_bpm = 66.4f,
         .avg_breath_bpm = 14.8f, .avg_motion = 0.11f, .out_of_bed_events = 1,
         .interventions = 1},
    };

    time_t now = time(NULL);
    if (now < 1700000000) {
        ESP_LOGW(TAG, "cannot seed demo history before clock sync");
        return false;
    }

    size_t wanted = requested_days ? requested_days : 7;
    if (wanted > SLEEP_LOG_HISTORY_MAX_DAYS) wanted = SLEEP_LOG_HISTORY_MAX_DAYS;
    sleep_history_store_t demo = {
        .magic = SLEEP_HISTORY_MAGIC,
        .version = SLEEP_HISTORY_VERSION,
    };
    for (size_t i = 0; i < wanted; ++i) {
        size_t days_ago = wanted - 1U - i;
        time_t sample_time = now - (time_t)(days_ago * 24U * 60U * 60U);
        struct tm local = {0};
        localtime_r(&sample_time, &local);
        sleep_daily_summary_t sample = templates[i % 7U];
        sample.date_key = (uint32_t)(local.tm_year + 1900) * 10000U +
                          (uint32_t)(local.tm_mon + 1) * 100U +
                          (uint32_t)local.tm_mday;
        demo.days[demo.count++] = sample;
    }

    esp_err_t err = ESP_FAIL;
    sleep_history_store_t previous = s_history;
    s_history = demo;
    err = history_save();
    if (err != ESP_OK) {
        s_history = previous;
        return false;
    }
    printf("DG SleepHistory: demo seeded days=%u first=%08lu last=%08lu\r\n",
           (unsigned)s_history.count,
           (unsigned long)s_history.days[0].date_key,
           (unsigned long)s_history.days[s_history.count - 1].date_key);
    return true;
}

static void history_put(const sleep_daily_summary_t *summary)
{
    if (!summary || summary->date_key == 0) return;
    for (size_t i = 0; i < s_history.count; ++i) {
        if (s_history.days[i].date_key == summary->date_key) {
            s_history.days[i] = *summary;
            history_save();
            return;
        }
    }
    if (s_history.count < SLEEP_LOG_HISTORY_MAX_DAYS) {
        s_history.days[s_history.count++] = *summary;
    } else {
        memmove(&s_history.days[0], &s_history.days[1],
                sizeof(s_history.days[0]) * (SLEEP_LOG_HISTORY_MAX_DAYS - 1));
        s_history.days[SLEEP_LOG_HISTORY_MAX_DAYS - 1] = *summary;
    }
    history_save();
}

static uint32_t seconds_between(int64_t from_us, int64_t to_us)
{
    if (to_us <= from_us) {
        return 0;
    }
    int64_t sec = (to_us - from_us) / 1000000LL;
    if (sec < 1) {
        sec = 1;
    }
    if (sec > 10) {
        sec = 10;
    }
    return (uint32_t)sec;
}

static double average_or_zero(double sum, uint32_t count)
{
    return count > 0 ? sum / (double)count : 0.0;
}

static uint32_t safe_state_sec(sleep_state_t state)
{
    if (state < 0 || state > SLEEP_STATE_OUT_OF_BED) {
        return 0;
    }
    return s_session.state_sec[state];
}

static uint8_t clamp_score_int(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return (uint8_t)value;
}

static const char *quality_text(uint8_t score)
{
    if (score >= 85) {
        return "优秀";
    }
    if (score >= 70) {
        return "良好";
    }
    if (score >= 55) {
        return "一般";
    }
    return "偏差";
}

static uint8_t session_quality_score(uint32_t total_sec)
{
    if (total_sec == 0 || s_session.samples == 0) {
        return 0;
    }
    double avg_score = average_or_zero(s_session.score_sum, s_session.samples);
    double deep_ratio = (double)safe_state_sec(SLEEP_STATE_DEEP_TREND) / (double)total_sec;
    double out_ratio = (double)safe_state_sec(SLEEP_STATE_OUT_OF_BED) / (double)total_sec;
    double high_risk_ratio = (double)s_session.high_risk_samples / (double)s_session.samples;
    int score = (int)(avg_score +
                      deep_ratio * 18.0 -
                      out_ratio * 20.0 -
                      high_risk_ratio * 18.0 -
                      (double)s_session.out_of_bed_events * 3.0);
    return clamp_score_int(score);
}

static uint32_t minutes_round(uint32_t sec)
{
    return (sec + 30U) / 60U;
}

void sleep_log_init(void)
{
    s_samples = 0;
    memset(&s_session, 0, sizeof(s_session));
    s_session.last_state = SLEEP_STATE_UNKNOWN;
    /* sleep_log_init() runs before the cloud modules that traditionally
     * initialise NVS.  Initialise it here as well so persisted history is
     * available immediately after every reboot.  Never erase NVS here: Wi-Fi
     * and Feishu credentials share the partition and must be preserved. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_OK) {
        history_load();
    } else {
        printf("DG SleepHistory: NVS init failed err=%s\r\n",
               esp_err_to_name(nvs_err));
    }
    printf("DG SleepHistory: loaded days=%u\r\n", (unsigned)s_history.count);
    ESP_LOGI(TAG, "sleep log initialized history_days=%u", (unsigned)s_history.count);
}

void sleep_log_session_start(int64_t now_us)
{
    memset(&s_session, 0, sizeof(s_session));
    s_session.active = true;
    s_session.started_us = now_us;
    s_session.last_update_us = now_us;
    s_session.last_state = SLEEP_STATE_UNKNOWN;
    ESP_LOGI(TAG, "night session started");
    printf("DG SleepSession: started\r\n");
}

bool sleep_log_session_is_active(void)
{
    return s_session.active;
}

void sleep_log_session_update(const SleepBioState *bio,
                              const sleep_assessment_t *assessment,
                              const sleep_assist_status_t *assist,
                              const intervention_decision_t *decision,
                              int64_t now_us)
{
    if (!s_session.active || !assessment) {
        return;
    }

    uint32_t dt_sec = seconds_between(s_session.last_update_us, now_us);
    s_session.last_update_us = now_us;
    sleep_state_t state = assessment->state;
    if (state < SLEEP_STATE_UNKNOWN || state > SLEEP_STATE_OUT_OF_BED) {
        state = SLEEP_STATE_UNKNOWN;
    }
    s_session.state_sec[state] += dt_sec;
    if (assist && assist->active) {
        s_session.assist_sec += dt_sec;
    }
    if (assist && assist->sleep_locked) {
        s_session.sleep_locked_sec += dt_sec;
        if (s_session.sleep_onset_us == 0) {
            s_session.sleep_onset_us = now_us;
        }
    }

    if (state == SLEEP_STATE_OUT_OF_BED && s_session.last_state != SLEEP_STATE_OUT_OF_BED) {
        s_session.out_of_bed_events++;
    }
    if (s_session.last_state == SLEEP_STATE_OUT_OF_BED && state != SLEEP_STATE_OUT_OF_BED) {
        s_session.return_to_bed_events++;
    }
    s_session.last_state = state;

    s_session.samples++;
    s_session.score_sum += assessment->sleep_score;
    s_session.risk_sum += assessment->wake_risk;
    if (assessment->wake_risk >= 70) {
        s_session.high_risk_samples++;
    }
    if (assessment->wake_risk > s_session.max_wake_risk) {
        s_session.max_wake_risk = assessment->wake_risk;
    }

    if (bio && bio->valid) {
        s_session.valid_samples++;
        if (isfinite(bio->breath_bpm_smooth)) {
            s_session.breath_sum += bio->breath_bpm_smooth;
        }
        if (isfinite(bio->heart_bpm_smooth)) {
            s_session.heart_sum += bio->heart_bpm_smooth;
        }
        if (isfinite(bio->motion_smooth)) {
            s_session.motion_sum += bio->motion_smooth;
        }
        if (isfinite(bio->stability_score)) {
            s_session.stability_sum += bio->stability_score;
        }
    }

    if (decision && decision->action != INTERVENTION_ACTION_NONE) {
        bool new_action = decision->action != s_session.last_action ||
                          now_us - s_session.last_intervention_us > 60000000LL;
        if (new_action) {
            s_session.interventions++;
            if (decision->action == INTERVENTION_ACTION_NIGHT_PATH) {
                s_session.night_path_events++;
            }
            s_session.last_action = decision->action;
            s_session.last_intervention_us = now_us;
        }
    }
}

bool sleep_log_session_finish(const char *reason,
                              int64_t now_us,
                              char *report,
                              size_t report_size)
{
    if (!s_session.active) {
        if (report && report_size > 0) {
            report[0] = '\0';
        }
        return false;
    }
    if (s_session.last_update_us < now_us) {
        uint32_t dt_sec = seconds_between(s_session.last_update_us, now_us);
        s_session.state_sec[s_session.last_state] += dt_sec;
        s_session.last_update_us = now_us;
    }

    uint32_t total_sec = (uint32_t)((now_us - s_session.started_us) / 1000000LL);
    uint32_t awake_sec = safe_state_sec(SLEEP_STATE_AWAKE) + safe_state_sec(SLEEP_STATE_TRANSITION);
    uint32_t light_sec = safe_state_sec(SLEEP_STATE_LIGHT_TREND);
    uint32_t deep_sec = safe_state_sec(SLEEP_STATE_DEEP_TREND);
    uint32_t out_sec = safe_state_sec(SLEEP_STATE_OUT_OF_BED);
    uint32_t sleep_sec = light_sec + deep_sec;
    uint32_t onset_min = 0;
    if (s_session.sleep_onset_us > s_session.started_us) {
        onset_min = minutes_round((uint32_t)((s_session.sleep_onset_us - s_session.started_us) / 1000000LL));
    }
    uint8_t quality = session_quality_score(total_sec);
    sleep_daily_summary_t daily = {
        .date_key = current_date_key(),
        .total_min = (uint16_t)minutes_round(total_sec),
        .sleep_min = (uint16_t)minutes_round(sleep_sec),
        .deep_min = (uint16_t)minutes_round(deep_sec),
        .light_min = (uint16_t)minutes_round(light_sec),
        .quality = quality,
        .max_wake_risk = s_session.max_wake_risk,
        .avg_heart_bpm = (float)average_or_zero(s_session.heart_sum, s_session.valid_samples),
        .avg_breath_bpm = (float)average_or_zero(s_session.breath_sum, s_session.valid_samples),
        .avg_motion = (float)average_or_zero(s_session.motion_sum, s_session.valid_samples),
        .out_of_bed_events = (uint16_t)s_session.out_of_bed_events,
        .interventions = (uint16_t)s_session.interventions,
    };

    if (report && report_size > 0) {
        snprintf(report, report_size,
                 "DreamGuardian 完整睡眠报告\n"
                 "状态：睡眠模式已退出\n"
                 "结束原因：%s\n"
                 "总时长：%u 分钟，睡眠估计：%u 分钟，入睡用时：%u 分钟\n"
                 "深睡趋势：%u 分钟，浅睡趋势：%u 分钟，清醒/过渡：%u 分钟，离床：%u 分钟\n"
                 "睡眠质量：%u/100（%s），平均分：%.1f，最高苏醒风险：%u\n"
                 "平均呼吸：%.1f bpm，平均心率：%.1f bpm，平均体动：%.2f\n"
                 "离床次数：%u，回床次数：%u，干预次数：%u，夜灯次数：%u\n"
                 "建议：%s",
                 reason && reason[0] ? reason : "manual",
                 (unsigned)minutes_round(total_sec),
                 (unsigned)minutes_round(sleep_sec),
                 (unsigned)onset_min,
                 (unsigned)minutes_round(deep_sec),
                 (unsigned)minutes_round(light_sec),
                 (unsigned)minutes_round(awake_sec),
                 (unsigned)minutes_round(out_sec),
                 (unsigned)quality,
                 quality_text(quality),
                 average_or_zero(s_session.score_sum, s_session.samples),
                 (unsigned)s_session.max_wake_risk,
                 average_or_zero(s_session.breath_sum, s_session.valid_samples),
                 average_or_zero(s_session.heart_sum, s_session.valid_samples),
                 average_or_zero(s_session.motion_sum, s_session.valid_samples),
                 (unsigned)s_session.out_of_bed_events,
                 (unsigned)s_session.return_to_bed_events,
                 (unsigned)s_session.interventions,
                 (unsigned)s_session.night_path_events,
                 quality >= 70 ? "保持当前助眠节奏；如半夜醒来少，继续降低夜间声光刺激。" :
                 "今晚建议降低声光强度，延长入睡缓冲阶段，并检查雷达角度/距离。");
    }

    ESP_LOGI(TAG,
             "night session finish reason=%s total=%us sleep=%us deep=%us light=%us awake=%us out=%us quality=%u interventions=%u",
             reason ? reason : "",
             (unsigned)total_sec,
             (unsigned)sleep_sec,
             (unsigned)deep_sec,
             (unsigned)light_sec,
             (unsigned)awake_sec,
             (unsigned)out_sec,
             (unsigned)quality,
             (unsigned)s_session.interventions);
    printf("DG SleepSession: finished quality=%u total_min=%u sleep_min=%u deep_min=%u light_min=%u out_min=%u interventions=%u\r\n",
           (unsigned)quality,
           (unsigned)minutes_round(total_sec),
           (unsigned)minutes_round(sleep_sec),
           (unsigned)minutes_round(deep_sec),
           (unsigned)minutes_round(light_sec),
           (unsigned)minutes_round(out_sec),
           (unsigned)s_session.interventions);
    history_put(&daily);
    s_session.active = false;
    return true;
}

size_t sleep_log_get_recent_days(uint8_t requested_days,
                                 sleep_daily_summary_t *out,
                                 size_t out_capacity)
{
    if (!out || out_capacity == 0 || s_history.count == 0) return 0;
    size_t wanted = requested_days ? requested_days : 7;
    if (wanted > SLEEP_LOG_HISTORY_MAX_DAYS) wanted = SLEEP_LOG_HISTORY_MAX_DAYS;
    if (wanted > s_history.count) wanted = s_history.count;
    if (wanted > out_capacity) wanted = out_capacity;
    size_t first = s_history.count - wanted;
    memcpy(out, &s_history.days[first], wanted * sizeof(*out));
    return wanted;
}

size_t sleep_log_format_recent_report(uint8_t requested_days,
                                      char *report,
                                      size_t report_size)
{
    if (!report || report_size == 0) return 0;
    if (requested_days == 0) requested_days = 7;
    if (requested_days > SLEEP_LOG_HISTORY_MAX_DAYS) requested_days = SLEEP_LOG_HISTORY_MAX_DAYS;
    sleep_daily_summary_t days[SLEEP_LOG_HISTORY_MAX_DAYS];
    size_t count = sleep_log_get_recent_days(requested_days, days, SLEEP_LOG_HISTORY_MAX_DAYS);
    if (count == 0) {
        return (size_t)snprintf(report, report_size,
                               "DreamGuardian 近%u日睡眠报告\n"
                               "暂无已完成的睡眠记录。请至少完整退出一次睡眠模式，设备会从本次升级后开始按日保存数据。",
                               (unsigned)requested_days);
    }

    double score_sum = 0.0, sleep_sum = 0.0, heart_sum = 0.0, breath_sum = 0.0;
    uint32_t out_events = 0, interventions = 0;
    for (size_t i = 0; i < count; ++i) {
        score_sum += days[i].quality;
        sleep_sum += days[i].sleep_min;
        heart_sum += days[i].avg_heart_bpm;
        breath_sum += days[i].avg_breath_bpm;
        out_events += days[i].out_of_bed_events;
        interventions += days[i].interventions;
    }
    int trend = (int)days[count - 1].quality - (int)days[0].quality;
    const char *trend_text = trend >= 3 ? "睡眠质量呈上升趋势" :
                             trend <= -3 ? "睡眠质量呈下降趋势，建议检查睡前环境和雷达位置" :
                                           "睡眠质量总体平稳";
    uint32_t first = days[0].date_key;
    uint32_t last = days[count - 1].date_key;
    return (size_t)snprintf(report, report_size,
                           "DreamGuardian 近%u日睡眠报告\n"
                           "数据范围：%02u-%02u 至 %02u-%02u（已有 %u/%u 天有效记录）\n"
                           "平均睡眠质量：%.1f/100；日均睡眠：%.0f 分钟\n"
                           "平均心率：%.1f bpm；平均呼吸：%.1f bpm\n"
                           "离床：%u 次；睡眠干预：%u 次\n"
                           "趋势说明：%s（首日至末日 %+d 分）\n"
                           "图例：橙色折线=睡眠质量；蓝色折线=睡眠时长。数据仅用于睡眠趋势观察，不作为医疗诊断。",
                           (unsigned)requested_days,
                           (unsigned)((first / 100U) % 100U), (unsigned)(first % 100U),
                           (unsigned)((last / 100U) % 100U), (unsigned)(last % 100U),
                           (unsigned)count, (unsigned)requested_days,
                           score_sum / count, sleep_sum / count,
                           heart_sum / count, breath_sum / count,
                           (unsigned)out_events, (unsigned)interventions,
                           trend_text, trend);
}

void sleep_log_append_summary(const ld6002_snapshot_t *radar,
                              const sleep_features_t *features,
                              const sleep_assessment_t *assessment,
                              const intervention_decision_t *decision)
{
    s_samples++;
    if ((s_samples % 60) != 0) {
        return;
    }

    ESP_LOGI(TAG,
             "summary samples=%u bytes=%u raw=%u valid=%u unknown=%u checksum_errors=%u parse_errors=%u out_of_bed=%u low_quality=%u score=%u risk=%u action=%s session=%d",
             s_samples,
             radar->uart_bytes,
             radar->raw_frames,
             radar->frames,
             radar->unknown_frames,
             radar->checksum_errors,
             radar->parse_errors,
             features->out_of_bed_events,
             features->low_quality_windows,
             assessment->sleep_score,
             assessment->wake_risk,
             decision->skill_name,
             s_session.active);
}
