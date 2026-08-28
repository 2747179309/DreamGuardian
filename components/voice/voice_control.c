#include "voice_control.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "feishu_agent.h"
#include "llm_intent.h"
#include "mobile_app.h"
#include "online_asr.h"
#include "../storage/include/sleep_history.h"
#include "stereo_audio.h"
#include "voice_sr.h"
#include "sdkconfig.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "dg_debug_control.h"

#define SLEEP_RAMP_US (20LL * 1000000LL)
#define WAKE_STAGE_US (5LL * 60LL * 1000000LL)
#define MANUAL_STOP_SUPPRESS_US (10LL * 60LL * 1000000LL)
#define DG_CONSOLE_TASK_STACK 7168U

#define ZH_XIAOMENG "\xE5\xB0\x8F\xE6\xA2\xA6"
#define ZH_REST "\xE4\xBC\x91\xE6\x81\xAF"
#define ZH_SLEEP "\xE7\x9D\xA1\xE7\x9C\xA0"
#define ZH_SLEEP2 "\xE7\x9D\xA1\xE8\xA7\x89"
#define ZH_GO_TO_SLEEP "\xE7\x9D\xA1\xE8\xA7\x89\xE4\xBA\x86"
#define ZH_WAKE_UP "\xE8\xB5\xB7\xE5\xBA\x8A"
#define ZH_TIMER "\xE5\xAE\x9A\xE6\x97\xB6"
#define ZH_ALARM "\xE9\x97\xB9\xE9\x92\x9F"
#define ZH_STORY "\xE6\x95\x85\xE4\xBA\x8B"
#define ZH_TELL_STORY "\xE8\xAE\xB2\xE6\x95\x85\xE4\xBA\x8B"
#define ZH_CHAT "\xE8\x81\x8A\xE5\xA4\xA9"
#define ZH_EXITED "\xE9\x80\x80\xE5\x87\xBA\xE4\xBA\x86"
#define ZH_STOP "\xE5\x81\x9C\xE6\xAD\xA2"
#define ZH_STOP_SHORT "\xE5\x81\x9C"
#define ZH_HALF "\xE5\x8D\x8A"
#define ZH_ZERO "\xE9\x9B\xB6"
#define ZH_ONE "\xE4\xB8\x80"
#define ZH_TWO "\xE4\xBA\x8C"
#define ZH_TWO_ALT "\xE4\xB8\xA4"
#define ZH_THREE "\xE4\xB8\x89"
#define ZH_FOUR "\xE5\x9B\x9B"
#define ZH_FIVE "\xE4\xBA\x94"
#define ZH_SIX "\xE5\x85\xAD"
#define ZH_SEVEN "\xE4\xB8\x83"
#define ZH_EIGHT "\xE5\x85\xAB"
#define ZH_NINE "\xE4\xB9\x9D"
#define ZH_TEN "\xE5\x8D\x81"
#define ZH_TWENTY "\xE4\xBA\x8C\xE5\x8D\x81"

static const char *TAG = "voice_control";

static voice_control_status_t s_status;
static int64_t s_mode_started_us;
static int64_t s_policy_suppressed_until_us;
static bool s_stop_pending;
static bool s_self_test_pending;
static bool s_alarm_fired_for_minute;

esp_err_t dg_console_wifi_configure(const char *ssid, const char *password) __attribute__((weak));
esp_err_t dg_console_submit_command(mobile_app_command_t command) __attribute__((weak));

static bool contains(const char *text, const char *needle)
{
    return text && needle && strstr(text, needle) != NULL;
}

static void set_decision(intervention_decision_t *decision,
                         intervention_action_t action,
                         uint8_t volume,
                         uint8_t brightness,
                         uint16_t duration,
                         const char *skill,
                         const char *reason)
{
    memset(decision, 0, sizeof(*decision));
    decision->action = action;
    decision->volume_percent = volume;
    decision->brightness_percent = brightness;
    decision->duration_sec = duration;
    snprintf(decision->skill_name, sizeof(decision->skill_name), "%s", skill);
    snprintf(decision->reason, sizeof(decision->reason), "%s", reason);
}

static int chinese_digit_to_int(const char *text)
{
    if (contains(text, ZH_TWENTY)) return 20;
    if (contains(text, ZH_TEN)) return 10;
    if (contains(text, ZH_ZERO) || contains(text, "0")) return 0;
    if (contains(text, ZH_ONE) || contains(text, "1")) return 1;
    if (contains(text, ZH_TWO) || contains(text, ZH_TWO_ALT) || contains(text, "2")) return 2;
    if (contains(text, ZH_THREE) || contains(text, "3")) return 3;
    if (contains(text, ZH_FOUR) || contains(text, "4")) return 4;
    if (contains(text, ZH_FIVE) || contains(text, "5")) return 5;
    if (contains(text, ZH_SIX) || contains(text, "6")) return 6;
    if (contains(text, ZH_SEVEN) || contains(text, "7")) return 7;
    if (contains(text, ZH_EIGHT) || contains(text, "8")) return 8;
    if (contains(text, ZH_NINE) || contains(text, "9")) return 9;
    return -1;
}

static bool parse_alarm_time(const char *phrase, uint8_t *hour, uint8_t *minute)
{
    int parsed_hour = -1;
    int parsed_minute = 0;

    for (const char *p = phrase; p && *p; ++p) {
        if (isdigit((unsigned char)*p)) {
            parsed_hour = 0;
            while (isdigit((unsigned char)*p)) {
                parsed_hour = parsed_hour * 10 + (*p - '0');
                ++p;
            }
            break;
        }
    }

    if (parsed_hour < 0) {
        parsed_hour = chinese_digit_to_int(phrase);
    }

    if (contains(phrase, ZH_HALF)) {
        parsed_minute = 30;
    }

    if (parsed_hour < 0 || parsed_hour > 23 || parsed_minute > 59) {
        return false;
    }

    *hour = (uint8_t)parsed_hour;
    *minute = (uint8_t)parsed_minute;
    return true;
}

static void set_mode(voice_mode_t mode, voice_command_t command, const char *reply)
{
    s_status.mode = mode;
    s_status.last_command = command;
    s_status.command_count++;
    snprintf(s_status.last_reply, sizeof(s_status.last_reply), "%s", reply);
    s_mode_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "command=%s mode=%s reply=%s",
             voice_command_to_name(command), voice_mode_to_name(mode), s_status.last_reply);
}

static bool handle_intent_command(mobile_app_command_t command, const char *reply)
{
    if (command == MOBILE_APP_COMMAND_NONE) {
        return false;
    }

    switch (command) {
    case MOBILE_APP_COMMAND_SLEEP:
        s_policy_suppressed_until_us = 0;
        set_mode(VOICE_MODE_SLEEP_RAMP, VOICE_COMMAND_SLEEP,
                 reply && reply[0] ? reply : "reply.ok_sleep");
        return true;
    case MOBILE_APP_COMMAND_STOP:
        s_stop_pending = true;
        set_mode(VOICE_MODE_IDLE, VOICE_COMMAND_STOP,
                 reply && reply[0] ? reply : "reply.ok_stop");
        return true;
    case MOBILE_APP_COMMAND_SELF_TEST:
        s_self_test_pending = true;
        set_mode(VOICE_MODE_IDLE, VOICE_COMMAND_NONE,
                 reply && reply[0] ? reply : "reply.self_test");
        return true;
    case MOBILE_APP_COMMAND_STORY:
        s_policy_suppressed_until_us = 0;
        set_mode(VOICE_MODE_STORY, VOICE_COMMAND_STORY,
                 reply && reply[0] ? reply : "reply.ok_story");
        return true;
    default:
        break;
    }

    if (dg_console_submit_command) {
        esp_err_t err = dg_console_submit_command(command);
        char status[96];
        snprintf(status, sizeof(status), "reply.intent.%s.%s",
                 llm_intent_command_to_name(command), esp_err_to_name(err));
        set_mode(s_status.mode, VOICE_COMMAND_DEVICE_ACTION,
                 reply && reply[0] ? reply : status);
        return err == ESP_OK;
    }

    return false;
}

void voice_control_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.mode = VOICE_MODE_IDLE;
    snprintf(s_status.last_reply, sizeof(s_status.last_reply), "%s", "reply.ready");
    s_mode_started_us = esp_timer_get_time();
    s_policy_suppressed_until_us = 0;
    s_stop_pending = false;
    s_self_test_pending = false;
    s_alarm_fired_for_minute = false;
    ESP_LOGI(TAG, "voice command controller ready");
}

bool voice_control_handle_phrase(const char *phrase)
{
    if (!phrase || phrase[0] == '\0') {
        return false;
    }

    printf("DG Voice: phrase input=\"%s\"\r\n", phrase);

    /* Deep-sleep lock is intentionally touch-only for demonstrations. Keep
     * voice recognition from accidentally entering the locked test state. */
    if (contains(phrase, "深睡") || contains(phrase, "深度睡眠") ||
        contains(phrase, "deep sleep")) {
        snprintf(s_status.last_reply, sizeof(s_status.last_reply), "%s",
                 "reply.use_touch_deep_sleep");
        printf("DG Voice: deep sleep request ignored; use on-screen button\r\n");
        return false;
    }

    mobile_app_command_t local_intent = llm_intent_local_match(phrase);
    if ((contains(phrase, "hi") || contains(phrase, "Hi") || contains(phrase, ZH_XIAOMENG)) &&
        local_intent == MOBILE_APP_COMMAND_NONE) {
        set_mode(VOICE_MODE_LISTENING, VOICE_COMMAND_WAKE_WORD, "reply.zai_ne");
        return true;
    }

    if (contains(phrase, ZH_REST) || contains(phrase, ZH_SLEEP) || contains(phrase, ZH_SLEEP2) ||
        contains(phrase, ZH_GO_TO_SLEEP) ||
        contains(phrase, "sleep")) {
        s_policy_suppressed_until_us = 0;
        set_mode(VOICE_MODE_SLEEP_RAMP, VOICE_COMMAND_SLEEP, "reply.ok_sleep");
        return true;
    }

    if (contains(phrase, ZH_WAKE_UP) || contains(phrase, "wake")) {
        s_policy_suppressed_until_us = 0;
        set_mode(VOICE_MODE_WAKE_UP, VOICE_COMMAND_WAKE_UP, "reply.ok_wake_up");
        return true;
    }

    if (contains(phrase, ZH_TIMER) || contains(phrase, ZH_ALARM) || contains(phrase, "alarm")) {
        uint8_t hour = 0;
        uint8_t minute = 0;
        if (parse_alarm_time(phrase, &hour, &minute)) {
            s_status.alarm_enabled = true;
            s_status.alarm_hour = hour;
            s_status.alarm_minute = minute;
            char reply[96];
            snprintf(reply, sizeof(reply), "reply.ok_alarm_%02u_%02u", hour, minute);
            set_mode(s_status.mode, VOICE_COMMAND_SET_ALARM, reply);
            return true;
        }
        set_mode(VOICE_MODE_LISTENING, VOICE_COMMAND_SET_ALARM, "reply.repeat_alarm_time");
        return false;
    }

    if (contains(phrase, ZH_STORY) || contains(phrase, ZH_TELL_STORY) || contains(phrase, "story")) {
        s_policy_suppressed_until_us = 0;
        set_mode(VOICE_MODE_STORY, VOICE_COMMAND_STORY, "reply.ok_story");
        return true;
    }

    if (contains(phrase, ZH_CHAT) || contains(phrase, "chat")) {
        s_policy_suppressed_until_us = 0;
        set_mode(VOICE_MODE_CHAT, VOICE_COMMAND_CHAT, "reply.ok_chat");
        return true;
    }

    if (contains(phrase, "selftest") || contains(phrase, "test")) {
        s_self_test_pending = true;
        set_mode(VOICE_MODE_IDLE, VOICE_COMMAND_NONE, "reply.self_test");
        ESP_LOGI(TAG, "command=selftest");
        return true;
    }

    if (contains(phrase, ZH_EXITED) ||
        contains(phrase, ZH_STOP) || contains(phrase, ZH_STOP_SHORT) || contains(phrase, "stop")) {
        s_stop_pending = true;
        set_mode(VOICE_MODE_IDLE, VOICE_COMMAND_STOP, "reply.ok_stop");
        return true;
    }

    if (local_intent != MOBILE_APP_COMMAND_NONE) {
        const char *reply = llm_intent_command_reply(local_intent, ESP_OK);
        if (handle_intent_command(local_intent, reply)) {
            printf("DG Voice: phrase handled by intent command=%s source=local_prefilter\r\n",
                   llm_intent_command_to_name(local_intent));
            return true;
        }
    } else {
        llm_intent_result_t intent = {0};
        esp_err_t intent_err = llm_intent_resolve_text(phrase, true, &intent);
        if ((intent_err == ESP_OK || intent.command != MOBILE_APP_COMMAND_NONE) &&
            handle_intent_command(intent.command, intent.reply)) {
            ESP_LOGI(TAG, "llm intent phrase command=%s source=%d",
                     intent.command_name, (int)intent.source);
            printf("DG Voice: phrase handled by intent command=%s source=%d\r\n",
                   intent.command_name, (int)intent.source);
            return true;
        }

        printf("DG Voice: phrase unhandled err=%s intent=%s source=%d\r\n",
               esp_err_to_name(intent_err),
               intent.command_name[0] ? intent.command_name : "none",
               (int)intent.source);
        return false;
    }

    return false;
}


bool voice_control_policy_suppressed(int64_t now_us)
{
    return s_policy_suppressed_until_us > 0 && now_us < s_policy_suppressed_until_us;
}

bool voice_control_handle_touch(const char *button_name)
{
    if (!button_name) {
        return false;
    }
    return voice_control_handle_phrase(button_name);
}

void voice_control_set_clock(uint8_t hour, uint8_t minute)
{
    if (hour > 23 || minute > 59) {
        return;
    }

    if (s_status.current_hour != hour || s_status.current_minute != minute) {
        s_alarm_fired_for_minute = false;
    }

    s_status.clock_valid = true;
    s_status.current_hour = hour;
    s_status.current_minute = minute;
}

bool voice_control_take_self_test_request(void)
{
    if (!s_self_test_pending) {
        return false;
    }
    s_self_test_pending = false;
    return true;
}
bool voice_control_tick(const ld6002_snapshot_t *radar,
                        int64_t now_us,
                        intervention_decision_t *decision)
{
    if (!decision) {
        return false;
    }

    if (s_stop_pending) {
        s_stop_pending = false;
        s_policy_suppressed_until_us = now_us + MANUAL_STOP_SUPPRESS_US;
        ESP_LOGI(TAG, "manual stop hold active for %lld seconds",
                 (long long)(MANUAL_STOP_SUPPRESS_US / 1000000LL));
        set_decision(decision, INTERVENTION_ACTION_NONE, 0, 0, 0,
                     "voice.stop", "voice or touch stop command");
        return true;
    }

    if (s_status.alarm_enabled && s_status.clock_valid && !s_alarm_fired_for_minute &&
        s_status.current_hour == s_status.alarm_hour &&
        s_status.current_minute == s_status.alarm_minute) {
        s_alarm_fired_for_minute = true;
        set_mode(VOICE_MODE_WAKE_UP, VOICE_COMMAND_WAKE_UP, "reply.alarm_wake_up");
    }

    int64_t elapsed = now_us - s_mode_started_us;

    if (s_status.mode == VOICE_MODE_SLEEP_RAMP) {
        if (elapsed >= SLEEP_RAMP_US) {
            s_status.mode = VOICE_MODE_SLEEP_PROMOTE;
            s_mode_started_us = now_us;
            elapsed = 0;
            ESP_LOGI(TAG, "sleep ramp complete; promoting sleep");
        } else {
            uint8_t brightness = (uint8_t)(62 - ((elapsed * 28) / SLEEP_RAMP_US));
            set_decision(decision, INTERVENTION_ACTION_STEREO_BREATHING,
                         36, brightness, 5, "voice.sleep_ramp",
                         "sleep command ramping light and stereo breathing audio");
            return true;
        }
    }

    if (s_status.mode == VOICE_MODE_SLEEP_PROMOTE) {
        (void)radar;
        set_decision(decision, INTERVENTION_ACTION_NONE,
                     0, 0, 0, "voice.sleep_quiet",
                     "sleep command holding quiet mode and suppressing automatic arousal intervention");
        return true;
    }

    if (s_status.mode == VOICE_MODE_WAKE_UP) {
        if (elapsed >= WAKE_STAGE_US) {
            s_status.mode = VOICE_MODE_IDLE;
            set_decision(decision, INTERVENTION_ACTION_NONE, 0, 0, 0,
                         "voice.wake_complete", "wake stage completed");
            return true;
        }
        uint8_t volume = (uint8_t)(15 + ((elapsed * 25) / WAKE_STAGE_US));
        uint8_t brightness = (uint8_t)(10 + ((elapsed * 45) / WAKE_STAGE_US));
        set_decision(decision, INTERVENTION_ACTION_WAKE_MUSIC,
                     volume, brightness, 2, "voice.wake_music",
                     "wake command ramping light and gentle music");
        return true;
    }

    return false;
}

static bool parse_hhmm(const char *text, uint8_t *hour, uint8_t *minute)
{
    int h = -1;
    int m = -1;
    while (*text && !isdigit((unsigned char)*text)) {
        ++text;
    }
    if (sscanf(text, "%d:%d", &h, &m) != 2) {
        return false;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return false;
    }
    *hour = (uint8_t)h;
    *minute = (uint8_t)m;
    return true;
}

static void trim_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) {
        line[--len] = '\0';
    }
}

static void print_status(void)
{
    voice_control_status_t st;
    mobile_app_diag_t mobile;
    feishu_agent_status_t feishu;
    stereo_audio_status_t audio;
    voice_sr_status_t sr;
    voice_control_get_status(&st);
    mobile_app_get_diag(&mobile);
    feishu_agent_get_status(&feishu);
    stereo_audio_get_status(&audio);
    voice_sr_get_status(&sr);
    printf("\nDG status: mode=%s last=%s alarm=%s %02u:%02u clock=%s %02u:%02u reply=%s\n",
           voice_mode_to_name(st.mode),
           voice_command_to_name(st.last_command),
           st.alarm_enabled ? "on" : "off",
           st.alarm_hour,
           st.alarm_minute,
           st.clock_valid ? "ok" : "unset",
           st.current_hour,
           st.current_minute,
           st.last_reply);
    printf("mobile: http=%d wifi=%d port=%u server=%s http_task=%s stack=%u self_tcp=%d/%d wifi_started=%s sta=%s ssid=%s ip=%s\n",
           (int)mobile.http_result,
           (int)mobile.wifi_result,
           (unsigned)mobile.http_port,
           mobile.server_running ? "yes" : "no",
           mobile.http_task_found ? "yes" : "no",
           (unsigned)mobile.http_task_stack_free,
           mobile.self_tcp_result,
           mobile.self_tcp_errno,
           mobile.wifi_started ? "yes" : "no",
           mobile.sta_connected ? "yes" : "no",
           mobile.sta_ssid,
           mobile.sta_ip);
    printf("feishu: running=%s configured=%s ws=%s ever=%s frames=%u events=%u msg_events=%u parse_fail=%u rx=%u cmd=%u reply=%u http=%d event=%s last=%s\n",
           feishu.running ? "yes" : "no",
           feishu.configured ? "yes" : "no",
           feishu.ws_connected ? "yes" : "no",
           feishu.ws_ever_connected ? "yes" : "no",
           (unsigned)feishu.frame_count,
           (unsigned)feishu.event_count,
           (unsigned)feishu.message_event_count,
           (unsigned)feishu.parse_fail_count,
           (unsigned)feishu.received_count,
           (unsigned)feishu.command_count,
           (unsigned)feishu.reply_count,
           feishu.last_http_status,
           feishu.last_event_type,
           feishu.last_result);
    printf("audio: init=%s mode=%d vol=%u rate=%u wav=%u/%u remain=%u calls=%u writes=%u err=%d last=%s path=%s\n",
           audio.initialized ? "yes" : "no",
           audio.mode,
           audio.volume_percent,
           (unsigned)audio.sample_rate_hz,
           (unsigned)audio.wav_open_request_id,
           (unsigned)audio.wav_request_id,
           (unsigned)audio.wav_data_remaining,
           (unsigned)audio.write_calls,
           (unsigned)audio.write_errors,
           (int)audio.last_error,
           audio.last_result,
           audio.wav_path);
    printf("voice_sr: en=%s run=%s init=%s models=%s afe=%s wn=%s feed=%s detect=%s supp=%s learn=%s %u/%u frames=%u/%u read_err=%u overflow=%u drop=%u wake=%u cmd=%u chunks=%u/%u timing_us feed_i=%u fetch_i=%u feed_p=%u fetch_p=%u infer=%u err=%d last=%s model=%s mn=%s tpl=%u/%u src=%s best=%s %.2f thr=%.2f confirm=%.2f/%u\n",
           sr.enabled ? "yes" : "no",
           sr.running ? "yes" : "no",
           sr.initialized ? "yes" : "no",
           sr.models_loaded ? "yes" : "no",
           sr.afe_created ? "yes" : "no",
           sr.wakenet_loaded ? "yes" : "no",
           sr.feed_task_running ? "yes" : "no",
           sr.detect_task_running ? "yes" : "no",
           sr.assistant_suppressed ? "yes" : "no",
           sr.template_learning ? "yes" : "no",
           (unsigned)sr.template_learn_count,
           (unsigned)sr.template_learn_target,
           (unsigned)sr.feed_frames,
           (unsigned)sr.fetch_frames,
           (unsigned)sr.read_errors,
           (unsigned)sr.i2s_overflow_count,
           (unsigned)sr.audio_drop_count,
           (unsigned)sr.wake_events,
           (unsigned)sr.command_events,
           (unsigned)sr.feed_chunk,
           (unsigned)sr.fetch_chunk,
           (unsigned)sr.feed_interval_max_us,
           (unsigned)sr.fetch_interval_max_us,
           (unsigned)sr.feed_process_max_us,
           (unsigned)sr.fetch_process_max_us,
           (unsigned)sr.inference_time_max_us,
           (int)sr.last_error,
           sr.last_result,
           sr.wake_model,
           sr.command_model,
           (unsigned)sr.template_wake_events,
           (unsigned)sr.template_checks,
           sr.template_source,
           sr.template_best_name,
           (double)sr.template_best_score,
           (double)sr.template_threshold,
           (double)sr.template_confirm_threshold,
           (unsigned)sr.template_min_confirmations);
    printf("voice_sr_capture: compiled=%s active=%s pending=%s seconds=%u raw_samples=%u afe_samples=%u score_frames=%u drops=%u/%u/%u label=%s raw=%s afe=%s score=%s\n",
           sr.debug_capture_enabled ? "yes" : "no",
           sr.debug_capture_active ? "yes" : "no",
           sr.debug_capture_write_pending ? "yes" : "no",
           (unsigned)sr.debug_capture_seconds,
           (unsigned)sr.debug_capture_raw_samples,
           (unsigned)sr.debug_capture_afe_samples,
           (unsigned)sr.debug_capture_score_frames,
           (unsigned)sr.debug_capture_raw_drops,
           (unsigned)sr.debug_capture_afe_drops,
           (unsigned)sr.debug_capture_score_drops,
           sr.debug_capture_label,
           sr.debug_capture_raw_path,
           sr.debug_capture_afe_path,
           sr.debug_capture_score_path);
    online_asr_status_t asr;
    online_asr_get_status(&asr);
    printf("online_asr: enabled=%s configured=%s worker=%s busy=%s submitted=%u ok=%u fail=%u drop=%u err=%d http=%d model=%s base=%s result=%s text=%s\n",
           asr.enabled ? "yes" : "no",
           asr.configured ? "yes" : "no",
           asr.worker_running ? "yes" : "no",
           asr.busy ? "yes" : "no",
           (unsigned)asr.submitted_count,
           (unsigned)asr.ok_count,
           (unsigned)asr.failed_count,
           (unsigned)asr.dropped_count,
           (int)asr.last_error,
           asr.last_http_status,
           asr.model,
           asr.base_url,
           asr.last_result,
           asr.last_text);
}

static void console_task(void *arg)
{
    (void)arg;
    char line[256];
    printf("\nDreamGuardian test console ready. Commands: sleep, slept, wake, stop, selftest, story, chat, wifi SSID PASS, quiet on|off, llm qwen KEY, asr qwen KEY, asr apiyi KEY, asr key KEY, asr url URL, asr model MODEL, asr status, asr diag, rec LABEL [SEC], waketpl on|off [SEC], wakecfg THR CONF MIN, wakelearn N, capture LABEL [SEC], alarm HH:MM, time HH:MM, status, help\n");

    while (true) {
        printf("dg> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        trim_line(line);
        if (line[0] == '\0') {
            continue;
        }

        if (strcmp(line, "help") == 0) {
            printf("Commands: sleep | slept | wake | stop | selftest | story | chat | wifi SSID PASS | quiet on|off | llm qwen KEY | asr qwen KEY | asr apiyi KEY | asr key KEY | asr url URL | asr model MODEL | asr status | asr diag | rec LABEL [SEC] | waketpl on|off [SEC] | wakecfg THR CONF MIN | wakelearn N | capture LABEL [SEC] | alarm HH:MM | time HH:MM | status\n");
        } else if (strncmp(line, "wifi ", 5) == 0) {
            char ssid[33] = {0};
            char pass[65] = {0};
            char *args = line + 5;
            while (*args == ' ') args++;
            char *password = strrchr(args, ' ');
            if (password) {
                *password++ = '\0';
                while (*password == ' ') password++;
            }
            size_t ssid_len = strlen(args);
            if (ssid_len >= 2 && args[0] == '"' && args[ssid_len - 1] == '"') {
                args[ssid_len - 1] = '\0';
                args++;
            }
            snprintf(ssid, sizeof(ssid), "%.*s", (int)sizeof(ssid) - 1, args);
            snprintf(pass, sizeof(pass), "%.*s", (int)sizeof(pass) - 1,
                     password ? password : "");
            if (ssid[0] && dg_console_wifi_configure) {
                esp_err_t err = dg_console_wifi_configure(ssid, pass);
                printf("Wi-Fi config %s for SSID %s\n", err == ESP_OK ? "accepted" : "failed", ssid);
            } else if (!dg_console_wifi_configure) {
                printf("Wi-Fi config handler is not available\n");
            } else {
                printf("Usage: wifi SSID PASSWORD\n");
            }
        } else if (strncmp(line, "wakecfg ", 8) == 0) {
            float threshold = 0.0f;
            float confirm = 0.0f;
            unsigned min_confirmations = 0;
            int parsed = sscanf(line + 8, "%f %f %u", &threshold, &confirm, &min_confirmations);
            if (parsed >= 1) {
                if (parsed < 2) {
                    confirm = threshold - 0.02f;
                }
                if (parsed < 3) {
                    min_confirmations = 2;
                }
                esp_err_t err = voice_sr_template_match_configure(threshold, confirm, min_confirmations);
                printf("Wake template config %s threshold=%.3f confirm=%.3f min=%u\n",
                       err == ESP_OK ? "saved" : "failed",
                       (double)threshold,
                       (double)confirm,
                       min_confirmations);
            } else {
                printf("Usage: wakecfg THRESHOLD CONFIRM MIN_CONFIRMATIONS\n");
            }
        } else if (strncmp(line, "quiet ", 6) == 0) {
            bool enable = strcmp(line + 6, "on") == 0 || strcmp(line + 6, "1") == 0 ||
                          strcasecmp(line + 6, "true") == 0;
            bool disable = strcmp(line + 6, "off") == 0 || strcmp(line + 6, "0") == 0 ||
                           strcasecmp(line + 6, "false") == 0;
            if (enable || disable) {
                dg_debug_quiet_set(enable);
                printf("Debug quiet %s\n", enable ? "on" : "off");
            } else {
                printf("Usage: quiet on|off\n");
            }
        } else if (strcmp(line, "asr apiyi") == 0) {
            printf("Usage: asr apiyi APIYI_KEY\r\n");
        } else if (strncmp(line, "asr apiyi ", 10) == 0) {
            char *key = line + 10;
            while (*key == ' ') key++;
            esp_err_t err = online_asr_configure_openai_compatible(key, "https://api.apiyi.com/v1", "whisper-1");
            printf("ASR CONFIG: provider=apiyi base_url=https://api.apiyi.com/v1 model=whisper-1 err=%s\r\n",
                   esp_err_to_name(err));
        } else if (strcmp(line, "asr qwen") == 0) {
            printf("Usage: asr qwen DASHSCOPE_API_KEY\r\n");
        } else if (strncmp(line, "asr qwen ", 9) == 0) {
            char *key = line + 9;
            while (*key == ' ') key++;
            esp_err_t err = online_asr_configure_openai_compatible(
                key, "https://dashscope.aliyuncs.com/compatible-mode/v1", "qwen3-asr-flash");
            printf("ASR CONFIG: provider=qwen base_url=https://dashscope.aliyuncs.com/compatible-mode/v1 model=qwen3-asr-flash err=%s\r\n",
                   esp_err_to_name(err));
        } else if (strcmp(line, "llm qwen") == 0) {
            printf("Usage: llm qwen DASHSCOPE_API_KEY\r\n");
        } else if (strncmp(line, "llm qwen ", 9) == 0) {
            char *key = line + 9;
            while (*key == ' ') key++;
            esp_err_t err = llm_intent_configure_openai_compatible(
                key, "https://dashscope.aliyuncs.com/compatible-mode/v1", "qwen3.5-omni-flash");
            printf("LLM CONFIG: provider=qwen base_url=https://dashscope.aliyuncs.com/compatible-mode/v1 model=qwen3.5-omni-flash err=%s\r\n",
                   esp_err_to_name(err));
        } else if (strcmp(line, "asr key") == 0) {
            printf("Usage: asr key API_KEY\r\n");
        } else if (strncmp(line, "asr key ", 8) == 0) {
            char *key = line + 8;
            while (*key == ' ') key++;
            esp_err_t err = online_asr_configure_openai(key);
            printf("ASR CONFIG: key saved err=%s\r\n", esp_err_to_name(err));
        } else if (strcmp(line, "asr url") == 0) {
            printf("Usage: asr url https://api.apiyi.com/v1\r\n");
        } else if (strncmp(line, "asr url ", 8) == 0) {
            char *url = line + 8;
            while (*url == ' ') url++;
            esp_err_t err = online_asr_configure_base_url(url);
            printf("ASR CONFIG: base_url=%s err=%s\r\n", url, esp_err_to_name(err));
        } else if (strcmp(line, "asr model") == 0) {
            printf("Usage: asr model whisper-1\r\n");
        } else if (strncmp(line, "asr model ", 10) == 0) {
            char *model = line + 10;
            while (*model == ' ') model++;
            esp_err_t err = online_asr_configure_model(model);
            printf("ASR CONFIG: model=%s err=%s\r\n", model, esp_err_to_name(err));
        } else if (strcmp(line, "asr status") == 0) {
            online_asr_status_t asr;
            online_asr_get_status(&asr);
            printf("ASR STATUS: enabled=%d configured=%d busy=%d submitted=%u ok=%u fail=%u http=%d model=%s base=%s result=%s text=%s\r\n",
                   asr.enabled ? 1 : 0,
                   asr.configured ? 1 : 0,
                   asr.busy ? 1 : 0,
                   (unsigned)asr.submitted_count,
                   (unsigned)asr.ok_count,
                   (unsigned)asr.failed_count,
                   asr.last_http_status,
                   asr.model,
                   asr.base_url,
                   asr.last_result,
                   asr.last_text);
        } else if (strcmp(line, "asr diag") == 0) {
            online_asr_print_diag();
        } else if (strncmp(line, "asr", 3) == 0) {
            printf("Usage: asr qwen DASHSCOPE_API_KEY | asr apiyi APIYI_KEY | asr key API_KEY | asr url URL | asr model MODEL | asr status | asr diag\r\n");
        } else if (strncmp(line, "llm", 3) == 0) {
            printf("Usage: llm qwen DASHSCOPE_API_KEY\r\n");
        } else if (strncmp(line, "rec ", 4) == 0) {
            char label[80] = {0};
            unsigned seconds = 8;
            int parsed = sscanf(line + 4, "%79s %u", label, &seconds);
            if (parsed >= 1) {
                dg_debug_quiet_set(true);
                (void)voice_sr_template_experiment_enable(false, 0);
                esp_err_t err = voice_sr_debug_capture_start(label, seconds);
                if (err != ESP_OK) {
                    printf("REC FAILED label=%s err=%s\r\n", label, esp_err_to_name(err));
                }
            } else {
                printf("Usage: rec LABEL [SECONDS]\n");
            }
        } else if (strncmp(line, "waketpl ", 8) == 0) {
            char mode[8] = {0};
            unsigned duration = 120;
            int parsed = sscanf(line + 8, "%7s %u", mode, &duration);
            if (parsed >= 1) {
                bool enable = strcmp(mode, "on") == 0 || strcmp(mode, "1") == 0 ||
                              strcasecmp(mode, "true") == 0;
                bool disable = strcmp(mode, "off") == 0 || strcmp(mode, "0") == 0 ||
                               strcasecmp(mode, "false") == 0;
                if (enable || disable) {
                    esp_err_t err = voice_sr_template_experiment_enable(enable, duration);
                    printf("Wake template %s result=%s duration=%u\n",
                           enable ? "enabled" : "disabled",
                           esp_err_to_name(err),
                           duration);
                } else {
                    printf("Usage: waketpl on|off [SECONDS]\n");
                }
            } else {
                printf("Usage: waketpl on|off [SECONDS]\n");
            }
        } else if (strncmp(line, "wakelearn", 9) == 0) {
            unsigned target = 8;
            (void)sscanf(line + 9, "%u", &target);
            esp_err_t err = voice_sr_template_learning_start(target);
            printf("Wake template learning %s target=%u\n",
                   err == ESP_OK ? "started" : "failed", target);
        } else if (strncmp(line, "capture ", 8) == 0) {
            char label[80] = {0};
            unsigned seconds = 5;
            int parsed = sscanf(line + 8, "%79s %u", label, &seconds);
            if (parsed >= 1) {
                esp_err_t err = voice_sr_debug_capture_start(label, seconds);
                printf("Audio capture %s label=%s seconds=%u\n",
                       esp_err_to_name(err), label, seconds);
            } else {
                printf("Usage: capture LABEL [SECONDS]\n");
            }
        } else if (strncmp(line, "alarm", 5) == 0) {
            uint8_t hour = 0;
            uint8_t minute = 0;
            if (parse_hhmm(line, &hour, &minute)) {
                char phrase[32];
                snprintf(phrase, sizeof(phrase), "alarm %02u:%02u", hour, minute);
                voice_control_handle_phrase(phrase);
                printf("Alarm set to %02u:%02u\n", hour, minute);
            } else {
                printf("Usage: alarm HH:MM\n");
            }
        } else if (strncmp(line, "time", 4) == 0) {
            uint8_t hour = 0;
            uint8_t minute = 0;
            if (parse_hhmm(line, &hour, &minute)) {
                voice_control_set_clock(hour, minute);
                printf("Clock set to %02u:%02u\n", hour, minute);
            } else {
                printf("Usage: time HH:MM\n");
            }
        } else if (strcmp(line, "demo history") == 0) {
            bool ok = sleep_log_seed_demo_history(7);
            printf("Demo sleep history %s (7 days)\n", ok ? "seeded" : "failed");
        } else if (strcmp(line, "status") == 0) {
            print_status();
        } else if (strcmp(line, "sleep") == 0) {
            if (dg_console_submit_command) {
                esp_err_t err = dg_console_submit_command(MOBILE_APP_COMMAND_SLEEP);
                printf("Sleep command %s\n", err == ESP_OK ? "submitted" : "failed");
            } else {
                voice_control_handle_phrase(line);
            }
            print_status();
        } else if (strcmp(line, "slept") == 0) {
            if (dg_console_submit_command) {
                esp_err_t err = dg_console_submit_command(MOBILE_APP_COMMAND_SLEPT);
                printf("Slept lock command %s\n", err == ESP_OK ? "submitted" : "failed");
            } else {
                printf("Slept lock handler is not available\n");
            }
            print_status();
        } else if (strcmp(line, "stop") == 0) {
            if (dg_console_submit_command) {
                esp_err_t err = dg_console_submit_command(MOBILE_APP_COMMAND_STOP);
                printf("Stop command %s\n", err == ESP_OK ? "submitted" : "failed");
            } else {
                voice_control_handle_phrase(line);
            }
            print_status();
        } else if (strcmp(line, "wake") == 0 ||
                   strcmp(line, "selftest") == 0 || strcmp(line, "story") == 0 ||
                   strcmp(line, "chat") == 0) {
            voice_control_handle_phrase(line);
            print_status();
        } else if (voice_control_handle_phrase(line)) {
            print_status();
        } else {
            printf("Unknown command: %s\n", line);
        }
    }
}

void voice_control_console_start(void)
{
    static bool started;
    if (started) {
        return;
    }

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&usb_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB console driver install failed: %s", esp_err_to_name(err));
    }
    usb_serial_jtag_vfs_use_driver();
    fcntl(fileno(stdin), F_SETFL, 0);
    fcntl(fileno(stdout), F_SETFL, 0);
#elif CONFIG_ESP_CONSOLE_UART
    const int uart_num = CONFIG_ESP_CONSOLE_UART_NUM;
    esp_err_t err = uart_driver_install(uart_num, 2048, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "console UART driver install failed: %s", esp_err_to_name(err));
    }
    uart_vfs_dev_port_set_rx_line_endings(uart_num, ESP_LINE_ENDINGS_CRLF);
    uart_vfs_dev_port_set_tx_line_endings(uart_num, ESP_LINE_ENDINGS_CRLF);
    uart_vfs_dev_use_driver(uart_num);
#else
    ESP_LOGW(TAG, "test console disabled by ESP console configuration");
    return;
#endif

    setvbuf(stdin, NULL, _IONBF, 0);

    /* Console commands read/write NVS. Keep this stack internal: flash cache
     * suspension cannot safely run from an external-PSRAM task stack. */
    BaseType_t ok = xTaskCreateWithCaps(console_task, "dg_console", DG_CONSOLE_TASK_STACK,
                                        NULL, 3, NULL,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ok == pdPASS) {
        started = true;
    } else {
        ESP_LOGE(TAG, "failed to create console task");
    }
}

void voice_control_get_status(voice_control_status_t *status)
{
    if (!status) {
        return;
    }
    memcpy(status, &s_status, sizeof(*status));
}

const char *voice_command_to_name(voice_command_t command)
{
    switch (command) {
    case VOICE_COMMAND_WAKE_WORD: return "wake_word";
    case VOICE_COMMAND_SLEEP: return "sleep";
    case VOICE_COMMAND_WAKE_UP: return "wake_up";
    case VOICE_COMMAND_SET_ALARM: return "set_alarm";
    case VOICE_COMMAND_STORY: return "story";
    case VOICE_COMMAND_CHAT: return "chat";
    case VOICE_COMMAND_DEVICE_ACTION: return "device_action";
    case VOICE_COMMAND_STOP: return "stop";
    default: return "none";
    }
}

const char *voice_mode_to_name(voice_mode_t mode)
{
    switch (mode) {
    case VOICE_MODE_LISTENING: return "listening";
    case VOICE_MODE_SLEEP_RAMP: return "sleep_ramp";
    case VOICE_MODE_SLEEP_PROMOTE: return "sleep_promote";
    case VOICE_MODE_WAKE_UP: return "wake_up";
    case VOICE_MODE_STORY: return "story";
    case VOICE_MODE_CHAT: return "chat";
    default: return "idle";
    }
}
