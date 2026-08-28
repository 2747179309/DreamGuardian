#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"

#include "app_config.h"
#include "intervention_policy.h"
#include "ld6002_task.h"
#include "sleep_agent.h"
#include "sleep_score.h"
#include "stereo_audio.h"
#include "audio_monitor.h"
#include "voice_sr.h"
#include "sleep_ui.h"
#include "sleep_log.h"
#include "voice_control.h"
#include "status_light.h"
#include "bio_filter.h"
#include "sleep_assist.h"
#include "voice_prompt.h"
#include "mobile_app.h"
#include "csi_monitor.h"
#include "time_sync.h"
#include "ai_bridge.h"
#include "feishu_agent.h"
#include "dg_debug_control.h"
#include "online_asr.h"
#include "online_chat.h"

static const char *TAG = "dreamguardian";
#define DG_AUDIO_SAFE_ISOLATION 0
#define DG_SPEAKER_SAFE_DISABLED 0
#define DG_PREF_NVS_NAMESPACE "dg_prefs"
#define DG_PREF_SCENE_KEY "scene"
#define DG_PREF_BREATH_KEY "breath"
#define DG_PREF_COUNT_PREFIX "cnt_"
#define MOBILE_COMMAND_QUEUE_LEN 8
#define DG_VOICE_COMMAND_WAIT_US 10000000LL
#define DG_VOICE_SESSION_IDLE_US 10000000LL
#define DG_SLEEP_STOP_BLANK_SEC 1
#define DG_AUTO_WAKE_START_HOUR 5
#define DG_AUTO_WAKE_END_HOUR 12
#define DG_AUTO_WAKE_OUT_OF_BED_SEC 600
#define DG_AUTO_WAKE_ACTIVE_SEC 480
#define DG_RETURN_SLEEP_STABLE_SEC 45
#define DG_RETURN_SLEEP_WEAK_SEC 420
#define DG_OUT_OF_BED_CONFIRM_SEC 3
#define DG_RETURN_TO_BED_CONFIRM_SEC 3
#define DG_SLEPT_TEST_RETURN_STABLE_SEC 10
#define DG_SLEPT_TEST_ENTRY_GRACE_SEC 8
#define DG_RADAR_PRESENCE_MAX_AGE_US 3000000LL
#define DG_RADAR_VITAL_PRESENCE_GUARD_US 2000000LL
#define DG_TURN_WINDOW_US 6000000LL
#define DG_TURN_RISK_HOLD_US 12000000LL
#define DG_TURN_MIN_SPAN_US 2000000LL
#define DG_TURN_MOTION_THRESHOLD 0.10f
#define DG_DEMO_TURN_MOTION_THRESHOLD 0.55f
#define DG_DEMO_TURN_CONTINUITY_US 2000000LL
#define DG_DEMO_TURN_MINOR_MIN_SPAN_US 1000000LL
#define DG_DEMO_TURN_STRONG_MIN_SPAN_US 2500000LL
#define DG_DEMO_TURN_STRONG_PEAK_THRESHOLD 1.10f
#define DG_INTERVENTION_MIN_REASSESS_US 5000000LL
#define DG_RESTLESS_ESCALATION_GUARD_US 12000000LL
#define DG_INTERVENTION_RECOVERY_CONFIRM_US 8000000LL
#define DG_INTERVENTION_RECOVERY_COOLDOWN_US 15000000LL
#define DG_FEISHU_STARTUP_WAIT_SEC 8
#define DG_RADAR_STATUS_LOG_US 3000000LL
#define DG_ONLINE_ASR_COMMAND_SEC 6U
#define DG_ONLINE_ASR_START_GRACE_US 400000LL
#define DG_ONLINE_ASR_NEXT_GAP_US 300000LL
#define DG_ONLINE_ASR_REJECT_GAP_US 700000LL
#define DG_ONLINE_ASR_BUSY_IDLE_US 30000000LL
#define DG_VOICE_START_WAIT_MS 20000U
#define DG_RADAR_VITAL_MAX_AGE_US 15000000LL
#define DG_VOICE_START_TASK_STACK 8192U
#define DG_SLEEP_REPORT_RETRY_US 10000000LL
#define DG_SLEEP_PERIODIC_REPORT_US 10000000LL
#define DG_FEISHU_STATUS_LOG_US 10000000LL
#define DG_SNORE_REPORT_RETRY_US 3000000LL
#define DG_SNORE_STATE_CLEAR_US 15000000LL
#define DG_SNORE_PLAYBACK_GATE_VOLUME_PERCENT 20U
#define DG_ASR_GATE_FRAME_SAMPLES 320U
#define DG_ASR_GATE_MAX_FRAMES 260U
#define DG_ASR_GATE_PAD_SAMPLES 3200U
#define DG_ASR_GATE_LEAD_IGNORE_SAMPLES 3200U
#define DG_ASR_GATE_MIN_ACTIVE_MS 420U
#define DG_MANUAL_LIGHT_BRIGHTNESS 0.35f
#define DG_MANUAL_LIGHT_BRIGHTNESS_STEP 0.15f
#define DG_MANUAL_LIGHT_BRIGHTNESS_MIN 0.05f
#define DG_VOICE_SESSION_HARD_TIMEOUT_US 20000000LL

static portMUX_TYPE s_command_mux = portMUX_INITIALIZER_UNLOCKED;
static mobile_app_command_t s_command_queue[MOBILE_COMMAND_QUEUE_LEN];
static uint8_t s_command_head;
static uint8_t s_command_tail;
static uint8_t s_command_count;
static mobile_app_command_t s_immediate_command;
static int64_t s_manual_light_until_us;
static uint8_t s_manual_light_red;
static uint8_t s_manual_light_green;
static uint8_t s_manual_light_blue;
static float s_manual_light_brightness;
static bool s_manual_screen_active;
static uint32_t s_manual_screen_rgb;
static int64_t s_manual_screen_until_us;
static uint8_t s_manual_screen_brightness_percent = 90;
static char s_manual_screen_label[24] = "MANUAL";
static int64_t s_manual_audio_until_us;
static int64_t s_force_outputs_off_until_us;
static char s_scene_preset[16] = "ppm";
static char s_breath_mode[16] = "relax";
static bool s_audio_ready;
static bool s_audio_init_attempted;
static bool s_sleep_assist_scene_audio_active;
static uint8_t s_user_audio_volume_percent = 100;
static volatile bool s_native_sr_running;
static volatile bool s_voice_start_done;
static int64_t s_mid_sleep_intervention_until_us;
static int64_t s_mid_sleep_cooldown_until_us;
static int64_t s_mid_sleep_started_us;
static int64_t s_mid_sleep_night_path_last_update_us;
static int64_t s_intervention_recovery_since_us;
static int64_t s_voice_command_wait_until_us;
static int64_t s_online_asr_start_us;
static bool s_voice_session_active;
static int64_t s_voice_session_idle_until_us;
static int64_t s_voice_session_hard_until_us;
static uint32_t s_online_asr_sequence;
static uint32_t s_online_asr_seen_failed_count;
static uint32_t s_online_chat_seen_failed_count;
static bool s_sleep_voice_assistant_paused;
static bool s_slept_test_mode;
static int64_t s_slept_test_started_us;
static bool s_radar_receiver_started;
static int64_t s_last_radar_start_attempt_us;
static int64_t s_turn_window_started_us;
static int64_t s_turn_first_active_us;
static int64_t s_turn_risk_until_us;
static int64_t s_turn_last_phase_update_us;
static int64_t s_turn_last_active_us;
static int64_t s_turn_demo_last_log_us;
static uint16_t s_turn_active_events;
static uint8_t s_turn_risk_floor;
static float s_turn_peak_motion;
static bool s_turn_strong_escalation_pending;

static void maybe_print_radar_status(const ld6002_snapshot_t *radar, int64_t now_us)
{
    static int64_t s_last_radar_status_us;

    if (!radar || now_us - s_last_radar_status_us < DG_RADAR_STATUS_LOG_US) {
        return;
    }
    s_last_radar_status_us = now_us;
    if (dg_debug_quiet_enabled()) {
        return;
    }
    int64_t presence_age_ms = radar->presence_update_us > 0 && now_us >= radar->presence_update_us ?
        (now_us - radar->presence_update_us) / 1000LL : -1;
    int64_t breath_age_ms = radar->breath_update_us > 0 && now_us >= radar->breath_update_us ?
        (now_us - radar->breath_update_us) / 1000LL : -1;
    int64_t heart_age_ms = radar->heart_update_us > 0 && now_us >= radar->heart_update_us ?
        (now_us - radar->heart_update_us) / 1000LL : -1;
    printf("DG Radar main: p=%d p_age=%lldms b=%.1f b_age=%lldms h=%.1f h_age=%lldms rx=%u last=0x%02x raw=%u ok=%u unk=%u err=%u/%u type=0x%04x len=%u\r\n",
           radar->human_present ? 1 : 0,
           (long long)presence_age_ms,
           (double)radar->breath_rate_bpm,
           (long long)breath_age_ms,
           (double)radar->heart_rate_bpm,
           (long long)heart_age_ms,
           (unsigned)radar->uart_bytes,
           radar->last_byte,
           (unsigned)radar->raw_frames,
           (unsigned)radar->frames,
           (unsigned)radar->unknown_frames,
           (unsigned)radar->checksum_errors,
           (unsigned)radar->parse_errors,
           (unsigned)radar->last_type,
           (unsigned)radar->last_len);
}

static esp_err_t start_radar_receiver(void)
{
    ld6002_task_config_t radar_cfg = {
        .uart_num = DG_LD6002_UART_NUM,
        .tx_gpio = DG_LD6002_UART_TX_GPIO,
        .rx_gpio = DG_LD6002_UART_RX_GPIO,
        .baud_rate = DG_LD6002_UART_BAUD,
    };
    s_last_radar_start_attempt_us = esp_timer_get_time();
    printf("DG Radar: start request uart=%d baud=%d tx=GPIO%d rx=GPIO%d\r\n",
           (int)radar_cfg.uart_num, radar_cfg.baud_rate,
           radar_cfg.tx_gpio, radar_cfg.rx_gpio);
    esp_err_t err = ld6002_task_start(&radar_cfg);
    printf("DG Radar: start result err=%d %s\r\n", (int)err, esp_err_to_name(err));
    if (err == ESP_OK) {
        s_radar_receiver_started = true;
    }
    return err;
}
static bool s_wifi_screen_last_connected;
static uint32_t s_wifi_screen_last_config_version;
static char s_wifi_screen_last_ip[16];
static intervention_action_t s_mid_sleep_action = INTERVENTION_ACTION_NONE;
static uint8_t s_mid_sleep_brightness_percent;
static uint32_t s_mid_sleep_count;
static char s_mid_sleep_reason[96];
static bool s_session_out_of_bed_active;
static bool s_out_of_bed_demo_return_allowed;
static bool s_return_sleep_boost_used_for_event;
static int64_t s_returned_to_bed_since_us;
static int64_t s_out_of_bed_candidate_since_us;
static int64_t s_auto_wake_out_of_bed_since_us;
static int64_t s_auto_wake_active_since_us;
static bool s_sleep_start_report_sent;
static bool s_sleep_onset_report_sent;
static int64_t s_sleep_session_started_us;
static int64_t s_sleep_start_report_last_attempt_us;
static int64_t s_sleep_onset_report_last_attempt_us;
static int64_t s_sleep_periodic_report_last_us;
static uint32_t s_sleep_periodic_report_count;
static int64_t s_feishu_status_last_log_us;
static bool s_sleep_snore_session_active;
static uint32_t s_sleep_snore_observed_events;
static uint32_t s_sleep_snore_detection_count;
static bool s_sleep_snore_detected;
static bool s_sleep_snore_state_report_pending;
static bool s_sleep_snore_state_report_detected;
static int64_t s_sleep_snore_last_detected_us;
static int64_t s_sleep_snore_last_attempt_us;

static esp_err_t ensure_audio_ready(void);
static void clear_mid_sleep_intervention(void);

static void update_sleep_voice_assistant_mode(bool sleep_mode_active, int64_t now_us)
{
    if (sleep_mode_active) {
        if (s_native_sr_running && !s_sleep_voice_assistant_paused) {
            voice_sr_set_assistant_suppressed(true);
            ESP_LOGI(TAG, "voice assistant suppressed for sleep mode; speech is ignored");
            printf("DG Voice: sleep mode active, wake/commands ignored; no waiting UI\r\n");
            s_sleep_voice_assistant_paused = true;
        }
        s_voice_command_wait_until_us = 0;
        s_voice_session_active = false;
        s_voice_session_idle_until_us = 0;
        s_online_asr_start_us = 0;
        return;
    }

    if (s_native_sr_running && s_sleep_voice_assistant_paused) {
        voice_sr_set_assistant_suppressed(false);
        s_sleep_voice_assistant_paused = false;
        ESP_LOGI(TAG, "voice assistant restored after sleep mode");
        printf("DG Voice: assistant wake/commands restored after sleep mode\r\n");
    }
}

static void voice_sr_start_task(void *arg)
{
    (void)arg;
    printf("DG Voice: WakeNet startup begin internal=%u largest=%u psram=%u\r\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    esp_err_t err = voice_sr_start();
    UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(NULL);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_native_sr_running = true;
        ESP_LOGI(TAG, "ESP-SR WakeNet task ready");
        voice_sr_status_t status = {0};
        voice_sr_get_status(&status);
        printf("DG Voice: WakeNet ready err=%s model=%s feed=%u fetch=%u stack_free_min=%u internal=%u largest=%u\r\n",
               esp_err_to_name(err), status.wake_model,
               (unsigned)status.feed_chunk, (unsigned)status.fetch_chunk,
               (unsigned)stack_high_water,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        s_voice_start_done = true;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    ESP_LOGW(TAG, "ESP-SR start failed: %s; starting mic monitor without wake control",
             esp_err_to_name(err));
    voice_sr_status_t status = {0};
    voice_sr_get_status(&status);
    printf("DG Voice: WakeNet failed err=%s stage=%s stack_free_min=%u internal=%u largest=%u psram=%u\r\n",
           esp_err_to_name(err), status.last_result,
           (unsigned)stack_high_water,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    audio_monitor_config_t mic_cfg;
    audio_monitor_get_default_config(&mic_cfg);
    err = audio_monitor_start(&mic_cfg);
    printf("DG Voice: fallback mic monitor err=%s (wake word unavailable)\r\n",
           esp_err_to_name(err));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Audio monitor start failed: %s", esp_err_to_name(err));
    }
    s_voice_start_done = true;
    vTaskDeleteWithCaps(NULL);
}

static void start_voice_or_monitor(void)
{
    s_voice_start_done = false;
    /* voice_sr_start() reads NVS/model flash. Its stack must remain internal
     * because ESP-IDF disables the PSRAM cache during those reads. */
    if (xTaskCreateWithCaps(voice_sr_start_task, "voice_sr_start", DG_VOICE_START_TASK_STACK,
                            NULL, 2, NULL,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS) {
        return;
    }

    ESP_LOGW(TAG, "ESP-SR start task create failed; starting mic monitor without wake control");
    audio_monitor_config_t mic_cfg;
    audio_monitor_get_default_config(&mic_cfg);
    esp_err_t err = audio_monitor_start(&mic_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Audio monitor start failed: %s", esp_err_to_name(err));
    }
    s_voice_start_done = true;
}

static const char *mobile_command_name(mobile_app_command_t command)
{
    switch (command) {
    case MOBILE_APP_COMMAND_SLEEP: return "sleep";
    case MOBILE_APP_COMMAND_SLEPT: return "slept";
    case MOBILE_APP_COMMAND_STOP: return "stop";
    case MOBILE_APP_COMMAND_SELF_TEST: return "self_test";
    case MOBILE_APP_COMMAND_WAKE_ACK: return "wake_ack";
    case MOBILE_APP_COMMAND_LIGHT_RED: return "light_red";
    case MOBILE_APP_COMMAND_LIGHT_GREEN: return "light_green";
    case MOBILE_APP_COMMAND_LIGHT_BLUE: return "light_blue";
    case MOBILE_APP_COMMAND_LIGHT_YELLOW: return "light_yellow";
    case MOBILE_APP_COMMAND_LIGHT_OFF: return "light_off";
    case MOBILE_APP_COMMAND_LIGHT_BRIGHTER: return "light_brighter";
    case MOBILE_APP_COMMAND_LIGHT_DIMMER: return "light_dimmer";
    case MOBILE_APP_COMMAND_SCREEN_RED: return "screen_red";
    case MOBILE_APP_COMMAND_SCREEN_GREEN: return "screen_green";
    case MOBILE_APP_COMMAND_SCREEN_BLUE: return "screen_blue";
    case MOBILE_APP_COMMAND_SCREEN_YELLOW: return "screen_yellow";
    case MOBILE_APP_COMMAND_SCREEN_OFF: return "screen_off";
    case MOBILE_APP_COMMAND_AUDIO_VOLUME_MAX: return "audio_volume_max";
    case MOBILE_APP_COMMAND_AUDIO_VOLUME_UP: return "audio_volume_up";
    case MOBILE_APP_COMMAND_AUDIO_VOLUME_DOWN: return "audio_volume_down";
    case MOBILE_APP_COMMAND_AUDIO_MUTE: return "audio_mute";
    default: return "other";
    }
}

static esp_err_t mobile_command_cb(mobile_app_command_t command, void *ctx)
{
    (void)ctx;
    if (command == MOBILE_APP_COMMAND_NONE) {
        return ESP_OK;
    }
    if (command == MOBILE_APP_COMMAND_SLEEP ||
        command == MOBILE_APP_COMMAND_SLEPT ||
        command == MOBILE_APP_COMMAND_STOP) {
        portENTER_CRITICAL(&s_command_mux);
        s_command_head = 0;
        s_command_tail = 0;
        s_command_count = 0;
        if (command == MOBILE_APP_COMMAND_STOP || s_immediate_command != MOBILE_APP_COMMAND_STOP) {
            s_immediate_command = command;
        }
        portEXIT_CRITICAL(&s_command_mux);
        ESP_LOGI(TAG, "immediate command=%d", command);
        printf("DG Command: immediate %s(%d)\r\n",
               mobile_command_name(command), (int)command);
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_command_mux);
    if (s_command_count >= MOBILE_COMMAND_QUEUE_LEN) {
        s_command_tail = (uint8_t)((s_command_tail + 1) % MOBILE_COMMAND_QUEUE_LEN);
        s_command_count--;
    }
    s_command_queue[s_command_head] = command;
    s_command_head = (uint8_t)((s_command_head + 1) % MOBILE_COMMAND_QUEUE_LEN);
    s_command_count++;
    uint8_t queued = s_command_count;
    portEXIT_CRITICAL(&s_command_mux);
    ESP_LOGI(TAG, "mobile command=%d", command);
    printf("DG Command: queued %s(%d) depth=%u\r\n",
           mobile_command_name(command), (int)command, (unsigned)queued);
    return ESP_OK;
}

esp_err_t dg_console_submit_command(mobile_app_command_t command)
{
    return mobile_command_cb(command, NULL);
}

esp_err_t dg_console_wifi_configure(const char *ssid, const char *password)
{
    esp_err_t err = mobile_app_configure_wifi(ssid, password);
    ESP_LOGI(TAG, "console wifi config ssid=%s result=%s", ssid ? ssid : "", esp_err_to_name(err));
    return err;
}

static void mobile_config_saved_cb(void *ctx)
{
    (void)ctx;
#if DG_FEISHU_AGENT_ENABLED
    ESP_LOGI(TAG, "Runtime config saved; restarting Feishu agent");
    (void)feishu_agent_restart();
#else
    printf("DG Feishu: inbound agent disabled; config saved without ws restart\r\n");
#endif
}

static bool feishu_credentials_configured(void)
{
    nvs_handle_t nvs = 0;
    size_t id_len = 0;
    size_t secret_len = 0;
    bool configured = false;

    if (nvs_open("dg_claw", NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    if (nvs_get_str(nvs, "fs_app_id", NULL, &id_len) == ESP_OK &&
        nvs_get_str(nvs, "fs_secret", NULL, &secret_len) == ESP_OK) {
        configured = id_len > 1 && secret_len > 1;
    }
    nvs_close(nvs);
    return configured;
}

static float clampf_app(float value, float lo, float hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static bool ld6002_field_fresh(int64_t now_us, int64_t update_us, int64_t max_age_us)
{
    return update_us > 0 && now_us >= update_us && (now_us - update_us) <= max_age_us;
}

static bool ld6002_vitals_confirm_presence(const ld6002_snapshot_t *radar, int64_t now_us)
{
    if (!radar) {
        return false;
    }

    bool breath_live = ld6002_field_fresh(now_us, radar->breath_update_us,
                                           DG_RADAR_VITAL_PRESENCE_GUARD_US) &&
                       isfinite(radar->breath_rate_bpm) &&
                       radar->breath_rate_bpm >= 4.0f && radar->breath_rate_bpm <= 30.0f;
    bool heart_live = ld6002_field_fresh(now_us, radar->heart_update_us,
                                          DG_RADAR_VITAL_PRESENCE_GUARD_US) &&
                      isfinite(radar->heart_rate_bpm) &&
                      radar->heart_rate_bpm >= 40.0f && radar->heart_rate_bpm <= 140.0f;
    /* Heart rate is the stronger in-bed veto for this installation: the
     * LD6002 breath estimate can briefly fall below its reliable range while
     * a person is still lying in front of the radar.  A fresh physiological
     * heart stream therefore keeps presence asserted on its own.  Missing
     * heart data does not declare absence; the normal radar-presence and
     * three-second out-of-bed confirmation path still makes that decision. */
    (void)breath_live;
    return heart_live;
}

static float ld6002_bio_confidence(const ld6002_snapshot_t *radar, int64_t now_us,
                                   bool breath_fresh, bool heart_fresh)
{
    if (!radar || radar->raw_frames == 0) {
        return 0.0f;
    }
    /* raw_frames already contains checksum-valid TF frames.  unknown_frames
     * are legitimate LD6002 message types that this application does not
     * consume (for example 0x0A38), so they must not lower bio confidence. */
    uint32_t parsed_total = radar->raw_frames + radar->checksum_errors + radar->parse_errors;
    float parser_quality = parsed_total > 0 ?
        (float)radar->raw_frames / (float)parsed_total : 0.0f;
    parser_quality = clampf_app(parser_quality, 0.0f, 1.0f);
    bool presence_fresh = ld6002_field_fresh(now_us, radar->presence_update_us, 5000000LL);
    float confidence = parser_quality;
    if (!radar->human_present || !presence_fresh) {
        confidence *= 0.35f;
    }
    if (!breath_fresh) {
        confidence *= 0.45f;
    }
    if (!heart_fresh) {
        confidence *= 0.55f;
    }
    return clampf_app(confidence, 0.0f, 1.0f);
}

static float ld6002_phase_motion_index(const ld6002_snapshot_t *radar, int64_t now_us)
{
    static bool have_phase;
    static float previous_phase;
    static int64_t previous_phase_us;

    if (!radar || !radar->human_present ||
        !ld6002_field_fresh(now_us, radar->phase_update_us, 3000000LL) ||
        !isfinite(radar->total_phase)) {
        have_phase = false;
        return 0.0f;
    }

    if (!have_phase || previous_phase_us <= 0 || radar->phase_update_us == previous_phase_us) {
        have_phase = true;
        previous_phase = radar->total_phase;
        previous_phase_us = radar->phase_update_us;
        return 0.03f;
    }

    float dt_sec = (float)(radar->phase_update_us - previous_phase_us) * 0.000001f;
    if (dt_sec <= 0.0f || dt_sec > 5.0f) {
        dt_sec = 1.0f;
    }
    float delta = fabsf(radar->total_phase - previous_phase);
    previous_phase = radar->total_phase;
    previous_phase_us = radar->phase_update_us;

    float motion = (delta / dt_sec) * 0.08f;
    return clampf_app(motion, 0.0f, 2.0f);
}

static void reset_turning_detector(bool clear_hold)
{
    s_turn_window_started_us = 0;
    s_turn_first_active_us = 0;
    s_turn_last_phase_update_us = 0;
    s_turn_last_active_us = 0;
    s_turn_demo_last_log_us = 0;
    s_turn_active_events = 0;
    s_turn_peak_motion = 0.0f;
    if (clear_hold) {
        s_turn_risk_until_us = 0;
        s_turn_risk_floor = 0;
        s_turn_strong_escalation_pending = false;
    }
}

static void apply_sustained_turning_risk(float radar_motion,
                                         int64_t phase_update_us,
                                         bool presence,
                                         const sleep_assist_status_t *assist,
                                         sleep_assessment_t *assessment,
                                         int64_t now_us)
{
    if (!assessment || !assist || !assist->active || !assist->sleep_locked || !presence) {
        reset_turning_detector(true);
        return;
    }

    /* Entering the manual deep-sleep demo restarts the radar receiver.  Phase
     * samples around that restart are discontinuous and the touch operation
     * itself may move the bed, so they must not be counted as turning.  Keep
     * feeding the phase estimator in the main loop, but start this detector
     * from a clean window after the entry grace period. */
    if (s_slept_test_mode && s_slept_test_started_us > 0 &&
        now_us - s_slept_test_started_us <
            (int64_t)DG_SLEPT_TEST_ENTRY_GRACE_SEC * 1000000LL) {
        reset_turning_detector(true);
        return;
    }

    if (phase_update_us > 0 && phase_update_us != s_turn_last_phase_update_us) {
        s_turn_last_phase_update_us = phase_update_us;
        if (s_turn_window_started_us == 0 ||
            now_us - s_turn_window_started_us > DG_TURN_WINDOW_US) {
            s_turn_window_started_us = now_us;
            s_turn_first_active_us = 0;
            s_turn_active_events = 0;
            s_turn_peak_motion = 0.0f;
        }

        float motion_threshold = s_slept_test_mode ?
            DG_DEMO_TURN_MOTION_THRESHOLD : DG_TURN_MOTION_THRESHOLD;
        bool motion_active = radar_motion >= motion_threshold;

        /* In a live demo the person is awake and their ordinary breathing can
         * move the LD6002 phase more than it would during real sleep.  Count
         * only a continuous run of large phase changes.  Isolated phase
         * spikes and normal breathing must expire instead of accumulating in
         * the six-second turning window. */
        if (s_slept_test_mode && !motion_active && s_turn_last_active_us > 0 &&
            now_us - s_turn_last_active_us > DG_DEMO_TURN_CONTINUITY_US) {
            s_turn_window_started_us = now_us;
            s_turn_first_active_us = 0;
            s_turn_last_active_us = 0;
            s_turn_active_events = 0;
            s_turn_peak_motion = 0.0f;
        }

        if (motion_active) {
            if (s_turn_first_active_us == 0) {
                s_turn_first_active_us = now_us;
            }
            s_turn_last_active_us = now_us;
            if (s_turn_active_events < UINT16_MAX) {
                s_turn_active_events++;
            }
            if (radar_motion > s_turn_peak_motion) {
                s_turn_peak_motion = radar_motion;
            }
            int64_t active_span_us = now_us - s_turn_first_active_us;
            bool minor_turning = s_slept_test_mode ?
                (s_turn_active_events >= 5 &&
                 active_span_us >= DG_DEMO_TURN_MINOR_MIN_SPAN_US) :
                (s_turn_active_events >= 3 && active_span_us >= DG_TURN_MIN_SPAN_US);
            if (minor_turning) {
                uint8_t next_floor = 58;
                bool strong_turning = s_slept_test_mode ?
                    (s_turn_active_events >= 8 &&
                     active_span_us >= DG_DEMO_TURN_STRONG_MIN_SPAN_US &&
                     s_turn_peak_motion >= DG_DEMO_TURN_STRONG_PEAK_THRESHOLD) :
                    ((s_turn_active_events >= 6 && active_span_us >= 4000000LL) ||
                     (s_turn_active_events >= 4 && s_turn_peak_motion >= 0.30f));
                if (strong_turning) {
                    next_floor = 78;
                    s_turn_strong_escalation_pending = true;
                }
                if (next_floor > s_turn_risk_floor || now_us >= s_turn_risk_until_us) {
                    printf("DG Motion: sustained turning risk_floor=%u events=%u span_ms=%lld peak=%.3f\r\n",
                           (unsigned)next_floor,
                           (unsigned)s_turn_active_events,
                           (long long)(active_span_us / 1000LL),
                           (double)s_turn_peak_motion);
                }
                if (next_floor > s_turn_risk_floor) {
                    s_turn_risk_floor = next_floor;
                }
                s_turn_risk_until_us = now_us + DG_TURN_RISK_HOLD_US;
            }
        }

        if (s_slept_test_mode &&
            now_us - s_turn_demo_last_log_us >= 1000000LL) {
            printf("DG Motion demo: motion=%.3f threshold=%.2f active=%d events=%u peak=%.3f\r\n",
                   (double)radar_motion,
                   (double)motion_threshold,
                   motion_active ? 1 : 0,
                   (unsigned)s_turn_active_events,
                   (double)s_turn_peak_motion);
            s_turn_demo_last_log_us = now_us;
        }
    }

    /* Once a second, stronger turning burst is confirmed, keep its high-risk
     * decision latched until the current breathing-guide guard can complete.
     * Otherwise the demo baseline (risk 20) can expire the short motion hold
     * and incorrectly report recovery before escalation is evaluated. */
    if (s_turn_strong_escalation_pending) {
        s_turn_risk_floor = 78;
        s_turn_risk_until_us = now_us + DG_TURN_RISK_HOLD_US;
    }

    if (s_turn_risk_until_us > now_us && s_turn_risk_floor > 0) {
        if (assessment->wake_risk < s_turn_risk_floor) {
            assessment->wake_risk = s_turn_risk_floor;
        }
        uint8_t stability_cap = s_turn_risk_floor >= 75 ? 30 : 50;
        if (assessment->stable_sleep_index > stability_cap) {
            assessment->stable_sleep_index = stability_cap;
        }
        assessment->state = SLEEP_STATE_TRANSITION;
        snprintf(assessment->reason, sizeof(assessment->reason),
                 "sustained turning detected; intervention risk floor %u",
                 (unsigned)s_turn_risk_floor);
    } else if (s_turn_risk_until_us > 0 && now_us >= s_turn_risk_until_us) {
        s_turn_risk_until_us = 0;
        s_turn_risk_floor = 0;
    }
}
static mobile_app_command_t take_mobile_command(void)
{
    mobile_app_command_t command = MOBILE_APP_COMMAND_NONE;
    portENTER_CRITICAL(&s_command_mux);
    if (s_command_count > 0) {
        command = s_command_queue[s_command_tail];
        s_command_tail = (uint8_t)((s_command_tail + 1) % MOBILE_COMMAND_QUEUE_LEN);
        s_command_count--;
    }
    portEXIT_CRITICAL(&s_command_mux);
    return command;
}

static mobile_app_command_t take_immediate_command(void)
{
    mobile_app_command_t command = MOBILE_APP_COMMAND_NONE;
    portENTER_CRITICAL(&s_command_mux);
    command = s_immediate_command;
    s_immediate_command = MOBILE_APP_COMMAND_NONE;
    portEXIT_CRITICAL(&s_command_mux);
    return command;
}

static void hold_manual_light(uint8_t red, uint8_t green, uint8_t blue, float brightness, int64_t now_us)
{
    s_manual_light_red = red;
    s_manual_light_green = green;
    s_manual_light_blue = blue;
    s_manual_light_brightness = brightness;
    s_manual_light_until_us = now_us + 60000000LL;
    (void)status_light_sleep_assist_set(red, green, blue, brightness);
}

static void hold_manual_light_for(uint8_t red, uint8_t green, uint8_t blue,
                                  float brightness, int64_t now_us, uint16_t duration_sec)
{
    s_manual_light_red = red;
    s_manual_light_green = green;
    s_manual_light_blue = blue;
    s_manual_light_brightness = brightness;
    s_manual_light_until_us = now_us + (int64_t)duration_sec * 1000000LL;
    (void)status_light_sleep_assist_set(red, green, blue, brightness);
}

static void clear_manual_light(void)
{
    s_manual_light_until_us = 0;
    s_manual_light_red = 0;
    s_manual_light_green = 0;
    s_manual_light_blue = 0;
    s_manual_light_brightness = 0.0f;
    (void)status_light_sleep_assist_set(0, 0, 0, 0.0f);
}

static void hold_manual_screen(uint32_t rgb, int64_t now_us)
{
    s_manual_screen_active = true;
    s_manual_screen_rgb = rgb;
    s_manual_screen_until_us = now_us + 60000000LL;
    s_manual_screen_brightness_percent = 90;
    snprintf(s_manual_screen_label, sizeof(s_manual_screen_label), "%s", "MANUAL");
    (void)sleep_ui_show_solid_color(rgb, s_manual_screen_brightness_percent, s_manual_screen_label);
}

static void hold_manual_screen_for(uint32_t rgb, int64_t now_us, uint16_t duration_sec, const char *label)
{
    s_manual_screen_active = true;
    s_manual_screen_rgb = rgb;
    s_manual_screen_until_us = now_us + (int64_t)duration_sec * 1000000LL;
    s_manual_screen_brightness_percent = 90;
    snprintf(s_manual_screen_label, sizeof(s_manual_screen_label), "%s", label ? label : "MANUAL");
    (void)sleep_ui_show_solid_color(rgb, s_manual_screen_brightness_percent, s_manual_screen_label);
}

static void hold_manual_screen_dim_for(uint32_t rgb, uint8_t brightness_percent,
                                       int64_t now_us, uint16_t duration_sec,
                                       const char *label)
{
    s_manual_screen_active = true;
    s_manual_screen_rgb = rgb;
    s_manual_screen_until_us = now_us + (int64_t)duration_sec * 1000000LL;
    s_manual_screen_brightness_percent = brightness_percent;
    snprintf(s_manual_screen_label, sizeof(s_manual_screen_label), "%s", label ? label : "");
    (void)sleep_ui_show_solid_color(rgb, s_manual_screen_brightness_percent, s_manual_screen_label);
}

static void clear_manual_screen(void)
{
    s_manual_screen_active = false;
    s_manual_screen_rgb = 0;
    s_manual_screen_until_us = 0;
    s_manual_screen_brightness_percent = 90;
    snprintf(s_manual_screen_label, sizeof(s_manual_screen_label), "%s", "MANUAL");
    if (!s_voice_session_active) {
        (void)sleep_ui_show_clock_mode();
    }
}

static void update_wifi_feedback(int64_t now_us)
{
    mobile_app_diag_t diag = {0};
    mobile_app_get_diag(&diag);

    if (diag.sta_config_version != s_wifi_screen_last_config_version) {
        s_wifi_screen_last_config_version = diag.sta_config_version;
        snprintf(s_wifi_screen_last_ip, sizeof(s_wifi_screen_last_ip), "%s", "");
        s_wifi_screen_last_connected = false;
        printf("DG Wi-Fi connecting: ssid=%s setup=http://192.168.4.1/provision\r\n", diag.sta_ssid);
        hold_manual_screen_for(0x0B3D5A, now_us, 20, "WIFI...");
    }

    if (diag.sta_connected && (!s_wifi_screen_last_connected ||
                               strcmp(s_wifi_screen_last_ip, diag.sta_ip) != 0)) {
        char label[24] = {0};
        snprintf(label, sizeof(label), "WIFI OK\n%s", diag.sta_ip[0] ? diag.sta_ip : "NO IP");
        printf("DG Wi-Fi OK: ssid=%s ip=%s dashboard=http://%s/\r\n",
               diag.sta_ssid, diag.sta_ip, diag.sta_ip);
        hold_manual_screen_for(0x0B5A2A, now_us, 5, label);
        s_wifi_screen_last_connected = true;
        snprintf(s_wifi_screen_last_ip, sizeof(s_wifi_screen_last_ip), "%s", diag.sta_ip);
    } else if (!diag.sta_connected && s_wifi_screen_last_connected) {
        printf("DG Wi-Fi disconnected: ssid=%s setup=http://192.168.4.1/provision\r\n", diag.sta_ssid);
        hold_manual_screen_for(0x5A230B, now_us, 15, "WIFI LOST");
        s_wifi_screen_last_connected = false;
        snprintf(s_wifi_screen_last_ip, sizeof(s_wifi_screen_last_ip), "%s", "");
    }
}

static void hold_manual_audio(int64_t now_us, uint16_t duration_sec)
{
    s_manual_audio_until_us = now_us + (int64_t)duration_sec * 1000000LL;
}

static void clear_manual_audio(void)
{
    s_manual_audio_until_us = 0;
    (void)stereo_audio_mute();
}

static void reset_sleep_session_runtime_flags(void)
{
    s_session_out_of_bed_active = false;
    s_out_of_bed_demo_return_allowed = false;
    s_return_sleep_boost_used_for_event = false;
    s_returned_to_bed_since_us = 0;
    s_out_of_bed_candidate_since_us = 0;
    s_auto_wake_out_of_bed_since_us = 0;
    s_auto_wake_active_since_us = 0;
    s_intervention_recovery_since_us = 0;
    s_turn_window_started_us = 0;
    s_turn_first_active_us = 0;
    s_turn_risk_until_us = 0;
    s_turn_last_phase_update_us = 0;
    s_turn_last_active_us = 0;
    s_turn_demo_last_log_us = 0;
    s_turn_active_events = 0;
    s_turn_risk_floor = 0;
    s_turn_peak_motion = 0.0f;
    s_sleep_snore_session_active = false;
    s_sleep_snore_observed_events = 0;
    s_sleep_snore_detection_count = 0;
    s_sleep_snore_detected = false;
    s_sleep_snore_state_report_pending = false;
    s_sleep_snore_state_report_detected = false;
    s_sleep_snore_last_detected_us = 0;
    s_sleep_snore_last_attempt_us = 0;
}

static void format_sleep_report_time(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    struct tm local_time = {0};
    if (time_sync_get_local_time(&local_time, NULL)) {
        snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d:%02d",
                 local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday,
                 local_time.tm_hour, local_time.tm_min, local_time.tm_sec);
    } else {
        snprintf(out, out_size, "%s", "时间同步中");
    }
}

static void append_sleep_snore_status(char *text, size_t text_size, bool final_report)
{
    if (!text || text_size == 0) {
        return;
    }
    size_t used = strlen(text);
    if (used >= text_size) {
        return;
    }
    if (!final_report || s_sleep_snore_detected) {
        snprintf(text + used, text_size - used,
                 "\n鼾声状态：%s",
                 s_sleep_snore_detected ? "检测到用户鼾声" : "未检测到用户鼾声");
    }
    if (!final_report) {
        return;
    }
    used = strlen(text);
    if (used >= text_size) {
        return;
    }
    snprintf(text + used, text_size - used,
             "\n本次睡眠检测到用户鼾声：%u 次",
             (unsigned)s_sleep_snore_detection_count);
}

static void maybe_send_snore_event_report(int64_t now_us)
{
    if (!s_sleep_snore_session_active) {
        return;
    }

    audio_monitor_status_t mic = {0};
    audio_monitor_get_status(&mic);
    if (mic.snore_events != s_sleep_snore_observed_events) {
        s_sleep_snore_observed_events = mic.snore_events;
        s_sleep_snore_last_detected_us = now_us;
        if (!s_sleep_snore_detected) {
            s_sleep_snore_detected = true;
            s_sleep_snore_detection_count++;
            s_sleep_snore_state_report_pending = true;
            s_sleep_snore_state_report_detected = true;
            s_sleep_snore_last_attempt_us = 0;
            printf("DG Snore: state=detected detections=%u\r\n",
                   (unsigned)s_sleep_snore_detection_count);
        }
    } else if (s_sleep_snore_detected && s_sleep_snore_last_detected_us > 0 &&
               now_us - s_sleep_snore_last_detected_us >= DG_SNORE_STATE_CLEAR_US) {
        s_sleep_snore_detected = false;
        s_sleep_snore_state_report_pending = true;
        s_sleep_snore_state_report_detected = false;
        s_sleep_snore_last_attempt_us = 0;
        printf("DG Snore: state=clear detections=%u quiet_ms=%u\r\n",
               (unsigned)s_sleep_snore_detection_count,
               (unsigned)(DG_SNORE_STATE_CLEAR_US / 1000LL));
    }

    if (!s_sleep_snore_state_report_pending) {
        return;
    }
    if (!mobile_app_is_sta_connected()) {
        return;
    }

    char clock_text[32];
    char message[384];
    format_sleep_report_time(clock_text, sizeof(clock_text));
    if (s_sleep_snore_last_attempt_us > 0 &&
        now_us - s_sleep_snore_last_attempt_us < DG_SNORE_REPORT_RETRY_US) {
        return;
    }
    s_sleep_snore_last_attempt_us = now_us;
    if (s_sleep_snore_state_report_detected) {
        snprintf(message, sizeof(message),
                 "DreamGuardian 鼾声确认报告\n"
                 "状态：检测到用户鼾声\n"
                 "时间：%s\n"
                 "声音电平：%.1f dBFS，环境基线：%.1f dBFS\n"
                 "说明：声学脉冲已通过重复节律和雷达呼吸联合验证。",
                 clock_text,
                 (double)mic.level_dbfs, (double)mic.noise_floor_dbfs);
    } else {
        snprintf(message, sizeof(message),
                 "DreamGuardian 鼾声状态更新\n"
                 "状态：未检测到用户鼾声\n"
                 "时间：%s\n"
                 "说明：连续15秒未再确认鼾声，系统已恢复持续监测。",
                 clock_text);
    }
    esp_err_t err = ai_bridge_send_queued_text(message);
    if (err == ESP_OK) {
        s_sleep_snore_state_report_pending = false;
    }
    printf("DG Snore: feishu state=%s queued=%d err=%s\r\n",
           s_sleep_snore_state_report_detected ? "detected" : "clear",
           err == ESP_OK ? 1 : 0, esp_err_to_name(err));
}

static void maybe_send_sleep_start_report(int64_t now_us, bool immediate)
{
    if (s_sleep_start_report_sent ||
        (!immediate && s_sleep_start_report_last_attempt_us > 0 &&
         now_us - s_sleep_start_report_last_attempt_us < DG_SLEEP_REPORT_RETRY_US)) {
        return;
    }
    s_sleep_start_report_last_attempt_us = now_us;
    char clock_text[32];
    char message[320];
    format_sleep_report_time(clock_text, sizeof(clock_text));
    snprintf(message, sizeof(message),
             "DreamGuardian 睡眠阶段报告\n"
             "阶段：睡眠监测已开始\n"
             "时间：%s\n"
             "状态：正在建立个人基线并持续监测呼吸、心率、体动和离床情况。",
             clock_text);
    /* Sleep-session milestones must survive a transient hotspot outage or a
     * reboot. The bridge persists this text in NVS and retries until Feishu
     * acknowledges it instead of dropping it after one HTTP attempt. */
    esp_err_t err = ai_bridge_send_reliable_text(message);
    if (err == ESP_OK) {
        s_sleep_start_report_sent = true;
    }
    printf("DG SleepSession: start report persisted=%d err=%s\r\n",
           s_sleep_start_report_sent ? 1 : 0, esp_err_to_name(err));
}

static void maybe_send_sleep_onset_report(const SleepBioState *bio,
                                          const sleep_assessment_t *assessment,
                                          int64_t now_us)
{
    if (s_sleep_onset_report_sent ||
        (s_sleep_onset_report_last_attempt_us > 0 &&
         now_us - s_sleep_onset_report_last_attempt_us < DG_SLEEP_REPORT_RETRY_US)) {
        return;
    }
    s_sleep_onset_report_last_attempt_us = now_us;
    char clock_text[32];
    char message[400];
    format_sleep_report_time(clock_text, sizeof(clock_text));
    uint32_t onset_min = s_sleep_session_started_us > 0 && now_us > s_sleep_session_started_us ?
        (uint32_t)(((now_us - s_sleep_session_started_us) / 1000000LL + 30) / 60) : 0;
    if (bio && bio->valid) {
        snprintf(message, sizeof(message),
                 "DreamGuardian 睡眠阶段报告\n"
                 "阶段：已检测到用户睡着\n"
                 "时间：%s\n"
                 "入睡用时：%u 分钟\n"
                 "当前睡眠评分：%u/100\n"
                 "呼吸：%.1f bpm，心率：%.1f bpm，体动：%.2f，稳定度：%.2f\n"
                 "状态：已进入安静守护，系统将继续记录整晚睡眠数据。",
                 clock_text, (unsigned)onset_min,
                 assessment ? (unsigned)assessment->sleep_score : 0,
                 (double)(bio->breath_bpm_latest > 0.0f ? bio->breath_bpm_latest : bio->breath_bpm_smooth),
                 (double)(bio->heart_bpm_latest > 0.0f ? bio->heart_bpm_latest : bio->heart_bpm_smooth),
                 (double)bio->motion_smooth, (double)bio->stability_score);
    } else {
        snprintf(message, sizeof(message),
                 "DreamGuardian 睡眠阶段报告\n"
                 "阶段：已检测到用户睡着\n"
                 "时间：%s\n"
                 "入睡用时：%u 分钟\n"
                 "当前睡眠评分：%u/100\n"
                 "生命体征：毫米波雷达正在稳定采集\n"
                 "状态：已进入安静守护，系统将继续记录整晚睡眠数据。",
                 clock_text, (unsigned)onset_min,
                 assessment ? (unsigned)assessment->sleep_score : 0);
    }
    esp_err_t err = ai_bridge_send_reliable_text(message);
    if (err == ESP_OK) {
        s_sleep_onset_report_sent = true;
    }
    printf("DG SleepSession: onset report persisted=%d err=%s\r\n",
           s_sleep_onset_report_sent ? 1 : 0, esp_err_to_name(err));
}

static void start_sleep_session(int64_t now_us)
{
    reset_sleep_session_runtime_flags();
    s_sleep_start_report_sent = false;
    s_sleep_onset_report_sent = false;
    s_sleep_session_started_us = now_us;
    s_sleep_start_report_last_attempt_us = 0;
    s_sleep_onset_report_last_attempt_us = 0;
    s_sleep_periodic_report_last_us = now_us;
    s_sleep_periodic_report_count = 0;
    audio_monitor_status_t mic = {0};
    audio_monitor_get_status(&mic);
    s_sleep_snore_observed_events = mic.snore_events;
    s_sleep_snore_detection_count = 0;
    s_sleep_snore_detected = false;
    s_sleep_snore_state_report_pending = false;
    s_sleep_snore_state_report_detected = false;
    s_sleep_snore_last_detected_us = 0;
    s_sleep_snore_last_attempt_us = 0;
    s_sleep_snore_session_active = true;
    sleep_log_session_start(now_us);
    maybe_send_sleep_start_report(now_us, true);
}

static void finish_sleep_session_report(const char *reason, int64_t now_us)
{
    char report[1024];
    if (!sleep_log_session_finish(reason, now_us, report, sizeof(report))) {
        return;
    }
    append_sleep_snore_status(report, sizeof(report), true);
    printf("DG SleepSession Report:\r\n%s\r\n", report);
    esp_err_t queue_err = ai_bridge_send_reliable_text(report);
    printf("DG SleepSession: report persisted/queued err=%d\r\n", (int)queue_err);
}

static void stop_sleep_mode_with_reason(int64_t now_us, const char *reason)
{
    printf("DG Sleep: stop begin\r\n");
    finish_sleep_session_report(reason ? reason : "manual stop", now_us);
    clear_mid_sleep_intervention();
    reset_sleep_session_runtime_flags();
    s_slept_test_mode = false;
    s_slept_test_started_us = 0;
    s_sleep_periodic_report_last_us = 0;
    s_sleep_assist_scene_audio_active = false;
    voice_prompt_reset_session();
    (void)sleep_assist_stop(now_us);
    sleep_ui_set_sleep_mode_active(false);
    clear_manual_audio();
    hold_manual_light_for(0, 0, 0, 0.0f, now_us, 5);
    hold_manual_screen_dim_for(0x000000, 0, now_us, DG_SLEEP_STOP_BLANK_SEC, "");
    s_force_outputs_off_until_us = now_us + (int64_t)DG_SLEEP_STOP_BLANK_SEC * 1000000LL;
    update_sleep_voice_assistant_mode(false, now_us);
    (void)stereo_audio_mute();
    esp_err_t light_err = status_light_force_off();
    printf("DG Sleep: stop force light off err=%d\r\n", (int)light_err);
    printf("DG Sleep: stopped, outputs muted\r\n");
}

static void stop_sleep_mode(int64_t now_us)
{
    stop_sleep_mode_with_reason(now_us, "manual stop");
}

static void start_sleep_mode_now(int64_t now_us, const char *source, bool sync_voice_control)
{
    bool session_was_active = sleep_log_session_is_active();
    if (sync_voice_control) {
        (void)voice_control_handle_phrase("sleep");
    }
    clear_manual_light();
    clear_manual_screen();
    clear_manual_audio();
    clear_mid_sleep_intervention();
    voice_prompt_reset_session();
    s_sleep_assist_scene_audio_active = false;
    s_slept_test_mode = false;
    s_slept_test_started_us = 0;
    sleep_assist_start(now_us);
    if (!session_was_active) {
        start_sleep_session(now_us);
    }
    sleep_ui_set_sleep_mode_active(true);
    /* Sleep mode is touch-controlled. Ignore all speech and keep the assist
     * animation/buttons visible instead of opening a voice waiting screen. */
    update_sleep_voice_assistant_mode(true, now_us);
    esp_err_t radar_err = ld6002_task_request_recovery();
    if (radar_err != ESP_OK) {
        printf("DG Radar: sleep-entry recovery request err=%s\r\n", esp_err_to_name(radar_err));
    }
    printf("DG Sleep: start source=%s session=%s\r\n",
           source ? source : "command",
           session_was_active ? "continued" : "new");
}

static void start_slept_mode_now(int64_t now_us, const char *source)
{
    bool session_was_active = sleep_log_session_is_active();
    clear_manual_light();
    clear_manual_screen();
    clear_manual_audio();
    clear_mid_sleep_intervention();
    /* A new manual demo is an independent test run.  Do not inherit the
     * previous intervention's cooldown/risk window, otherwise an old high
     * risk can either trigger immediately or suppress the next demonstration. */
    s_mid_sleep_cooldown_until_us = 0;
    s_intervention_recovery_since_us = 0;
    reset_turning_detector(true);
    voice_prompt_reset_session();
    s_sleep_assist_scene_audio_active = false;
    s_slept_test_mode = true;
    s_slept_test_started_us = now_us;
    s_sleep_periodic_report_last_us = now_us;
    (void)sleep_assist_start_locked(now_us);
    if (!session_was_active) {
        start_sleep_session(now_us);
    }
    sleep_ui_set_sleep_mode_active(true);
    update_sleep_voice_assistant_mode(true, now_us);
    esp_err_t radar_err = ld6002_task_request_recovery();
    if (radar_err != ESP_OK) {
        printf("DG Radar: slept-entry recovery request err=%s\r\n", esp_err_to_name(radar_err));
    }
    (void)stereo_audio_mute();
    (void)status_light_force_off();
    (void)sleep_ui_show_sleep_locked_clock();
    printf("DG Sleep: slept lock source=%s session=%s demo_settle=%ds\r\n",
           source ? source : "command",
           session_was_active ? "continued" : "new",
           DG_SLEPT_TEST_ENTRY_GRACE_SEC);
}

static const char *scene_for_command(mobile_app_command_t command)
{
    switch (command) {
    case MOBILE_APP_COMMAND_SCENE_OCEAN:
    case MOBILE_APP_COMMAND_SCENE_OCEAN_PLAY:
        return "ocean";
    case MOBILE_APP_COMMAND_SCENE_FOREST:
    case MOBILE_APP_COMMAND_SCENE_FOREST_PLAY:
        return "forest";
    case MOBILE_APP_COMMAND_SCENE_RAIN:
    case MOBILE_APP_COMMAND_SCENE_RAIN_PLAY:
        return "rain";
    case MOBILE_APP_COMMAND_SCENE_ZEN:
    case MOBILE_APP_COMMAND_SCENE_ZEN_PLAY:
        return "zen";
    case MOBILE_APP_COMMAND_SCENE_EMPTY:
        return "empty";
    case MOBILE_APP_COMMAND_SCENE_PPM:
    case MOBILE_APP_COMMAND_SCENE_PPM_PLAY:
        return "ppm";
    default:
        return NULL;
    }
}

static bool scene_play_command(mobile_app_command_t command)
{
    return command == MOBILE_APP_COMMAND_SCENE_PPM_PLAY ||
           command == MOBILE_APP_COMMAND_SCENE_OCEAN_PLAY ||
           command == MOBILE_APP_COMMAND_SCENE_FOREST_PLAY ||
           command == MOBILE_APP_COMMAND_SCENE_RAIN_PLAY ||
           command == MOBILE_APP_COMMAND_SCENE_ZEN_PLAY;
}

static const char *breath_for_command(mobile_app_command_t command)
{
    switch (command) {
    case MOBILE_APP_COMMAND_BREATH_BOX:
        return "box";
    case MOBILE_APP_COMMAND_BREATH_DEEP:
        return "deep";
    case MOBILE_APP_COMMAND_BREATH_RELAX:
        return "relax";
    default:
        return NULL;
    }
}

static void apply_sleep_preferences(void)
{
    (void)stereo_audio_set_sleep_music_preset(s_scene_preset);
    (void)sleep_ui_set_scene_preset(s_scene_preset);
    (void)sleep_assist_set_breath_mode(s_breath_mode);
}

static void load_sleep_preferences(void)
{
    nvs_handle_t nvs;
    if (nvs_open(DG_PREF_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        apply_sleep_preferences();
        return;
    }
    size_t len = sizeof(s_scene_preset);
    if (nvs_get_str(nvs, DG_PREF_SCENE_KEY, s_scene_preset, &len) != ESP_OK || !s_scene_preset[0]) {
        snprintf(s_scene_preset, sizeof(s_scene_preset), "%s", "ppm");
    }
    len = sizeof(s_breath_mode);
    if (nvs_get_str(nvs, DG_PREF_BREATH_KEY, s_breath_mode, &len) != ESP_OK || !s_breath_mode[0]) {
        snprintf(s_breath_mode, sizeof(s_breath_mode), "%s", "relax");
    }
    nvs_close(nvs);
    apply_sleep_preferences();
}

static void increment_scene_count(nvs_handle_t nvs, const char *scene)
{
    char key[16];
    snprintf(key, sizeof(key), "%s%s", DG_PREF_COUNT_PREFIX, scene ? scene : "ppm");
    int32_t count = 0;
    (void)nvs_get_i32(nvs, key, &count);
    (void)nvs_set_i32(nvs, key, count + 1);
}

static esp_err_t save_scene_preference(const char *scene)
{
    if (!scene || !scene[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DG_PREF_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, DG_PREF_SCENE_KEY, scene);
    if (err == ESP_OK) {
        increment_scene_count(nvs, scene);
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        snprintf(s_scene_preset, sizeof(s_scene_preset), "%s", scene);
        apply_sleep_preferences();
    }
    return err;
}

static esp_err_t save_breath_preference(const char *mode)
{
    if (!mode || !mode[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DG_PREF_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, DG_PREF_BREATH_KEY, mode);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err == ESP_OK) {
        snprintf(s_breath_mode, sizeof(s_breath_mode), "%s", mode);
        apply_sleep_preferences();
    }
    return err;
}

static uint8_t adapt_audio_volume(uint8_t volume_percent)
{
    if (volume_percent == 0 || s_user_audio_volume_percent == 0) {
        return 0;
    }
    float gain = audio_monitor_volume_gain();
    float adjusted = (float)volume_percent * gain * ((float)s_user_audio_volume_percent / 100.0f);
    if (adjusted < 1.0f && volume_percent > 0) {
        adjusted = 1.0f;
    }
    if (adjusted > 100.0f) {
        adjusted = 100.0f;
    }
    return (uint8_t)(adjusted + 0.5f);
}

static esp_err_t set_user_audio_volume(uint8_t volume_percent)
{
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    s_user_audio_volume_percent = volume_percent;
    esp_err_t err = ensure_audio_ready();
    if (err != ESP_OK) {
        printf("DG Audio: volume preset=%u audio not ready err=%d\r\n",
               (unsigned)s_user_audio_volume_percent, (int)err);
        return err;
    }
    err = stereo_audio_set_current_volume(volume_percent);
    printf("DG Audio: user volume=%u err=%d\r\n",
           (unsigned)s_user_audio_volume_percent, (int)err);
    return err;
}

static esp_err_t change_user_audio_volume(int delta_percent)
{
    int next = (int)s_user_audio_volume_percent + delta_percent;
    if (next < 0) {
        next = 0;
    }
    if (next > 100) {
        next = 100;
    }
    return set_user_audio_volume((uint8_t)next);
}

static uint8_t adapt_audio_volume_floor(uint8_t volume_percent, uint8_t minimum_percent)
{
    uint8_t adjusted = adapt_audio_volume(volume_percent);
    return adjusted < minimum_percent ? minimum_percent : adjusted;
}

#define DG_SLEEP_SCENE_DIGITAL_VOLUME_PERCENT 95
#define DG_SYNTH_SCENE_DIGITAL_VOLUME_PERCENT 80
#define DG_SCENE_PREVIEW_DURATION_SEC 180
#define DG_SLEEP_ASSIST_MUSIC_DURATION_SEC 1800
#define DG_MID_SLEEP_COOLDOWN_SEC 180
#define DG_MID_SLEEP_MASKING_VOLUME_PERCENT 24
#define DG_MID_SLEEP_AROUSAL_VOLUME_PERCENT 18
#define DG_NIGHT_PATH_RAMP_SEC 60
#define DG_NIGHT_PATH_UPDATE_INTERVAL_US 50000LL
#define DG_WAKE_ACK_WAV_VOLUME_PERCENT 100
#define DG_WAKE_ACK_FALLBACK_VOLUME_PERCENT 85
#define DG_WAKE_ACK_MIN_VOLUME_PERCENT 70
#define DG_WAKE_ACK_DURATION_SEC 2
#define DG_WAKE_ACK_AUDIO_ENABLED 1
#define DG_WAKE_ACK_SR_PAUSE_MS (DG_WAKE_ACK_DURATION_SEC * 1000U)
#define DG_VOICE_WAITING_SCREEN_SEC 12
#define DG_VOICE_ASR_SCREEN_SEC 6
#define DG_VOICE_CHAT_MAX_AUDIO_SEC 24U

static bool play_wav_or_fallback(const char *path, uint8_t wav_volume, uint16_t duration_sec,
                                 mobile_app_command_t fallback)
{
    (void)duration_sec;
    voice_sr_pause(1000);
    esp_err_t ready_err = ensure_audio_ready();
    if (ready_err != ESP_OK) {
        ESP_LOGW(TAG, "audio not ready for playback: %s", esp_err_to_name(ready_err));
        return false;
    }

    wav_volume = adapt_audio_volume(wav_volume);
    esp_err_t wav_err = stereo_audio_play_wav(path, wav_volume, duration_sec);
    if (wav_err == ESP_OK) {
        return true;
    }
    ESP_LOGI(TAG, "WAV unavailable path=%s err=%s; using fallback=%d",
             path, esp_err_to_name(wav_err), fallback);
    if (fallback == MOBILE_APP_COMMAND_AUDIO_NOISE) {
        return stereo_audio_play_pink_noise(adapt_audio_volume(16), duration_sec) == ESP_OK;
    }
    if (fallback == MOBILE_APP_COMMAND_AUDIO_BREATHING) {
        /* breathing.wav is optional; preserve the decision's already-adapted
         * volume when the packaged file is absent and synthesis is used. */
        return stereo_audio_play_breathing(wav_volume, duration_sec) == ESP_OK;
    }
    return stereo_audio_play_music(adapt_audio_volume(20), duration_sec) == ESP_OK;
}

static const char *scene_wav_path(const char *scene)
{
    if (scene && strcmp(scene, "ocean") == 0) {
        return "/spiffs/ocean.wav";
    }
    if (scene && strcmp(scene, "forest") == 0) {
        return "/spiffs/forest.wav";
    }
    if (scene && strcmp(scene, "rain") == 0) {
        return "/spiffs/rain.wav";
    }
    if (scene && strcmp(scene, "zen") == 0) {
        return "/spiffs/zen.wav";
    }
    return "/spiffs/default.wav";
}

static bool play_current_sleep_scene_with_volume(uint16_t duration_sec, bool loop, uint8_t volume_percent)
{
    voice_sr_pause(1000);
    esp_err_t ready_err = ensure_audio_ready();
    if (ready_err != ESP_OK) {
        ESP_LOGW(TAG, "audio not ready for sleep scene: %s", esp_err_to_name(ready_err));
        return false;
    }

    const char *path = scene_wav_path(s_scene_preset);
    uint8_t effective_volume = adapt_audio_volume(volume_percent);
    printf("DG Audio: sleep scene path=%s volume=%u effective=%u loop=%d duration=%u\r\n",
           path, (unsigned)volume_percent, (unsigned)effective_volume,
           loop ? 1 : 0, (unsigned)duration_sec);
    esp_err_t wav_err = loop ?
        stereo_audio_play_wav_loop(path, effective_volume, duration_sec) :
        stereo_audio_play_wav(path, effective_volume, duration_sec);
    if (wav_err == ESP_OK) {
        ESP_LOGI(TAG, "playing sleep scene wav=%s loop=%d", path, loop);
        return true;
    }
    ESP_LOGI(TAG, "scene WAV unavailable path=%s err=%s; using synthesized fallback",
             path, esp_err_to_name(wav_err));
    uint8_t synth_volume = adapt_audio_volume(DG_SYNTH_SCENE_DIGITAL_VOLUME_PERCENT);
    printf("DG Audio: sleep synth volume=%u effective=%u duration=%u\r\n",
           (unsigned)DG_SYNTH_SCENE_DIGITAL_VOLUME_PERCENT,
           (unsigned)synth_volume,
           (unsigned)duration_sec);
    return stereo_audio_play_music(synth_volume, duration_sec) == ESP_OK;
}

static bool play_current_sleep_scene_internal(uint16_t duration_sec, bool loop)
{
    return play_current_sleep_scene_with_volume(duration_sec, loop, DG_SLEEP_SCENE_DIGITAL_VOLUME_PERCENT);
}

static bool play_current_sleep_scene(uint16_t duration_sec)
{
    return play_current_sleep_scene_internal(duration_sec, false);
}

static bool mid_sleep_intervention_active(int64_t now_us)
{
    return s_mid_sleep_intervention_until_us > now_us;
}

static float mid_sleep_dim_brightness(uint8_t brightness_percent)
{
    return clampf_app((float)brightness_percent / 1000.0f, 0.002f, 0.018f);
}

static void clear_mid_sleep_intervention(void)
{
    s_mid_sleep_intervention_until_us = 0;
    s_mid_sleep_started_us = 0;
    s_mid_sleep_night_path_last_update_us = 0;
    s_mid_sleep_action = INTERVENTION_ACTION_NONE;
    s_mid_sleep_brightness_percent = 0;
    s_mid_sleep_reason[0] = '\0';
    s_intervention_recovery_since_us = 0;
    s_turn_strong_escalation_pending = false;
}

static float smoothstep01(float value)
{
    float x = clampf_app(value, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

static float night_path_ramp_factor(int64_t now_us)
{
    float ramp_sec = s_mid_sleep_started_us > 0 ?
        (float)(now_us - s_mid_sleep_started_us) * 0.000001f : (float)DG_NIGHT_PATH_RAMP_SEC;
    float ramp = smoothstep01(ramp_sec / (float)DG_NIGHT_PATH_RAMP_SEC);
    return ramp * ramp;
}

static float night_path_target_brightness(void)
{
    return clampf_app((float)s_mid_sleep_brightness_percent / 300.0f, 0.0f, 0.045f);
}

static void apply_mid_sleep_night_path_light(int64_t now_us)
{
    if (s_mid_sleep_night_path_last_update_us > 0 &&
        now_us - s_mid_sleep_night_path_last_update_us < DG_NIGHT_PATH_UPDATE_INTERVAL_US) {
        return;
    }
    s_mid_sleep_night_path_last_update_us = now_us;
    (void)status_light_ws2812_night_path(night_path_ramp_factor(now_us), night_path_target_brightness());
}

static void apply_mid_sleep_overlay(int64_t now_us)
{
    if (!mid_sleep_intervention_active(now_us)) {
        return;
    }

    float elapsed = (float)(now_us % 8000000LL) * 0.000001f;
    float phase = elapsed / 8.0f;
    float curve = 0.5f * (1.0f - cosf(6.28318530718f * phase));

    if (s_mid_sleep_action == INTERVENTION_ACTION_NIGHT_PATH) {
        float ramp = night_path_ramp_factor(now_us);
        uint8_t screen_brightness = (uint8_t)clampf_app(1.0f + 6.0f * ramp, 1.0f, 7.0f);
        apply_mid_sleep_night_path_light(now_us);
        (void)sleep_ui_show_solid_color(0x1A1004, screen_brightness, "PATH");
        return;
    }

    float brightness = mid_sleep_dim_brightness(s_mid_sleep_brightness_percent);
    if (s_mid_sleep_action == INTERVENTION_ACTION_PINK_NOISE) {
        brightness *= 0.35f;
    }
    float pulsed = clampf_app(brightness * (0.45f + 0.55f * curve), 0.0f, 0.018f);
    (void)status_light_sleep_assist_set(255, 92, 8, pulsed);
    (void)sleep_ui_sleep_assist_show(phase, pulsed * 10.0f, false);
}

static void build_mid_sleep_test_decision(mobile_app_command_t command, intervention_decision_t *decision)
{
    if (!decision) {
        return;
    }
    memset(decision, 0, sizeof(*decision));
    switch (command) {
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_MINOR:
        decision->action = INTERVENTION_ACTION_PINK_NOISE;
        decision->volume_percent = 12;
        decision->brightness_percent = 0;
        decision->duration_sec = 90;
        snprintf(decision->skill_name, sizeof(decision->skill_name), "%s", "audio.play_pink_noise");
        snprintf(decision->reason, sizeof(decision->reason), "%s", "test: minor sleep disturbance");
        break;
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_RESTLESS:
        decision->action = INTERVENTION_ACTION_STEREO_BREATHING;
        decision->volume_percent = 22;
        decision->brightness_percent = 10;
        decision->duration_sec = 120;
        snprintf(decision->skill_name, sizeof(decision->skill_name), "%s", "audio.stereo_breathing");
        snprintf(decision->reason, sizeof(decision->reason), "%s", "test: restless turning during sleep");
        break;
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_AROUSAL:
        decision->action = INTERVENTION_ACTION_REDUCE_AROUSAL;
        decision->volume_percent = 12;
        decision->brightness_percent = 8;
        decision->duration_sec = 150;
        snprintf(decision->skill_name, sizeof(decision->skill_name), "%s", "policy.reduce_arousal");
        snprintf(decision->reason, sizeof(decision->reason), "%s", "test: high wake risk or dreaming");
        break;
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_OUT_OF_BED:
        decision->action = INTERVENTION_ACTION_NIGHT_PATH;
        decision->volume_percent = 0;
        decision->brightness_percent = 12;
        decision->duration_sec = 120;
        snprintf(decision->skill_name, sizeof(decision->skill_name), "%s", "light.night_path");
        snprintf(decision->reason, sizeof(decision->reason), "%s", "test: out-of-bed night path");
        break;
    default:
        decision->action = INTERVENTION_ACTION_NONE;
        snprintf(decision->skill_name, sizeof(decision->skill_name), "%s", "none");
        snprintf(decision->reason, sizeof(decision->reason), "%s", "no mid-sleep test command");
        break;
    }
}

static void send_intervention_report(const intervention_decision_t *decision,
                                     const SleepBioState *bio,
                                     const sleep_assessment_t *assessment,
                                     bool audio_started)
{
    if (!decision) {
        return;
    }
    const char *situation = "睡眠状态波动";
    const char *basis = "睡眠监测策略触发";
    const char *method = "低刺激睡眠干预";
    switch (decision->action) {
    case INTERVENTION_ACTION_PINK_NOISE:
        situation = "轻微睡眠扰动";
        basis = "生命体征数据有效，苏醒风险为 35–54";
        method = "低音量粉红噪声掩蔽环境声音，并使用极暗暖光呼吸效果";
        break;
    case INTERVENTION_ACTION_STEREO_BREATHING:
        situation = "睡眠不稳定";
        basis = "生命体征数据有效，苏醒风险为 55–74";
        method = "播放立体声呼吸引导，并使用低亮度暖光呼吸效果";
        break;
    case INTERVENTION_ACTION_REDUCE_AROUSAL:
        situation = strstr(decision->reason, "returned to bed") ?
            "起夜回床后仍处于易醒状态" : "翻身频繁有高苏醒风险或梦境波动";
        basis = strstr(decision->reason, "returned to bed") ?
            "离床后已回床并保持安静，执行弱助眠恢复" :
            "生命体征数据有效，苏醒风险不低于 75";
        method = "播放更低音量的睡眠安抚声，并降低灯光和屏幕刺激";
        break;
    case INTERVENTION_ACTION_NIGHT_PATH:
        situation = "检测到用户持续离床";
        basis = "原始雷达和睡眠状态连续 8 秒均确认无人，单次丢帧或大动作不会触发";
        method = "保持扬声器静音，暖色路径灯渐亮，屏幕显示 PATH";
        break;
    case INTERVENTION_ACTION_WARM_LIGHT:
        situation = "入睡过渡不稳定";
        basis = "生命体征数据有效、苏醒风险低于 35，但稳定睡眠指数低于 55";
        method = "仅使用低亮度暖色呼吸灯辅助入睡";
        break;
    default:
        break;
    }

    char clock_text[32];
    char message[768];
    format_sleep_report_time(clock_text, sizeof(clock_text));
    bool is_test = strncmp(decision->reason, "test:", 5) == 0;
    if (bio && bio->valid && assessment) {
        snprintf(message, sizeof(message),
                 "DreamGuardian 睡眠干预报告\n"
                 "类型：%s\n"
                 "状况：%s\n"
                 "时间：%s\n"
                 "判断依据：%s\n"
                 "当前数据：苏醒风险 %u，睡眠评分 %u，呼吸 %.1f bpm，心率 %.1f bpm，体动 %.2f\n"
                 "干预方法：%s\n"
                 "执行参数：音量 %u%%，亮度 %u，持续 %u 秒\n"
                 "执行状态：%s",
                 is_test ? "现场模拟" : "自动检测",
                 situation, clock_text, basis,
                 (unsigned)assessment->wake_risk, (unsigned)assessment->sleep_score,
                 (double)(bio->breath_bpm_latest > 0.0f ? bio->breath_bpm_latest : bio->breath_bpm_smooth),
                 (double)(bio->heart_bpm_latest > 0.0f ? bio->heart_bpm_latest : bio->heart_bpm_smooth),
                 (double)bio->motion_smooth, method,
                 (unsigned)decision->volume_percent,
                 (unsigned)decision->brightness_percent,
                 (unsigned)decision->duration_sec,
                 audio_started ? "干预已启动" : "光效已启动，音频启动失败");
    } else {
        snprintf(message, sizeof(message),
                 "DreamGuardian 睡眠干预报告\n"
                 "类型：%s\n"
                 "状况：%s\n"
                 "时间：%s\n"
                 "判断依据：%s\n"
                 "当前数据：苏醒风险 %u，睡眠评分 %u\n"
                 "干预方法：%s\n"
                 "执行参数：音量 %u%%，亮度 %u，持续 %u 秒\n"
                 "执行状态：%s",
                 is_test ? "现场模拟" : "自动检测",
                 situation, clock_text, basis,
                 assessment ? (unsigned)assessment->wake_risk : 0,
                 assessment ? (unsigned)assessment->sleep_score : 0,
                 method,
                 (unsigned)decision->volume_percent,
                 (unsigned)decision->brightness_percent,
                 (unsigned)decision->duration_sec,
                 audio_started ? "干预已启动" : "光效已启动，音频启动失败");
    }
    esp_err_t report_err = ai_bridge_send_queued_text(message);
    printf("DG SleepIntervention: feishu queued err=%s action=%s\r\n",
           esp_err_to_name(report_err), intervention_action_to_name(decision->action));
}

static const char *sleep_state_report_name(sleep_state_t state)
{
    switch (state) {
    case SLEEP_STATE_AWAKE: return "清醒";
    case SLEEP_STATE_TRANSITION: return "入睡过渡";
    case SLEEP_STATE_LIGHT_TREND: return "浅睡趋势";
    case SLEEP_STATE_DEEP_TREND: return "深睡趋势";
    case SLEEP_STATE_OUT_OF_BED: return "离床";
    default: return "数据校准中";
    }
}

static void maybe_send_periodic_sleep_report(const SleepBioState *bio,
                                             const sleep_assessment_t *assessment,
                                             const sleep_assist_status_t *assist,
                                             const intervention_decision_t *decision,
                                             int64_t now_us)
{
    bool intervention_active = mid_sleep_intervention_active(now_us);
    bool report_mode_active = (assist && assist->active) || intervention_active;
    if (!report_mode_active) {
        s_sleep_periodic_report_last_us = 0;
        return;
    }
    if (s_sleep_periodic_report_last_us == 0) {
        s_sleep_periodic_report_last_us = now_us;
        return;
    }
    if (now_us - s_sleep_periodic_report_last_us < DG_SLEEP_PERIODIC_REPORT_US) {
        return;
    }

    /* Periodic snapshots are expendable. Do not let them fill the small RAM
     * queue while the phone hotspot is unavailable; reliable milestone
     * reports remain persisted and are delivered after reconnection. */
    if (!mobile_app_is_sta_connected()) {
        s_sleep_periodic_report_last_us = now_us;
        printf("DG SleepPeriodic: skipped offline; reliable milestones retained\r\n");
        return;
    }

    const char *mode = intervention_active ? "睡眠干预模式" :
        (assist && assist->sleep_locked ? "睡眠锁定模式" : "未睡着助眠模式");
    const char *action = intervention_active ? intervention_action_to_name(s_mid_sleep_action) :
        (decision && decision->skill_name[0] ? decision->skill_name : "持续监测");
    char clock_text[32];
    char message[768];
    format_sleep_report_time(clock_text, sizeof(clock_text));
    uint32_t next_count = s_sleep_periodic_report_count + 1;
    if (bio && bio->valid && assessment) {
        snprintf(message, sizeof(message),
                 "DreamGuardian 10秒状态快报 #%u\n"
                 "时间：%s\n"
                 "模式：%s\n"
                 "睡眠状态：%s\n"
                 "睡眠评分：%u/100，苏醒风险：%u\n"
                 "呼吸：%.1f bpm，心率：%.1f bpm，体动：%.2f，稳定度：%.2f\n"
                 "当前动作：%s",
                 (unsigned)next_count, clock_text, mode,
                 sleep_state_report_name(assessment->state),
                 (unsigned)assessment->sleep_score, (unsigned)assessment->wake_risk,
                 (double)(bio->breath_bpm_latest > 0.0f ? bio->breath_bpm_latest : bio->breath_bpm_smooth),
                 (double)(bio->heart_bpm_latest > 0.0f ? bio->heart_bpm_latest : bio->heart_bpm_smooth),
                 (double)bio->motion_smooth, (double)bio->stability_score, action);
    } else {
        snprintf(message, sizeof(message),
                 "DreamGuardian 10秒状态快报 #%u\n"
                 "时间：%s\n"
                 "模式：%s\n"
                 "睡眠状态：%s\n"
                 "睡眠评分：%u/100，苏醒风险：%u\n"
                 "生命体征：毫米波雷达正在校准或等待稳定数据\n"
                 "当前动作：%s",
                 (unsigned)next_count, clock_text, mode,
                 assessment ? sleep_state_report_name(assessment->state) : "数据校准中",
                 assessment ? (unsigned)assessment->sleep_score : 0,
                 assessment ? (unsigned)assessment->wake_risk : 0,
                 action);
    }
    append_sleep_snore_status(message, sizeof(message), false);
    esp_err_t err = ai_bridge_send_queued_text(message);
    s_sleep_periodic_report_last_us = now_us;
    if (err == ESP_OK) {
        s_sleep_periodic_report_count = next_count;
    }
    printf("DG SleepPeriodic: count=%u mode=%s breath=%.1f heart=%.1f err=%s\r\n",
           (unsigned)s_sleep_periodic_report_count, mode,
           bio ? (double)(bio->breath_bpm_latest > 0.0f ? bio->breath_bpm_latest : bio->breath_bpm_smooth) : 0.0,
           bio ? (double)(bio->heart_bpm_latest > 0.0f ? bio->heart_bpm_latest : bio->heart_bpm_smooth) : 0.0,
           esp_err_to_name(err));
}

static void maybe_print_feishu_status(int64_t now_us)
{
    if (s_feishu_status_last_log_us > 0 &&
        now_us - s_feishu_status_last_log_us < DG_FEISHU_STATUS_LOG_US) {
        return;
    }
    s_feishu_status_last_log_us = now_us;
    ai_bridge_status_t status = {0};
    ai_bridge_get_status(&status);
    printf("DG FeishuWebhook status: enabled=%d configured=%d worker=%d queued=%u sent=%u dropped=%u http=%d result=%s\r\n",
           status.enabled ? 1 : 0,
           status.configured ? 1 : 0,
           status.worker_running ? 1 : 0,
           (unsigned)status.queued_count,
           (unsigned)status.sent_count,
           (unsigned)status.dropped_count,
           status.last_http_status,
           status.last_result);
}

static bool execute_mid_sleep_intervention(const intervention_decision_t *decision,
                                           const SleepBioState *bio,
                                           const sleep_assessment_t *assessment,
                                           int64_t now_us, bool force)
{
    if (!decision || decision->action == INTERVENTION_ACTION_NONE) {
        return false;
    }
    if (!force && now_us < s_mid_sleep_cooldown_until_us) {
        return false;
    }
    if (!sleep_agent_handle_decision(decision)) {
        return false;
    }

    bool audio_started = true;
    uint8_t volume = decision->volume_percent;
    if (decision->action == INTERVENTION_ACTION_STEREO_BREATHING && volume == 0) {
        volume = DG_MID_SLEEP_MASKING_VOLUME_PERCENT;
    } else if (decision->action == INTERVENTION_ACTION_REDUCE_AROUSAL && volume == 0) {
        volume = DG_MID_SLEEP_AROUSAL_VOLUME_PERCENT;
    }

    switch (decision->action) {
    case INTERVENTION_ACTION_PINK_NOISE:
        audio_started = play_wav_or_fallback("/spiffs/noise.wav", volume,
                                             decision->duration_sec,
                                             MOBILE_APP_COMMAND_AUDIO_NOISE);
        hold_manual_audio(now_us, decision->duration_sec);
        break;
    case INTERVENTION_ACTION_STEREO_BREATHING:
        audio_started = play_wav_or_fallback("/spiffs/breathing.wav", volume,
                                             decision->duration_sec,
                                             MOBILE_APP_COMMAND_AUDIO_BREATHING);
        hold_manual_audio(now_us, decision->duration_sec);
        break;
    case INTERVENTION_ACTION_REDUCE_AROUSAL:
        audio_started = play_current_sleep_scene_with_volume(decision->duration_sec, false, volume);
        hold_manual_audio(now_us, decision->duration_sec);
        break;
    case INTERVENTION_ACTION_WARM_LIGHT:
        break;
    case INTERVENTION_ACTION_NIGHT_PATH:
        /* Out-of-bed guidance must be silent even if white noise or another
         * sleep intervention was already playing. */
        clear_manual_audio();
        s_sleep_assist_scene_audio_active = false;
        audio_started = stereo_audio_mute() == ESP_OK;
        break;
    default:
        return false;
    }

    s_mid_sleep_intervention_until_us = now_us + (int64_t)decision->duration_sec * 1000000LL;
    s_mid_sleep_cooldown_until_us = s_mid_sleep_intervention_until_us +
                                    (int64_t)DG_MID_SLEEP_COOLDOWN_SEC * 1000000LL;
    s_mid_sleep_started_us = now_us;
    s_intervention_recovery_since_us = 0;
    s_sleep_periodic_report_last_us = now_us;
    s_mid_sleep_action = decision->action;
    if (decision->action == INTERVENTION_ACTION_REDUCE_AROUSAL) {
        s_turn_strong_escalation_pending = false;
    }
    s_mid_sleep_brightness_percent = decision->brightness_percent;
    snprintf(s_mid_sleep_reason, sizeof(s_mid_sleep_reason), "%s", decision->reason);
    if (decision->action == INTERVENTION_ACTION_NIGHT_PATH) {
        s_session_out_of_bed_active = true;
        s_return_sleep_boost_used_for_event = false;
        s_returned_to_bed_since_us = 0;
    }
    s_mid_sleep_count++;
    apply_mid_sleep_overlay(now_us);
    send_intervention_report(decision, bio, assessment, audio_started);

    ESP_LOGI(TAG, "mid sleep intervention action=%s audio=%d duration=%us reason=%s count=%u",
             intervention_action_to_name(decision->action),
             audio_started,
             decision->duration_sec,
             decision->reason,
             (unsigned)s_mid_sleep_count);
    return true;
}

static void send_return_to_bed_report(const SleepBioState *bio, int64_t now_us)
{
    char clock_text[32];
    char message[384];
    format_sleep_report_time(clock_text, sizeof(clock_text));
    snprintf(message, sizeof(message),
             "DreamGuardian 睡眠阶段报告\n"
             "阶段：检测到用户返床\n"
             "时间：%s\n"
             "状态：起夜灯正在关闭，重新进入助眠模式\n"
             "返床数据：呼吸 %.1f bpm，心率 %.1f bpm，体动 %.2f\n"
             "后续：持续助眠，检测到用户睡着后自动恢复睡眠锁定",
             clock_text,
             bio && bio->valid ? (double)(bio->breath_bpm_latest > 0.0f ? bio->breath_bpm_latest : bio->breath_bpm_smooth) : 0.0,
             bio && bio->valid ? (double)(bio->heart_bpm_latest > 0.0f ? bio->heart_bpm_latest : bio->heart_bpm_smooth) : 0.0,
             bio ? (double)bio->motion_smooth : 0.0);
    esp_err_t err = ai_bridge_send_queued_text(message);
    printf("DG SleepReturn: feishu queued err=%s\r\n", esp_err_to_name(err));
    (void)now_us;
}

static bool maybe_restart_assist_after_return(const SleepBioState *bio,
                                              const sleep_assessment_t *assessment,
                                              intervention_decision_t *decision,
                                              int64_t now_us)
{
    if (!bio || !assessment || !decision ||
        (!sleep_log_session_is_active() && !s_out_of_bed_demo_return_allowed)) {
        return false;
    }
    if (assessment->state == SLEEP_STATE_OUT_OF_BED || !bio->presence) {
        if (s_session_out_of_bed_active) {
            s_return_sleep_boost_used_for_event = false;
            s_returned_to_bed_since_us = 0;
        }
        return false;
    }
    if (!s_session_out_of_bed_active || s_return_sleep_boost_used_for_event) {
        return false;
    }

    bool quiet_in_bed = s_out_of_bed_demo_return_allowed ?
        (bio->presence && bio->valid) :
        (bio->presence && (!bio->valid || bio->motion_smooth < 0.18f) &&
         assessment->wake_risk < 90);
    if (!quiet_in_bed) {
        s_returned_to_bed_since_us = 0;
        return false;
    }
    if (s_returned_to_bed_since_us == 0) {
        s_returned_to_bed_since_us = now_us;
        return false;
    }
    if (now_us - s_returned_to_bed_since_us <
        (int64_t)DG_RETURN_TO_BED_CONFIRM_SEC * 1000000LL) {
        return false;
    }

    clear_mid_sleep_intervention();
    (void)stereo_audio_mute();
    (void)status_light_force_off();
    send_return_to_bed_report(bio, now_us);
    start_sleep_mode_now(now_us, "return_to_bed", false);
    s_return_sleep_boost_used_for_event = true;
    s_session_out_of_bed_active = false;
    s_returned_to_bed_since_us = 0;
    s_out_of_bed_candidate_since_us = 0;
    memset(decision, 0, sizeof(*decision));
    decision->action = INTERVENTION_ACTION_NONE;
    snprintf(decision->skill_name, sizeof(decision->skill_name), "%s", "sleep.return_assist");
    snprintf(decision->reason, sizeof(decision->reason), "%s",
             "returned to bed; restarted sleep assist until sleep lock");
    printf("DG SleepReturn: presence stable, sleep assist restarted\r\n");
    return true;
}

static bool maybe_start_out_of_bed_path(const ld6002_snapshot_t *radar,
                                        const SleepBioState *bio,
                                        const sleep_assessment_t *assessment,
                                        const sleep_assist_status_t *assist,
                                        intervention_decision_t *decision,
                                        int64_t now_us)
{
    if (!bio || !assessment || !assist || !decision ||
        !assist->active || !assist->sleep_locked) {
        s_out_of_bed_candidate_since_us = 0;
        return false;
    }
    if (s_slept_test_started_us > 0 &&
        now_us - s_slept_test_started_us <
        (int64_t)DG_SLEPT_TEST_ENTRY_GRACE_SEC * 1000000LL) {
        return false;
    }

    bool raw_absent = radar && radar->raw_frames > 0 && !radar->human_present;
    bool out_of_bed = raw_absent && !bio->presence &&
                      assessment->state == SLEEP_STATE_OUT_OF_BED;
    if (!out_of_bed) {
        s_out_of_bed_candidate_since_us = 0;
        return false;
    }

    if (s_out_of_bed_candidate_since_us == 0) {
        s_out_of_bed_candidate_since_us = now_us;
        return false;
    }
    if (now_us - s_out_of_bed_candidate_since_us <
        (int64_t)DG_OUT_OF_BED_CONFIRM_SEC * 1000000LL) {
        return false;
    }

    if (mid_sleep_intervention_active(now_us) &&
        s_mid_sleep_action == INTERVENTION_ACTION_NIGHT_PATH) {
        if (out_of_bed) {
            int64_t keep_until = now_us + 30000000LL;
            if (s_mid_sleep_intervention_until_us < keep_until) {
                s_mid_sleep_intervention_until_us = keep_until;
            }
        }
        s_session_out_of_bed_active = true;
        return false;
    }

    clear_mid_sleep_intervention();
    clear_manual_audio();
    s_sleep_assist_scene_audio_active = false;
    (void)stereo_audio_mute();
    memset(decision, 0, sizeof(*decision));
    decision->action = INTERVENTION_ACTION_NIGHT_PATH;
    decision->volume_percent = 0;
    decision->brightness_percent = 12;
    decision->duration_sec = 180;
    snprintf(decision->skill_name, sizeof(decision->skill_name), "%s", "light.night_path");
    snprintf(decision->reason, sizeof(decision->reason), "%s",
             "sustained radar absence confirmed; audio muted");
    if (execute_mid_sleep_intervention(decision, bio, assessment, now_us, true)) {
        s_session_out_of_bed_active = true;
        s_return_sleep_boost_used_for_event = false;
        s_returned_to_bed_since_us = 0;
        printf("DG Sleep: night path trigger reason=%s raw_p=%d raw_motion=%.3f bio_motion=%.3f audio=muted\r\n",
               decision->reason,
               radar && radar->human_present ? 1 : 0,
               (double)(radar ? ld6002_phase_motion_index(radar, now_us) : 0.0f),
               (double)bio->motion_smooth);
        return true;
    }
    return false;
}

static bool local_time_in_auto_wake_window(const struct tm *local_time, bool valid)
{
    if (!valid || !local_time) {
        return false;
    }
    return local_time->tm_hour >= DG_AUTO_WAKE_START_HOUR &&
           local_time->tm_hour < DG_AUTO_WAKE_END_HOUR;
}

static bool maybe_auto_finish_sleep_session(const SleepBioState *bio,
                                            const sleep_assessment_t *assessment,
                                            const struct tm *local_time,
                                            bool local_time_valid,
                                            int64_t now_us,
                                            const char **reason)
{
    if (!sleep_log_session_is_active() ||
        !local_time_in_auto_wake_window(local_time, local_time_valid) ||
        !assessment || !bio) {
        s_auto_wake_out_of_bed_since_us = 0;
        s_auto_wake_active_since_us = 0;
        return false;
    }

    bool out_of_bed = assessment->state == SLEEP_STATE_OUT_OF_BED || !bio->presence;
    if (out_of_bed) {
        if (s_auto_wake_out_of_bed_since_us == 0) {
            s_auto_wake_out_of_bed_since_us = now_us;
        }
        if (now_us - s_auto_wake_out_of_bed_since_us >=
            (int64_t)DG_AUTO_WAKE_OUT_OF_BED_SEC * 1000000LL) {
            if (reason) {
                *reason = "auto morning wake: out of bed";
            }
            return true;
        }
    } else {
        s_auto_wake_out_of_bed_since_us = 0;
    }

    bool active_awake = bio->presence && bio->valid &&
                        assessment->state == SLEEP_STATE_AWAKE &&
                        bio->motion_smooth >= 0.18f;
    if (active_awake) {
        if (s_auto_wake_active_since_us == 0) {
            s_auto_wake_active_since_us = now_us;
        }
        if (now_us - s_auto_wake_active_since_us >=
            (int64_t)DG_AUTO_WAKE_ACTIVE_SEC * 1000000LL) {
            if (reason) {
                *reason = "auto morning wake: active awake";
            }
            return true;
        }
    } else {
        s_auto_wake_active_since_us = 0;
    }

    return false;
}

static uint8_t intervention_severity(intervention_action_t action)
{
    switch (action) {
    case INTERVENTION_ACTION_WARM_LIGHT:
        return 1;
    case INTERVENTION_ACTION_PINK_NOISE:
        return 2;
    case INTERVENTION_ACTION_STEREO_BREATHING:
        return 3;
    case INTERVENTION_ACTION_REDUCE_AROUSAL:
        return 4;
    case INTERVENTION_ACTION_NIGHT_PATH:
        return 5;
    default:
        return 0;
    }
}

static bool finish_recovered_intervention(const SleepBioState *bio,
                                          const sleep_assessment_t *assessment,
                                          intervention_decision_t *decision,
                                          int64_t now_us)
{
    intervention_action_t completed_action = s_mid_sleep_action;
    clear_manual_audio();
    clear_mid_sleep_intervention();
    s_mid_sleep_cooldown_until_us = now_us + DG_INTERVENTION_RECOVERY_COOLDOWN_US;
    (void)status_light_force_off();
    (void)sleep_ui_show_sleep_locked_clock();

    char clock_text[32];
    char message[512];
    format_sleep_report_time(clock_text, sizeof(clock_text));
    snprintf(message, sizeof(message),
             "DreamGuardian 睡眠干预动态报告\n"
             "状态：苏醒风险已恢复稳定，干预提前结束\n"
             "时间：%s\n"
             "结束动作：%s\n"
             "当前数据：苏醒风险 %u，稳定睡眠指数 %u，呼吸 %.1f bpm，心率 %.1f bpm\n"
             "后续：恢复无声音、无灯光的深睡守护状态",
             clock_text,
             intervention_action_to_name(completed_action),
             assessment ? (unsigned)assessment->wake_risk : 0,
             assessment ? (unsigned)assessment->stable_sleep_index : 0,
             bio ? (double)(bio->breath_bpm_latest > 0.0f ? bio->breath_bpm_latest : bio->breath_bpm_smooth) : 0.0,
             bio ? (double)(bio->heart_bpm_latest > 0.0f ? bio->heart_bpm_latest : bio->heart_bpm_smooth) : 0.0);
    esp_err_t err = ai_bridge_send_queued_text(message);

    memset(decision, 0, sizeof(*decision));
    decision->action = INTERVENTION_ACTION_NONE;
    snprintf(decision->skill_name, sizeof(decision->skill_name), "%s", "sleep.deep_guard");
    snprintf(decision->reason, sizeof(decision->reason), "%s",
             "wake risk recovered; intervention ended early");
    printf("DG SleepIntervention: recovered end old=%s risk=%u stable=%u feishu=%s\r\n",
           intervention_action_to_name(completed_action),
           assessment ? (unsigned)assessment->wake_risk : 0,
           assessment ? (unsigned)assessment->stable_sleep_index : 0,
           esp_err_to_name(err));
    return true;
}

static bool maybe_auto_mid_sleep_intervention(const sleep_features_t *features,
                                              const SleepBioState *bio,
                                              const sleep_assessment_t *assessment,
                                              const sleep_assist_status_t *assist,
                                              intervention_decision_t *decision,
                                              int64_t now_us)
{
    if (!features || !assessment || !assist || !decision) {
        return false;
    }
    if (!assist->active) {
        return false;
    }
    if (!assist->sleep_locked && assessment->state != SLEEP_STATE_LIGHT_TREND &&
        assessment->state != SLEEP_STATE_DEEP_TREND) {
        return false;
    }
    if (assessment->state == SLEEP_STATE_OUT_OF_BED) {
        /* The dedicated out-of-bed path applies debounce, overrides any
         * current audio and owns the return-to-bed state transition. */
        return false;
    }

    intervention_decide(features, assessment, decision);

    if (mid_sleep_intervention_active(now_us)) {
        if (s_mid_sleep_action == INTERVENTION_ACTION_NIGHT_PATH) {
            return false;
        }

        int64_t active_us = s_mid_sleep_started_us > 0 ? now_us - s_mid_sleep_started_us : 0;
        bool recovered = assessment->wake_risk < 35 &&
                         assessment->stable_sleep_index >= 55 &&
                         features->data_quality_ok;
        if (recovered && active_us >= DG_INTERVENTION_MIN_REASSESS_US) {
            if (s_intervention_recovery_since_us == 0) {
                s_intervention_recovery_since_us = now_us;
            }
            if (now_us - s_intervention_recovery_since_us >=
                DG_INTERVENTION_RECOVERY_CONFIRM_US) {
                return finish_recovered_intervention(bio, assessment, decision, now_us);
            }
        } else {
            s_intervention_recovery_since_us = 0;
        }

        bool escalation_guard_elapsed =
            s_mid_sleep_action != INTERVENTION_ACTION_STEREO_BREATHING ||
            active_us >= DG_RESTLESS_ESCALATION_GUARD_US;
        if (active_us >= DG_INTERVENTION_MIN_REASSESS_US &&
            escalation_guard_elapsed &&
            intervention_severity(decision->action) > intervention_severity(s_mid_sleep_action)) {
            intervention_action_t old_action = s_mid_sleep_action;
            intervention_action_t new_action = decision->action;
            clear_manual_audio();
            clear_mid_sleep_intervention();
            (void)status_light_force_off();
            bool started = execute_mid_sleep_intervention(decision, bio, assessment, now_us, true);
            printf("DG SleepIntervention: escalated old=%s new=%s risk=%u started=%d\r\n",
                   intervention_action_to_name(old_action),
                   intervention_action_to_name(new_action),
                   (unsigned)assessment->wake_risk,
                   started ? 1 : 0);
            return started;
        }
        return false;
    }

    if (voice_control_policy_suppressed(now_us)) {
        return false;
    }
    if (decision->action == INTERVENTION_ACTION_NONE) {
        return false;
    }
    return execute_mid_sleep_intervention(decision, bio, assessment, now_us, false);
}

static void mid_sleep_light_task(void *arg)
{
    (void)arg;
    while (true) {
        int64_t now_us = esp_timer_get_time();
        if (mid_sleep_intervention_active(now_us) &&
            s_mid_sleep_action == INTERVENTION_ACTION_NIGHT_PATH) {
            apply_mid_sleep_night_path_light(now_us);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void show_wake_ack_visual(int64_t now_us)
{
    if (sleep_assist_is_active()) {
        return;
    }
    hold_manual_light_for(0, 180, 255, 0.35f, now_us,
                          (uint16_t)(DG_VOICE_COMMAND_WAIT_US / 1000000LL));
    hold_manual_screen_for(0x5A4A00, now_us, DG_VOICE_WAITING_SCREEN_SEC, "WAITING");
}

static void show_voice_asr_ready_visual(int64_t now_us)
{
    if (sleep_assist_is_active()) {
        return;
    }
    hold_manual_screen_for(0x005A8C, now_us, DG_VOICE_ASR_SCREEN_SEC, "SAY");
}

static void show_voice_waiting_visual(int64_t now_us)
{
    if (sleep_assist_is_active()) {
        return;
    }
    hold_manual_screen_for(0x5A4A00, now_us, DG_VOICE_WAITING_SCREEN_SEC, "WAITING");
}

static void show_voice_received_visual(int64_t now_us, const char *label)
{
    if (sleep_assist_is_active()) {
        return;
    }
    hold_manual_screen_for(0x005A28, now_us, 3, label && label[0] ? label : "OK");
}

static void show_voice_upload_visual(int64_t now_us)
{
    show_voice_waiting_visual(now_us);
}

static void show_voice_error_visual(int64_t now_us, const char *label)
{
    if (sleep_assist_is_active()) {
        return;
    }
    hold_manual_screen_for(0x7A1800, now_us, 3, label && label[0] ? label : "ASR ERR");
}

typedef struct {
    bool accepted;
    uint32_t original_samples;
    uint32_t trimmed_samples;
    uint32_t active_ms;
    float rms;
    float noise_rms;
    float threshold_rms;
    int peak;
} asr_gate_result_t;

static bool prepare_asr_audio_for_upload(int16_t *samples, uint32_t *sample_count,
                                         asr_gate_result_t *result)
{
    if (result) {
        memset(result, 0, sizeof(*result));
    }
    if (!samples || !sample_count || *sample_count < DG_ASR_GATE_FRAME_SAMPLES) {
        return false;
    }

    const uint32_t original_count = *sample_count;
    uint32_t analysis_offset = 0;
    if (original_count > DG_ASR_GATE_LEAD_IGNORE_SAMPLES + DG_ASR_GATE_FRAME_SAMPLES) {
        analysis_offset = DG_ASR_GATE_LEAD_IGNORE_SAMPLES;
    }
    const uint32_t analysis_count = original_count - analysis_offset;
    uint32_t frame_count = analysis_count / DG_ASR_GATE_FRAME_SAMPLES;
    if (frame_count > DG_ASR_GATE_MAX_FRAMES) {
        frame_count = DG_ASR_GATE_MAX_FRAMES;
    }
    if (frame_count == 0) {
        return false;
    }

    float frame_rms[DG_ASR_GATE_MAX_FRAMES] = {0};
    int frame_peak[DG_ASR_GATE_MAX_FRAMES] = {0};
    double total_sq = 0.0;
    float min_rms = 1000000.0f;
    float mean_rms = 0.0f;
    int global_peak = 0;

    for (uint32_t f = 0; f < frame_count; ++f) {
        const uint32_t base = analysis_offset + f * DG_ASR_GATE_FRAME_SAMPLES;
        double sum_sq = 0.0;
        int peak = 0;
        for (uint32_t i = 0; i < DG_ASR_GATE_FRAME_SAMPLES; ++i) {
            int v = samples[base + i];
            int av = v < 0 ? -v : v;
            if (av > peak) {
                peak = av;
            }
            sum_sq += (double)v * (double)v;
        }
        float rms = sqrtf((float)(sum_sq / (double)DG_ASR_GATE_FRAME_SAMPLES));
        frame_rms[f] = rms;
        frame_peak[f] = peak;
        total_sq += sum_sq;
        mean_rms += rms;
        if (rms < min_rms) {
            min_rms = rms;
        }
        if (peak > global_peak) {
            global_peak = peak;
        }
    }

    mean_rms /= (float)frame_count;
    float noise_rms = min_rms + (mean_rms - min_rms) * 0.20f;
    float threshold = noise_rms * 3.00f;
    if (threshold < noise_rms + 90.0f) {
        threshold = noise_rms + 90.0f;
    }
    if (threshold < 120.0f) {
        threshold = 120.0f;
    }

    uint32_t first_active = UINT32_MAX;
    uint32_t last_active = 0;
    uint32_t active_frames = 0;
    int peak_threshold = (int)(noise_rms * 8.0f);
    if (peak_threshold < 850) {
        peak_threshold = 850;
    }

    for (uint32_t f = 0; f < frame_count; ++f) {
        bool active = frame_rms[f] >= threshold || frame_peak[f] >= peak_threshold;
        if (active) {
            if (first_active == UINT32_MAX) {
                first_active = f;
            }
            last_active = f;
            active_frames++;
        }
    }

    uint32_t active_ms = (active_frames * DG_ASR_GATE_FRAME_SAMPLES * 1000U) / 16000U;
    float overall_rms = sqrtf((float)(total_sq / (double)(frame_count * DG_ASR_GATE_FRAME_SAMPLES)));
    bool accepted = first_active != UINT32_MAX &&
                    active_ms >= DG_ASR_GATE_MIN_ACTIVE_MS &&
                    overall_rms >= noise_rms + 35.0f &&
                    global_peak >= 700;

    uint32_t trimmed_count = original_count;
    if (accepted) {
        uint32_t start = analysis_offset + first_active * DG_ASR_GATE_FRAME_SAMPLES;
        uint32_t end = analysis_offset + (last_active + 1U) * DG_ASR_GATE_FRAME_SAMPLES;
        start = start > DG_ASR_GATE_PAD_SAMPLES ? start - DG_ASR_GATE_PAD_SAMPLES : 0;
        end = end + DG_ASR_GATE_PAD_SAMPLES < original_count ? end + DG_ASR_GATE_PAD_SAMPLES : original_count;
        if (end > start && end - start >= DG_ASR_GATE_FRAME_SAMPLES) {
            trimmed_count = end - start;
            if (start > 0) {
                memmove(samples, samples + start, trimmed_count * sizeof(int16_t));
            }
            *sample_count = trimmed_count;
        }
    }

    if (result) {
        result->accepted = accepted;
        result->original_samples = original_count;
        result->trimmed_samples = accepted ? *sample_count : 0;
        result->active_ms = active_ms;
        result->rms = overall_rms;
        result->noise_rms = noise_rms;
        result->threshold_rms = threshold;
        result->peak = global_peak;
    }
    return accepted;
}

static const char *online_asr_screen_error_label(const online_asr_status_t *status)
{
    if (!status) {
        return "ASR ERR";
    }
    if (!status->configured) {
        return "NO KEY";
    }
    if (status->last_http_status == 401 || status->last_http_status == 403) {
        return "KEY ERR";
    }
    if (status->last_http_status == 408 || status->last_http_status == 504) {
        return "TIMEOUT";
    }
    if (status->last_http_status == 429) {
        return "RATE ERR";
    }
    if (status->last_http_status == 0) {
        return "NET ERR";
    }
    return "ASR ERR";
}

static void voice_session_stop(int64_t now_us, const char *reason)
{
    if (s_voice_session_active) {
        printf("DG Voice: session exit reason=%s\r\n", reason ? reason : "unknown");
    }
    s_voice_session_active = false;
    s_voice_session_idle_until_us = 0;
    s_voice_session_hard_until_us = 0;
    s_voice_command_wait_until_us = 0;
    s_online_asr_start_us = 0;
    clear_manual_light();
    clear_manual_screen();
    (void)now_us;
}

static void voice_session_mark_activity(int64_t now_us)
{
    bool was_active = s_voice_session_active;
    s_voice_session_active = true;
    s_voice_session_idle_until_us = now_us + DG_VOICE_SESSION_IDLE_US;
    if (!was_active || s_voice_session_hard_until_us <= 0) {
        s_voice_session_hard_until_us = now_us + DG_VOICE_SESSION_HARD_TIMEOUT_US;
    }
    s_voice_command_wait_until_us = s_voice_session_idle_until_us;
}

static bool voice_session_recording_or_scheduled(int64_t now_us)
{
    voice_sr_status_t sr_status = {0};
    if (s_native_sr_running) {
        voice_sr_get_status(&sr_status);
    }
    return sr_status.command_audio_active ||
           (s_online_asr_start_us > 0 && s_online_asr_start_us > now_us);
}

static bool voice_session_busy_for_idle(int64_t now_us)
{
    return online_asr_is_busy() || online_chat_is_busy() || voice_session_recording_or_scheduled(now_us);
}

static void voice_session_extend_for_asr_busy(int64_t now_us)
{
    if (!s_voice_session_active) {
        return;
    }
    if (s_voice_session_hard_until_us > 0 && now_us >= s_voice_session_hard_until_us) {
        voice_session_stop(now_us, "hard_timeout");
        return;
    }
    if (s_voice_session_hard_until_us > 0 && now_us >= s_voice_session_hard_until_us) {
        voice_session_stop(now_us, "hard_timeout");
        return;
    }
    s_voice_session_idle_until_us = now_us + DG_ONLINE_ASR_BUSY_IDLE_US;
    s_voice_command_wait_until_us = s_voice_session_idle_until_us;
}

static void voice_session_schedule_asr(int64_t now_us, int64_t delay_us, const char *reason)
{
    if (!s_voice_session_active || !online_asr_is_ready() || online_asr_is_busy() || online_chat_is_busy()) {
        return;
    }
    if (s_voice_session_idle_until_us > 0 && now_us >= s_voice_session_idle_until_us) {
        voice_session_stop(now_us, "idle_timeout");
        return;
    }
    s_online_asr_start_us = now_us + delay_us;
    if (delay_us <= 0) {
        show_voice_asr_ready_visual(now_us);
    }
    printf("DG ASR: scheduled reason=%s delay_ms=%u idle_left_ms=%d\r\n",
           reason ? reason : "session",
           (unsigned)(delay_us / 1000LL),
           (int)((s_voice_session_idle_until_us - now_us) / 1000LL));
}

static void voice_session_refresh_screen(int64_t now_us)
{
    if (sleep_assist_is_active()) {
        if (s_voice_session_active) {
            voice_session_stop(now_us, "sleep_mode");
        }
        return;
    }
    if (!s_voice_session_active) {
        return;
    }
    if (s_voice_session_idle_until_us > 0 && now_us >= s_voice_session_idle_until_us) {
        if (voice_session_busy_for_idle(now_us)) {
            voice_session_extend_for_asr_busy(now_us);
        } else {
            voice_session_stop(now_us, "idle_timeout");
            return;
        }
    }
    if (!s_voice_session_active) {
        return;
    }
    voice_sr_status_t sr_status = {0};
    if (s_native_sr_running) {
        voice_sr_get_status(&sr_status);
    }
    bool scheduled = s_online_asr_start_us > 0 && s_online_asr_start_us > now_us;
    if (online_asr_is_busy() || online_chat_is_busy() || scheduled) {
        hold_manual_screen_for(0x5A4A00, now_us, DG_VOICE_WAITING_SCREEN_SEC, "WAITING");
    } else if (sr_status.command_audio_active) {
        hold_manual_screen_for(0x005A8C, now_us, DG_VOICE_ASR_SCREEN_SEC, "SAY");
    } else {
        hold_manual_screen_for(0x5A4A00, now_us, 1, "WAITING");
    }
}

static bool voice_session_stop_if_idle(int64_t now_us)
{
    if (!s_voice_session_active) {
        return true;
    }
    if (s_voice_session_hard_until_us > 0 && now_us >= s_voice_session_hard_until_us) {
        voice_session_stop(now_us, "hard_timeout");
        return true;
    }
    if (voice_session_busy_for_idle(now_us)) {
        voice_session_extend_for_asr_busy(now_us);
        return false;
    }
    if (s_voice_session_idle_until_us > 0 && now_us >= s_voice_session_idle_until_us) {
        voice_session_stop(now_us, "idle_timeout");
        return true;
    }
    return false;
}

static void play_wake_ack(int64_t now_us)
{
    show_wake_ack_visual(now_us);
    voice_sr_pause(DG_WAKE_ACK_SR_PAUSE_MS);
    printf("DG Wake: ack start sr_pause_ms=%u\r\n", (unsigned)DG_WAKE_ACK_SR_PAUSE_MS);
#if !DG_WAKE_ACK_AUDIO_ENABLED
    printf("DG Wake: ack audio skipped for online ASR\r\n");
    return;
#endif
    esp_err_t ready_err = ensure_audio_ready();
    if (ready_err != ESP_OK) {
        ESP_LOGW(TAG, "audio not ready for wake ack: %s", esp_err_to_name(ready_err));
        printf("DG Wake: ack audio not ready err=%s\r\n", esp_err_to_name(ready_err));
        return;
    }

    uint8_t wav_volume = adapt_audio_volume_floor(DG_WAKE_ACK_WAV_VOLUME_PERCENT,
                                                  DG_WAKE_ACK_MIN_VOLUME_PERCENT);
    esp_err_t wav_err = stereo_audio_play_wav("/spiffs/zai_ne.wav",
                                              wav_volume, DG_WAKE_ACK_DURATION_SEC);
    if (wav_err != ESP_OK) {
        uint8_t fallback_volume = adapt_audio_volume_floor(DG_WAKE_ACK_FALLBACK_VOLUME_PERCENT,
                                                           DG_WAKE_ACK_MIN_VOLUME_PERCENT);
        esp_err_t fallback_err = stereo_audio_play_wake_ack(fallback_volume,
                                                            DG_WAKE_ACK_DURATION_SEC);
        printf("DG Wake: ack wav failed err=%s fallback=%s volume=%u\r\n",
               esp_err_to_name(wav_err), esp_err_to_name(fallback_err),
               (unsigned)fallback_volume);
    } else {
        printf("DG Wake: ack wav requested volume=%u duration=%u\r\n",
               (unsigned)wav_volume, (unsigned)DG_WAKE_ACK_DURATION_SEC);
    }
    voice_sr_pause(DG_WAKE_ACK_SR_PAUSE_MS);
    hold_manual_audio(now_us, DG_WAKE_ACK_DURATION_SEC);
}

static void run_hardware_self_test(void)
{
    ESP_LOGI(TAG, "hardware self-test start");
    status_light_self_test();
    sleep_ui_self_test();
    esp_err_t audio_test_err = ESP_ERR_INVALID_STATE;
    if (ensure_audio_ready() == ESP_OK) {
        audio_test_err = stereo_audio_self_test();
    }
    printf("DG Audio: speaker self-test err=%s\r\n", esp_err_to_name(audio_test_err));
    ESP_LOGI(TAG, "hardware self-test done");
}

static void init_audio_storage(void)
{
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS already ready: used=%u total=%u", (unsigned)used, (unsigned)total);
        return;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SPIFFS already mounted");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return;
    }
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS ready: used=%u total=%u", (unsigned)used, (unsigned)total);
    }
}

static esp_err_t ensure_audio_ready(void)
{
#if DG_AUDIO_SAFE_ISOLATION || DG_SPEAKER_SAFE_DISABLED
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (s_audio_ready) {
        return ESP_OK;
    }
    if (s_audio_init_attempted) {
        return ESP_ERR_INVALID_STATE;
    }
    s_audio_init_attempted = true;

    stereo_audio_config_t audio_cfg = {
        .i2s_port = DG_STEREO_I2S_NUM,
        .bclk_gpio = DG_STEREO_I2S_BCLK_GPIO,
        .ws_gpio = DG_STEREO_I2S_WS_GPIO,
        .dout_gpio = DG_STEREO_I2S_DOUT_GPIO,
        .sample_rate_hz = DG_STEREO_SAMPLE_RATE_HZ,
    };
    ESP_LOGI(TAG, "stereo audio lazy init start");
    esp_err_t err = stereo_audio_init(&audio_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stereo audio lazy init failed: %s", esp_err_to_name(err));
        s_audio_init_attempted = false;
        return err;
    }
    s_audio_ready = true;
    (void)stereo_audio_mute();
    ESP_LOGI(TAG, "stereo audio lazy init complete");
    return ESP_OK;
}

static esp_err_t sleep_assist_audio_set(float volume, float target_breath_bpm, float pan_depth, bool rhythm_enabled)
{
    (void)target_breath_bpm;
    (void)pan_depth;
    (void)rhythm_enabled;
    if (volume > 0.0001f) {
        if (!s_sleep_assist_scene_audio_active) {
            s_sleep_assist_scene_audio_active =
                play_current_sleep_scene_internal(DG_SLEEP_ASSIST_MUSIC_DURATION_SEC, true);
            return s_sleep_assist_scene_audio_active ? ESP_OK : ESP_FAIL;
        }
        return ESP_OK;
    } else if (!s_audio_ready) {
        s_sleep_assist_scene_audio_active = false;
        return ESP_OK;
    } else if (mid_sleep_intervention_active(esp_timer_get_time())) {
        return ESP_OK;
    }
    s_sleep_assist_scene_audio_active = false;
    return stereo_audio_mute();
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    ESP_LOGI(TAG, "Booting %s", DG_DEVICE_ID);
    printf("DG Boot: reset_reason=%d\r\n", (int)esp_reset_reason());

#if 1
    /* Recovery guard: silence the external power amplifier before any codec,
     * I2S or application task can touch the shared audio hardware. */
    (void)gpio_reset_pin(BSP_POWER_AMP_IO);
    (void)gpio_set_direction(BSP_POWER_AMP_IO, GPIO_MODE_OUTPUT);
    (void)gpio_set_level(BSP_POWER_AMP_IO, 0);
    printf("DG Audio: safe boot, PA forced off until playback\r\n");
#endif

    /*
     * I2S channels and their DMA descriptors must live in internal RAM.  Reserve
     * them before the radar UART and Wi-Fi/HTTP fragment that heap; otherwise the
     * late microphone start can fail with ESP_ERR_NO_MEM.
     */
    printf("DG Audio: early I2S reserve internal=%u largest=%u\r\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    /* Initialize only the codec control bus here.  Let the BOX-3 microphone
     * helper create/configure/enable I2S in one transaction later; splitting
     * I2S creation from codec open leaves RX in an invalid driver state. */
    esp_err_t err = bsp_i2c_init();
    printf("DG Audio: early I2C control init err=%s internal=%u largest=%u\r\n",
           esp_err_to_name(err),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Early I2S reserve failed: %s; voice/audio will retry later", esp_err_to_name(err));
    }
    /* Keep tiny control objects internal, but let ordinary allocations above
     * this limit use PSRAM. This is the runtime equivalent of the sdkconfig
     * setting and also applies when updating an existing build directory. */
    heap_caps_malloc_extmem_enable(256);
    printf("DG Heap: external malloc threshold=256 internal=%u largest=%u\r\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    (void)gpio_set_direction(BSP_MUTE_STATUS, GPIO_MODE_INPUT);
    int hardware_mute_level = gpio_get_level(BSP_MUTE_STATUS);
    printf("DG Audio: hardware mute gpio=%d level=%d active=%d\r\n",
           (int)BSP_MUTE_STATUS, hardware_mute_level,
           hardware_mute_level == 0 ? 1 : 0);

    /* Reserve the radar UART/ring buffer/task before Wi-Fi, TLS and ESP-SR consume RAM. */
    err = start_radar_receiver();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LD6002 early start failed: %s", esp_err_to_name(err));
    }

    sleep_ui_init();
    sleep_ui_register_command_callback(mobile_command_cb, NULL);
    sleep_log_init();
    sleep_agent_init();
    voice_control_init();
    voice_control_console_start();

    status_light_config_t light_cfg = {
        .red_gpio = DG_RGB_LED_R_GPIO,
        .green_gpio = DG_RGB_LED_G_GPIO,
        .blue_gpio = DG_RGB_LED_B_GPIO,
        .active_low = DG_RGB_LED_ACTIVE_LOW,
        .ws2812_enabled = DG_STATUS_LIGHT_WS2812,
        .ws2812_gpio = DG_WS2812_LED_GPIO,
        .ws2812_count = DG_WS2812_LED_COUNT,
    };
    err = status_light_init(&light_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Status light init failed: %s", esp_err_to_name(err));
    }
    (void)xTaskCreate(mid_sleep_light_task, "mid_sleep_light", 2048, NULL, 2, NULL);

    bio_filter_init(NULL);
    voice_prompt_init(NULL);
    sleep_assist_config_t assist_cfg;
    sleep_assist_get_default_config(&assist_cfg);
    assist_cfg.led_set = status_light_sleep_assist_set;
    assist_cfg.screen_set = sleep_ui_sleep_assist_show;
    assist_cfg.audio_set = sleep_assist_audio_set;
    assist_cfg.voice_play = voice_prompt_play;
    sleep_assist_init(&assist_cfg);

    /* BOX-3 codecs share one full-duplex I2S data interface.  Open the
     * speaker first while its PA is held off, then open the microphone last
     * so the final shared-bus configuration belongs to the recording side. */
    init_audio_storage();
    err = ensure_audio_ready();
    printf("DG Audio: speaker pre-voice startup err=%s internal=%u largest=%u\r\n",
           esp_err_to_name(err),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#if DG_AUDIO_SAFE_ISOLATION
    s_voice_start_done = true;
    s_native_sr_running = false;
    printf("DG Voice: startup skipped by safe isolation\r\n");
#else
    start_voice_or_monitor();
#endif
    uint32_t voice_waited_ms = 0;
    while (!s_voice_start_done && voice_waited_ms < DG_VOICE_START_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        voice_waited_ms += 100;
    }
    printf("DG Voice: startup complete=%d native=%d waited_ms=%u\r\n",
           s_voice_start_done ? 1 : 0, s_native_sr_running ? 1 : 0,
           (unsigned)voice_waited_ms);
    if (s_native_sr_running) {
        vTaskDelay(pdMS_TO_TICKS(500));
        voice_sr_status_t voice_status = {0};
        voice_sr_get_status(&voice_status);
        printf("DG Voice: microphone live feed_task=%d detect_task=%d feed_frames=%u fetch_frames=%u read_errors=%u peak=%u/%u\r\n",
               voice_status.feed_task_running ? 1 : 0,
               voice_status.detect_task_running ? 1 : 0,
               (unsigned)voice_status.feed_frames,
               (unsigned)voice_status.fetch_frames,
               (unsigned)voice_status.read_errors,
               (unsigned)voice_status.mic_peak_ch0,
               (unsigned)voice_status.mic_peak_ch1);
    }

    mobile_app_config_t mobile_cfg;
    mobile_app_get_default_config(&mobile_cfg);
    mobile_cfg.command_cb = mobile_command_cb;
    mobile_cfg.config_saved_cb = mobile_config_saved_cb;
    mobile_cfg.http_enabled = true;
    err = mobile_app_start(&mobile_cfg);
    printf("DG HTTP: mobile app start err=%s\r\n", esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mobile app start failed: %s", esp_err_to_name(err));
    }

    err = ai_bridge_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AI bridge init failed: %s", esp_err_to_name(err));
    }
    err = online_asr_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Online ASR init failed: %s", esp_err_to_name(err));
    }
    load_sleep_preferences();

    for (int i = 0; i < 30 && !mobile_app_is_sta_connected(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    err = online_chat_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Online chat init failed: %s", esp_err_to_name(err));
    }

#if DG_FEISHU_AGENT_ENABLED
    for (int i = 0; i < 30 && !s_voice_start_done; ++i) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    bool feishu_configured_at_boot = feishu_credentials_configured();
#if DG_FEISHU_AGENT_ENABLED
    feishu_agent_config_t feishu_cfg = {
        .command_cb = mobile_command_cb,
    };
    err = feishu_agent_start(&feishu_cfg);
    printf("DG Feishu: agent start err=%d configured_at_boot=%d\r\n",
           (int)err, feishu_configured_at_boot ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Feishu agent start failed: %s", esp_err_to_name(err));
    }

    if (feishu_configured_at_boot) {
        for (int i = 0; i < DG_FEISHU_STARTUP_WAIT_SEC; ++i) {
            feishu_agent_status_t fs_status = {0};
            feishu_agent_get_status(&fs_status);
            if (fs_status.ws_connected) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
#else
    if (feishu_configured_at_boot) {
        ESP_LOGW(TAG, "Feishu agent configured but deferred; local voice/http mode is enabled");
        printf("DG Feishu: configured but deferred for local voice/http stability\r\n");
    }
#endif

#if DG_FEISHU_AGENT_ENABLED
    feishu_agent_status_t fs_status = {0};
    feishu_agent_get_status(&fs_status);
    printf("DG Feishu: status running=%d configured=%d ws=%d ever=%d frames=%u events=%u msg=%u cmd=%u last=%s\r\n",
           fs_status.running ? 1 : 0,
           fs_status.configured ? 1 : 0,
           fs_status.ws_connected ? 1 : 0,
           fs_status.ws_ever_connected ? 1 : 0,
           (unsigned)fs_status.frame_count,
           (unsigned)fs_status.event_count,
           (unsigned)fs_status.message_event_count,
           (unsigned)fs_status.command_count,
           fs_status.last_result);
    if (feishu_configured_at_boot && !fs_status.ws_connected) {
        ESP_LOGW(TAG, "Feishu not connected after %d s startup wait; continuing local voice/http startup",
                 DG_FEISHU_STARTUP_WAIT_SEC);
    }
#endif

    err = time_sync_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Time sync start failed: %s", esp_err_to_name(err));
    }
    err = csi_monitor_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CSI monitor start failed: %s", esp_err_to_name(err));
    }
    /* Do not run the destructive screen/audio diagnostic on every boot.  It
     * replaces the normal LVGL tree and drives the speaker at test volume;
     * keep it available only through the explicit `selftest` command. */
    ESP_LOGI(TAG, "Startup hardware self-test skipped; use explicit selftest command");

    uint32_t last_voice_command_count = 0;

    while (true) {
        ld6002_snapshot_t radar = {0};
        sleep_features_t features = {0};
        sleep_assessment_t assessment = {0};

        ld6002_get_snapshot(&radar);
        int64_t now_us = esp_timer_get_time();
        bool radar_reported_presence = radar.human_present;
        bool radar_presence_fresh = ld6002_field_fresh(now_us,
                                                       radar.presence_update_us,
                                                       DG_RADAR_PRESENCE_MAX_AGE_US);
        bool radar_vital_presence_guard = ld6002_vitals_confirm_presence(&radar, now_us);
        /* LD6002 can briefly publish an absent presence bit while it is still
         * producing fresh, physiological breath and heart measurements.  Do
         * not turn that one-field disagreement into an out-of-bed event.  The
         * guard expires quickly after either vital stream stops, after which
         * the normal three-second absence confirmation starts from zero. */
        radar.human_present = (radar_reported_presence && radar_presence_fresh) ||
                              radar_vital_presence_guard;
        if (!radar.human_present) {
            radar.breath_rate_bpm = 0.0f;
            radar.heart_rate_bpm = 0.0f;
            radar.breath_update_us = 0;
            radar.heart_update_us = 0;
        }
        static bool presence_log_initialized;
        static bool last_effective_presence;
        static bool last_vital_presence_guard;
        if (!presence_log_initialized || last_effective_presence != radar.human_present ||
            last_vital_presence_guard != radar_vital_presence_guard) {
            int64_t presence_age_ms = radar.presence_update_us > 0 && now_us >= radar.presence_update_us ?
                (now_us - radar.presence_update_us) / 1000LL : -1;
            printf("DG Radar presence: effective=%d reported=%d fresh=%d vital_guard=%d age_ms=%lld\r\n",
                   radar.human_present ? 1 : 0,
                   radar_reported_presence ? 1 : 0,
                   radar_presence_fresh ? 1 : 0,
                   radar_vital_presence_guard ? 1 : 0,
                   (long long)presence_age_ms);
            presence_log_initialized = true;
            last_effective_presence = radar.human_present;
            last_vital_presence_guard = radar_vital_presence_guard;
        }
        if (!s_radar_receiver_started &&
            now_us - s_last_radar_start_attempt_us >= 5000000LL) {
            (void)start_radar_receiver();
        }
        maybe_print_radar_status(&radar, now_us);
        update_wifi_feedback(now_us);
        maybe_print_feishu_status(now_us);
        sleep_score_from_radar(&radar, &features, &assessment);
        features.motion_energy = ld6002_phase_motion_index(&radar, now_us);
        float radar_motion_index = features.motion_energy;

        struct tm local_time = {0};
        bool local_time_valid = time_sync_get_local_time(&local_time, NULL);
        if (local_time_valid) {
            voice_control_set_clock((uint8_t)local_time.tm_hour, (uint8_t)local_time.tm_min);
        }
        bool breath_fresh = ld6002_field_fresh(now_us, radar.breath_update_us,
                                                DG_RADAR_VITAL_MAX_AGE_US);
        bool heart_fresh = ld6002_field_fresh(now_us, radar.heart_update_us,
                                               DG_RADAR_VITAL_MAX_AGE_US);
        SleepBioRaw bio_raw = {
            .presence = radar.human_present,
            .breath_bpm_raw = breath_fresh ? radar.breath_rate_bpm : NAN,
            .heart_bpm_raw = heart_fresh ? radar.heart_rate_bpm : NAN,
            .breath_update_us = breath_fresh ? radar.breath_update_us : 0,
            .heart_update_us = heart_fresh ? radar.heart_update_us : 0,
            .motion_raw = features.motion_energy,
            .confidence = ld6002_bio_confidence(&radar, now_us, breath_fresh, heart_fresh),
        };
        SleepBioState bio_state = {0};
        bio_filter_update(&bio_raw, now_us, &bio_state);

        intervention_decision_t decision = {0};
        strcpy(decision.skill_name, "idle.clock");
        strcpy(decision.reason, "standby clock mode; automatic audio intervention disabled");
        bool sleep_stop_requested = false;

        mobile_app_command_t immediate_command = take_immediate_command();
        bool skip_voice_control_tick = false;
        if (immediate_command == MOBILE_APP_COMMAND_SLEEP) {
            start_sleep_mode_now(now_us, "immediate", true);
            skip_voice_control_tick = true;
        } else if (immediate_command == MOBILE_APP_COMMAND_SLEPT) {
            start_slept_mode_now(now_us, "immediate");
            skip_voice_control_tick = true;
        } else if (immediate_command == MOBILE_APP_COMMAND_STOP) {
            (void)voice_control_handle_phrase("stop");
            printf("DG Sleep: immediate stop\r\n");
            stop_sleep_mode(now_us);
            sleep_stop_requested = true;
            skip_voice_control_tick = true;
            memset(&decision, 0, sizeof(decision));
            snprintf(decision.skill_name, sizeof(decision.skill_name), "%s", "sleep.stop.immediate");
            snprintf(decision.reason, sizeof(decision.reason), "%s", "immediate command cleared sleep outputs");
        }

        if (voice_control_policy_suppressed(now_us)) {
            strcpy(decision.skill_name, "voice.stop_hold");
            strcpy(decision.reason, "manual stop hold suppressing automatic intervention");
        }

        intervention_decision_t command_decision = {0};
        if (!skip_voice_control_tick && voice_control_tick(&radar, now_us, &command_decision)) {
            decision = command_decision;
        }

        sleep_assist_status_t voice_assist_status = {0};
        sleep_assist_get_status(&voice_assist_status);
        bool sleep_mode_active_now = voice_assist_status.active;
        stereo_audio_status_t snore_playback_status = {0};
        stereo_audio_get_status(&snore_playback_status);
        if (sleep_mode_active_now && voice_assist_status.sleep_locked &&
            !mid_sleep_intervention_active(now_us) &&
            snore_playback_status.initialized &&
            snore_playback_status.mode != 0 &&
            snore_playback_status.volume_percent >
                DG_SNORE_PLAYBACK_GATE_VOLUME_PERCENT) {
            /* The loud scene belongs to the pre-sleep ramp. Once sleep is
             * locked, return to the intended quiet baseline. Low-volume
             * interventions started afterwards must keep ownership of the
             * speaker for their full duration. */
            esp_err_t mute_err = stereo_audio_mute();
            printf("DG Snore: deep lock muted loud scene volume=%u err=%s\r\n",
                   (unsigned)snore_playback_status.volume_percent,
                   esp_err_to_name(mute_err));
            stereo_audio_get_status(&snore_playback_status);
        }
        bool snore_playback_active = snore_playback_status.initialized &&
                                     snore_playback_status.mode != 0 &&
                                     snore_playback_status.volume_percent >
                                         DG_SNORE_PLAYBACK_GATE_VOLUME_PERCENT;
        audio_monitor_shared_stream_set_state(
            sleep_mode_active_now && voice_assist_status.sleep_locked,
            snore_playback_active);
        audio_monitor_shared_stream_set_sleep_context(
            radar.human_present,
            breath_fresh,
            breath_fresh ? radar.breath_rate_bpm : 0.0f);
        update_sleep_voice_assistant_mode(sleep_mode_active_now, now_us);
        if (s_native_sr_running && voice_sr_take_wake_request()) {
            if (sleep_mode_active_now) {
                ESP_LOGI(TAG, "wake request ignored during sleep mode");
                printf("DG Voice: wake ignored during sleep mode\r\n");
                voice_session_stop(now_us, "sleep_mode");
            } else if (s_voice_session_active) {
                printf("DG Voice: wake ignored; session already active\r\n");
                voice_sr_status_t active_sr_status = {0};
                if (s_native_sr_running) {
                    voice_sr_get_status(&active_sr_status);
                }
                bool active_scheduled = s_online_asr_start_us > 0 && s_online_asr_start_us > now_us;
                if (online_asr_is_busy() || active_scheduled || !active_sr_status.command_audio_active) {
                    show_voice_waiting_visual(now_us);
                } else {
                    show_voice_asr_ready_visual(now_us);
                }
            } else if (online_asr_is_busy()) {
                printf("DG Voice: wake ignored; ASR busy\r\n");
                voice_session_stop(now_us, "busy_wake");
                show_voice_error_visual(now_us, "BUSY");
                voice_sr_pause(900);
            } else {
                printf("DG Voice: wake accepted; waiting for command\r\n");
                (void)voice_control_handle_phrase("hi");
                voice_session_mark_activity(now_us);
                if (online_asr_is_ready()) {
                    int64_t first_delay_us = ((int64_t)DG_WAKE_ACK_SR_PAUSE_MS * 1000LL) +
                                             DG_ONLINE_ASR_START_GRACE_US;
                    voice_session_schedule_asr(now_us, first_delay_us, "wake_ack");
                    printf("DG ASR: scheduled after wake ack in %u ms\r\n",
                           (unsigned)(DG_WAKE_ACK_SR_PAUSE_MS + (DG_ONLINE_ASR_START_GRACE_US / 1000LL)));
                } else {
                    s_online_asr_start_us = 0;
                    printf("DG ASR: not configured; using native command recognizer only\r\n");
                }
                play_wake_ack(now_us);
            }
        }
        voice_sr_command_t sr_command = VOICE_SR_COMMAND_NONE;
        if (s_native_sr_running && voice_sr_take_command(&sr_command)) {
            printf("DG Voice: native sr command=%d sleep_active=%d\r\n",
                   (int)sr_command, sleep_mode_active_now ? 1 : 0);
            /* The local result wins the race with the delayed cloud capture.
             * Drop any in-progress PCM so a completed one-shot command cannot
             * leave ASR busy or reopen the WAITING screen. */
            voice_sr_command_audio_cancel();
            voice_sr_pause(900);
            if (sleep_mode_active_now) {
                ESP_LOGI(TAG, "voice command ignored during sleep mode command=%d", (int)sr_command);
                printf("DG Voice: command ignored during sleep mode command=%d\r\n", (int)sr_command);
                voice_session_stop(now_us, "sleep_mode_command");
            } else if (sr_command == VOICE_SR_COMMAND_SLEEP) {
                voice_session_stop(now_us, "native_sleep");
                hold_manual_screen_for(0x005A28, now_us, 2, "SLEEP");
                (void)voice_control_handle_phrase("sleep");
            } else if (sr_command == VOICE_SR_COMMAND_STOP) {
                voice_session_stop(now_us, "native_stop");
                hold_manual_screen_for(0x8A0018, now_us, 2, "STOP");
                (void)voice_control_handle_phrase("stop");
            } else if (sr_command == VOICE_SR_COMMAND_LIGHT_ON) {
                voice_session_stop(now_us, "native_light_on");
                hold_manual_screen_for(0x006020, now_us, 2, "LIGHT ON");
                (void)mobile_command_cb(MOBILE_APP_COMMAND_LIGHT_YELLOW, NULL);
            } else if (sr_command == VOICE_SR_COMMAND_LIGHT_OFF) {
                voice_session_stop(now_us, "native_light_off");
                hold_manual_screen_for(0x402000, now_us, 2, "LIGHT OFF");
                (void)mobile_command_cb(MOBILE_APP_COMMAND_LIGHT_OFF, NULL);
            } else if (sr_command == VOICE_SR_COMMAND_LIGHT_BRIGHTER) {
                voice_session_stop(now_us, "native_light_brighter");
                hold_manual_screen_for(0x806000, now_us, 2, "BRIGHTER");
                (void)mobile_command_cb(MOBILE_APP_COMMAND_LIGHT_BRIGHTER, NULL);
            } else if (sr_command == VOICE_SR_COMMAND_LIGHT_DIMMER) {
                voice_session_stop(now_us, "native_light_dimmer");
                hold_manual_screen_for(0x202000, now_us, 2, "DIMMER");
                (void)mobile_command_cb(MOBILE_APP_COMMAND_LIGHT_DIMMER, NULL);
            }
        }

        if (!sleep_mode_active_now && s_online_asr_start_us > 0 && now_us >= s_online_asr_start_us) {
            s_online_asr_start_us = 0;
            esp_err_t asr_cap_err = online_asr_is_busy()
                ? ESP_ERR_INVALID_STATE
                : voice_sr_command_audio_start(DG_ONLINE_ASR_COMMAND_SEC);
            printf("DG ASR: capture start err=%s\r\n", esp_err_to_name(asr_cap_err));
            if (asr_cap_err != ESP_OK) {
                show_voice_error_visual(now_us, online_asr_is_busy() ? "BUSY" : "LISTEN ERR");
            } else {
                show_voice_asr_ready_visual(now_us);
            }
        }

        int16_t *asr_samples = NULL;
        uint32_t asr_sample_count = 0;
        if (s_native_sr_running && voice_sr_take_command_audio(&asr_samples, &asr_sample_count)) {
            if (sleep_mode_active_now) {
                free(asr_samples);
                asr_samples = NULL;
                asr_sample_count = 0;
                printf("DG Voice: captured speech discarded during sleep mode\r\n");
            } else {
            char label[32];
            snprintf(label, sizeof(label), "cmd_%lu", (unsigned long)++s_online_asr_sequence);
            esp_err_t submit_err = ESP_OK;
            asr_gate_result_t gate = {0};
            if (online_asr_is_busy()) {
                free(asr_samples);
                asr_samples = NULL;
                submit_err = ESP_ERR_INVALID_STATE;
            } else if (!prepare_asr_audio_for_upload(asr_samples, &asr_sample_count, &gate)) {
                printf("DG ASR: local reject label=%s samples=%u rms=%.1f noise=%.1f thr=%.1f peak=%d active_ms=%u\r\n",
                       label,
                       (unsigned)gate.original_samples,
                       (double)gate.rms,
                       (double)gate.noise_rms,
                       (double)gate.threshold_rms,
                       gate.peak,
                       (unsigned)gate.active_ms);
                free(asr_samples);
                asr_samples = NULL;
                submit_err = ESP_ERR_INVALID_SIZE;
                if (s_voice_session_active &&
                    s_voice_session_idle_until_us > 0 &&
                    now_us < s_voice_session_idle_until_us) {
                    voice_session_schedule_asr(now_us, DG_ONLINE_ASR_REJECT_GAP_US, "after_local_reject");
                    show_voice_waiting_visual(now_us);
                } else {
                    voice_session_stop(now_us, "idle_timeout");
                }
            } else {
                printf("DG ASR: local accept label=%s original=%u trimmed=%u rms=%.1f noise=%.1f thr=%.1f peak=%d active_ms=%u\r\n",
                       label,
                       (unsigned)gate.original_samples,
                       (unsigned)gate.trimmed_samples,
                       (double)gate.rms,
                       (double)gate.noise_rms,
                       (double)gate.threshold_rms,
                       gate.peak,
                       (unsigned)gate.active_ms);
                voice_session_mark_activity(now_us);
                submit_err = online_asr_submit_pcm16_take(asr_samples, asr_sample_count, label);
                asr_samples = NULL;
                if (submit_err == ESP_OK) {
                    voice_session_extend_for_asr_busy(now_us);
                }
            }
            printf("DG ASR: submit label=%s samples=%u err=%s\r\n",
                   label, (unsigned)asr_sample_count, esp_err_to_name(submit_err));
            if (submit_err != ESP_OK) {
                if (submit_err == ESP_ERR_INVALID_SIZE) {
                    show_voice_asr_ready_visual(now_us);
                } else {
                    show_voice_error_visual(now_us, online_asr_is_busy() ? "BUSY" : "ASR ERR");
                }
            } else {
                show_voice_upload_visual(now_us);
            }
            }
        }

        char asr_text[ONLINE_ASR_TEXT_MAX] = {0};
        if (online_asr_take_transcript(asr_text, sizeof(asr_text))) {
            if (sleep_mode_active_now) {
                printf("DG Voice: transcript discarded during sleep mode\r\n");
            } else {
            printf("DG Voice: online transcript input=\"%s\"\r\n", asr_text);
            show_voice_waiting_visual(now_us);
            if (voice_control_handle_phrase(asr_text)) {
                voice_control_status_t handled_status = {0};
                voice_control_get_status(&handled_status);
                if (handled_status.last_command == VOICE_COMMAND_STOP) {
                    printf("DG Voice: online exit command accepted\r\n");
                    voice_session_stop(now_us, "voice_exit");
                    show_voice_received_visual(now_us, "OK");
                } else if (handled_status.last_command == VOICE_COMMAND_DEVICE_ACTION) {
                    printf("DG Voice: one-shot device action accepted; ending session\r\n");
                    voice_session_stop(now_us, "device_action_done");
                    show_voice_received_visual(now_us, "OK");
                } else {
                    voice_session_mark_activity(now_us);
                    voice_session_schedule_asr(now_us, DG_ONLINE_ASR_NEXT_GAP_US, "after_ok");
                }
            } else {
                esp_err_t chat_err = online_chat_submit_text(asr_text);
                if (chat_err == ESP_OK) {
                    printf("DG Chat: submitted text=\"%s\"\r\n", asr_text);
                    voice_session_extend_for_asr_busy(now_us);
                    show_voice_waiting_visual(now_us);
                } else {
                    printf("DG Voice: online transcript unhandled chat_err=%s\r\n",
                           esp_err_to_name(chat_err));
                    if (s_voice_session_active &&
                        s_voice_session_idle_until_us > 0 &&
                        now_us < s_voice_session_idle_until_us) {
                        voice_session_schedule_asr(now_us, DG_ONLINE_ASR_NEXT_GAP_US, "after_noise");
                    } else {
                        voice_session_stop(now_us, "idle_timeout");
                    }
                }
            }
            }
        }

        online_chat_result_t chat_result = {0};
        if (online_chat_take_result(&chat_result)) {
            if (sleep_mode_active_now) {
                printf("DG Chat: result discarded during sleep mode\r\n");
            } else if (!s_voice_session_active) {
                printf("DG Chat: late result discarded after voice session exit\r\n");
            } else if (chat_result.error == ESP_OK && chat_result.audio_path[0]) {
                uint32_t duration_sec = (chat_result.duration_ms + 999U) / 1000U;
                if (duration_sec < 2U) {
                    duration_sec = 2U;
                }
                if (duration_sec > DG_VOICE_CHAT_MAX_AUDIO_SEC) {
                    duration_sec = DG_VOICE_CHAT_MAX_AUDIO_SEC;
                }
                printf("DG Chat: playback path=%s duration=%us text=\"%s\"\r\n",
                       chat_result.audio_path, (unsigned)duration_sec, chat_result.text);
                show_voice_waiting_visual(now_us);
                esp_err_t ready_err = ensure_audio_ready();
                esp_err_t play_err = ready_err == ESP_OK
                    ? stereo_audio_play_wav(chat_result.audio_path,
                                            adapt_audio_volume_floor(90, 60),
                                            (uint16_t)duration_sec)
                    : ready_err;
                if (play_err == ESP_OK) {
                    hold_manual_audio(now_us, (uint16_t)duration_sec);
                    voice_sr_pause(duration_sec * 1000U);
                    voice_session_mark_activity(now_us);
                    voice_session_schedule_asr(now_us,
                                               ((int64_t)duration_sec * 1000000LL) +
                                               DG_ONLINE_ASR_NEXT_GAP_US,
                                               "after_chat");
                } else {
                    printf("DG Chat: playback failed err=%s\r\n", esp_err_to_name(play_err));
                    show_voice_error_visual(now_us, "CHAT ERR");
                    voice_session_schedule_asr(now_us, DG_ONLINE_ASR_NEXT_GAP_US, "after_chat_error");
                }
            } else {
                printf("DG Chat: failed err=%s http=%d result=%s text=\"%s\"\r\n",
                       esp_err_to_name(chat_result.error), chat_result.http_status,
                       chat_result.result, chat_result.text);
                show_voice_error_visual(now_us, "CHAT ERR");
                if (s_voice_session_active &&
                    s_voice_session_idle_until_us > 0 &&
                    now_us < s_voice_session_idle_until_us) {
                    voice_session_schedule_asr(now_us, DG_ONLINE_ASR_NEXT_GAP_US, "after_chat_fail");
                }
            }
        }

        online_chat_status_t chat_status = {0};
        online_chat_get_status(&chat_status);
        if (!sleep_mode_active_now && chat_status.failed_count != s_online_chat_seen_failed_count) {
            s_online_chat_seen_failed_count = chat_status.failed_count;
            printf("DG Chat: screen error fail=%u err=%s http=%d result=%s\r\n",
                   (unsigned)chat_status.failed_count,
                   esp_err_to_name(chat_status.last_error),
                   chat_status.last_http_status,
                   chat_status.last_result);
        }
        online_asr_status_t asr_status = {0};
        online_asr_get_status(&asr_status);
        if (!sleep_mode_active_now && asr_status.failed_count != s_online_asr_seen_failed_count) {
            s_online_asr_seen_failed_count = asr_status.failed_count;
            const char *asr_label = online_asr_screen_error_label(&asr_status);
            printf("DG ASR: screen error label=%s fail=%u err=%s http=%d result=%s\r\n",
                   asr_label,
                   (unsigned)asr_status.failed_count,
                   esp_err_to_name(asr_status.last_error),
                   asr_status.last_http_status,
                   asr_status.last_result);
            if (s_voice_session_active && strcmp(asr_status.last_result, "asr_empty") == 0) {
                if (s_voice_session_idle_until_us > 0 && now_us < s_voice_session_idle_until_us) {
                    voice_session_schedule_asr(now_us, DG_ONLINE_ASR_NEXT_GAP_US, "after_empty");
                } else {
                    voice_session_stop(now_us, "idle_timeout");
                }
            } else {
                show_voice_error_visual(now_us, asr_label);
            }
        }

        if (s_voice_command_wait_until_us > 0 && now_us >= s_voice_command_wait_until_us) {
            (void)voice_session_stop_if_idle(now_us);
        }

        voice_control_status_t voice_status = {0};
        voice_control_get_status(&voice_status);
        bool voice_command_changed = voice_status.command_count != last_voice_command_count;
        if (skip_voice_control_tick && immediate_command != MOBILE_APP_COMMAND_NONE) {
            last_voice_command_count = voice_status.command_count;
            voice_command_changed = false;
        }
        if (voice_command_changed) {
            last_voice_command_count = voice_status.command_count;
            if (voice_status.last_command == VOICE_COMMAND_SLEEP) {
                start_sleep_mode_now(now_us, "voice", false);
            } else if (voice_status.last_command == VOICE_COMMAND_STOP ||
                       voice_status.last_command == VOICE_COMMAND_WAKE_UP) {
                stop_sleep_mode(now_us);
                sleep_stop_requested = true;
            } else if (voice_status.last_command == VOICE_COMMAND_STORY) {
                /* The console/voice parser historically changed only the
                 * visible mode for STORY. Route it through the same bounded
                 * device-command queue used by web and Feishu controls. */
                (void)mobile_command_cb(MOBILE_APP_COMMAND_STORY, NULL);
            }
        }

        mobile_app_command_t mobile_command = take_mobile_command();
        if (mobile_command != MOBILE_APP_COMMAND_NONE) {
            printf("DG Command: execute %s(%d)\r\n",
                   mobile_command_name(mobile_command), (int)mobile_command);
        }
        if (sleep_assist_is_active() &&
            mobile_command != MOBILE_APP_COMMAND_NONE &&
            (mobile_command == MOBILE_APP_COMMAND_SLEEP ||
             mobile_command == MOBILE_APP_COMMAND_SLEPT)) {
            printf("DG Sleep: ignoring duplicate command %s(%d) while active\r\n",
                   mobile_command_name(mobile_command), (int)mobile_command);
            mobile_command = MOBILE_APP_COMMAND_NONE;
        }
        if (mobile_command == MOBILE_APP_COMMAND_SLEEP) {
            start_sleep_mode_now(now_us, "queued", true);
        } else if (mobile_command == MOBILE_APP_COMMAND_SLEPT) {
            start_slept_mode_now(now_us, "queued");
        } else if (mobile_command == MOBILE_APP_COMMAND_STOP) {
            (void)voice_control_handle_phrase("stop");
            stop_sleep_mode(now_us);
            sleep_stop_requested = true;
            memset(&decision, 0, sizeof(decision));
            snprintf(decision.skill_name, sizeof(decision.skill_name), "%s", "sleep.stop");
            snprintf(decision.reason, sizeof(decision.reason), "%s", "manual stop cleared sleep outputs");
        } else if (mobile_command == MOBILE_APP_COMMAND_LIGHT_RED) {
            hold_manual_light(255, 0, 0, DG_MANUAL_LIGHT_BRIGHTNESS, now_us);
        } else if (mobile_command == MOBILE_APP_COMMAND_LIGHT_GREEN) {
            hold_manual_light(0, 255, 0, DG_MANUAL_LIGHT_BRIGHTNESS, now_us);
        } else if (mobile_command == MOBILE_APP_COMMAND_LIGHT_BLUE) {
            hold_manual_light(0, 0, 255, DG_MANUAL_LIGHT_BRIGHTNESS, now_us);
        } else if (mobile_command == MOBILE_APP_COMMAND_LIGHT_YELLOW) {
            hold_manual_light(255, 180, 0, DG_MANUAL_LIGHT_BRIGHTNESS, now_us);
        } else if (mobile_command == MOBILE_APP_COMMAND_LIGHT_OFF) {
            clear_manual_light();
        } else if (mobile_command == MOBILE_APP_COMMAND_LIGHT_BRIGHTER) {
            if (s_manual_light_until_us <= now_us) {
                s_manual_light_red = 255;
                s_manual_light_green = 180;
                s_manual_light_blue = 0;
                s_manual_light_brightness = DG_MANUAL_LIGHT_BRIGHTNESS;
            }
            hold_manual_light(s_manual_light_red, s_manual_light_green, s_manual_light_blue,
                              clampf_app(s_manual_light_brightness + DG_MANUAL_LIGHT_BRIGHTNESS_STEP,
                                         DG_MANUAL_LIGHT_BRIGHTNESS_MIN, 1.0f), now_us);
        } else if (mobile_command == MOBILE_APP_COMMAND_LIGHT_DIMMER) {
            if (s_manual_light_until_us <= now_us) {
                s_manual_light_red = 255;
                s_manual_light_green = 180;
                s_manual_light_blue = 0;
                s_manual_light_brightness = DG_MANUAL_LIGHT_BRIGHTNESS;
            }
            hold_manual_light(s_manual_light_red, s_manual_light_green, s_manual_light_blue,
                              clampf_app(s_manual_light_brightness - DG_MANUAL_LIGHT_BRIGHTNESS_STEP,
                                         DG_MANUAL_LIGHT_BRIGHTNESS_MIN, 1.0f), now_us);
        } else if (mobile_command == MOBILE_APP_COMMAND_SCREEN_RED) {
            hold_manual_screen(0xFF0000, now_us);
        } else if (mobile_command == MOBILE_APP_COMMAND_SCREEN_GREEN) {
            hold_manual_screen(0x00A000, now_us);
        } else if (mobile_command == MOBILE_APP_COMMAND_SCREEN_BLUE) {
            hold_manual_screen(0x0000FF, now_us);
        } else if (mobile_command == MOBILE_APP_COMMAND_SCREEN_YELLOW) {
            hold_manual_screen(0xFFC000, now_us);
        } else if (mobile_command == MOBILE_APP_COMMAND_SCREEN_OFF) {
            clear_manual_screen();
        } else if (mobile_command == MOBILE_APP_COMMAND_AUDIO_MUSIC) {
            if (play_current_sleep_scene(180)) {
                hold_manual_audio(now_us, 180);
            }
        } else if (mobile_command == MOBILE_APP_COMMAND_AUDIO_NOISE) {
            if (play_wav_or_fallback("/spiffs/noise.wav", 18, 300, MOBILE_APP_COMMAND_AUDIO_NOISE)) {
                hold_manual_audio(now_us, 300);
            }
        } else if (mobile_command == MOBILE_APP_COMMAND_AUDIO_BREATHING) {
            if (play_current_sleep_scene(240)) {
                hold_manual_audio(now_us, 240);
            }
        } else if (mobile_command == MOBILE_APP_COMMAND_AUDIO_VOLUME_MAX) {
            (void)set_user_audio_volume(100);
        } else if (mobile_command == MOBILE_APP_COMMAND_AUDIO_VOLUME_UP) {
            (void)change_user_audio_volume(15);
        } else if (mobile_command == MOBILE_APP_COMMAND_AUDIO_VOLUME_DOWN) {
            (void)change_user_audio_volume(-15);
        } else if (mobile_command == MOBILE_APP_COMMAND_AUDIO_MUTE) {
            s_user_audio_volume_percent = 0;
            if (ensure_audio_ready() == ESP_OK) {
                (void)stereo_audio_mute();
            }
        } else if (mobile_command == MOBILE_APP_COMMAND_STORY) {
            if (play_current_sleep_scene(180)) {
                hold_manual_audio(now_us, 180);
            }
        } else if (mobile_command == MOBILE_APP_COMMAND_WAKE_ACK) {
            if (sleep_assist_is_active()) {
                ESP_LOGI(TAG, "wake ack command ignored during sleep mode");
                printf("DG Voice: wake ack ignored during sleep mode\r\n");
            } else {
                (void)voice_control_handle_phrase("hi");
                play_wake_ack(now_us);
            }
        } else if (scene_for_command(mobile_command)) {
            const char *scene = scene_for_command(mobile_command);
            if (save_scene_preference(scene) == ESP_OK) {
                clear_manual_screen();
                ESP_LOGI(TAG, "scene preference=%s", scene);
                if (scene_play_command(mobile_command) &&
                    play_current_sleep_scene(DG_SCENE_PREVIEW_DURATION_SEC)) {
                    hold_manual_audio(now_us, DG_SCENE_PREVIEW_DURATION_SEC);
                }
            }
        } else if (breath_for_command(mobile_command)) {
            const char *mode = breath_for_command(mobile_command);
            if (save_breath_preference(mode) == ESP_OK) {
                ESP_LOGI(TAG, "breath preference=%s", mode);
            }
        } else if (mobile_command == MOBILE_APP_COMMAND_MID_SLEEP_TEST_MINOR ||
                   mobile_command == MOBILE_APP_COMMAND_MID_SLEEP_TEST_RESTLESS ||
                   mobile_command == MOBILE_APP_COMMAND_MID_SLEEP_TEST_AROUSAL ||
                   mobile_command == MOBILE_APP_COMMAND_MID_SLEEP_TEST_OUT_OF_BED) {
            intervention_decision_t test_decision = {0};
            build_mid_sleep_test_decision(mobile_command, &test_decision);
            if (execute_mid_sleep_intervention(&test_decision, &bio_state,
                                               &assessment, now_us, true)) {
                if (mobile_command == MOBILE_APP_COMMAND_MID_SLEEP_TEST_OUT_OF_BED) {
                    s_out_of_bed_demo_return_allowed = true;
                    s_returned_to_bed_since_us = 0;
                }
                decision = test_decision;
            }
        }

        if (voice_control_take_self_test_request() || mobile_command == MOBILE_APP_COMMAND_SELF_TEST) {
            clear_manual_light();
            clear_manual_screen();
            run_hardware_self_test();
            stereo_audio_mute();
            status_light_sleep_assist_set(0, 0, 0, 0.0f);
        }

        bool assist_user_interaction =
            (voice_command_changed && voice_status.last_command != VOICE_COMMAND_SLEEP) ||
            (mobile_command != MOBILE_APP_COMMAND_NONE && mobile_command != MOBILE_APP_COMMAND_SLEEP);
        sleep_ui_set_sleep_mode_active(sleep_assist_is_active());
        if (sleep_assist_is_active()) {
            sleep_assist_tick(&bio_state, assist_user_interaction, now_us);
            apply_mid_sleep_overlay(now_us);
            sleep_assist_status_t assist_status = {0};
            sleep_assist_get_status(&assist_status);
            memset(&decision, 0, sizeof(decision));
            snprintf(decision.skill_name, sizeof(decision.skill_name),
                     "sleep_assist.%s", sleep_assist_state_name(assist_status.state));
            snprintf(decision.reason, sizeof(decision.reason),
                     "filtered bio valid=%d target=%.1f stability=%.2f",
                     bio_state.valid,
                     (double)assist_status.target_breath_bpm,
                     (double)bio_state.stability_score);
            if (assist_status.sleep_locked && !mid_sleep_intervention_active(now_us) &&
                !s_manual_screen_active) {
                (void)status_light_force_off();
                (void)sleep_ui_show_sleep_locked_clock();
            }
        } else {
            if (s_manual_audio_until_us <= now_us) {
                if (s_manual_audio_until_us > 0) {
                    clear_manual_audio();
                } else {
                    stereo_audio_mute();
                }
            }
            if (s_manual_light_until_us > now_us) {
                status_light_sleep_assist_set(s_manual_light_red,
                                              s_manual_light_green,
                                              s_manual_light_blue,
                                              s_manual_light_brightness);
            } else if (!mid_sleep_intervention_active(now_us)) {
                status_light_sleep_assist_set(0, 0, 0, 0.0f);
            }
        }
        if (!sleep_assist_is_active()) {
            apply_mid_sleep_overlay(now_us);
        }

        if (s_force_outputs_off_until_us > now_us) {
            (void)stereo_audio_mute();
            (void)status_light_sleep_assist_set_local(0, 0, 0, 0.0f);
        }

        if (s_manual_screen_active && s_manual_screen_until_us > now_us) {
            (void)sleep_ui_show_solid_color(s_manual_screen_rgb,
                                            s_manual_screen_brightness_percent,
                                            s_manual_screen_label);
        } else if (s_manual_screen_active) {
            clear_manual_screen();
        }

        if (!sleep_assist_is_active() && !s_voice_session_active &&
            !s_manual_screen_active && !mid_sleep_intervention_active(now_us)) {
            sleep_ui_show_clock_mode();
        }
        csi_monitor_status_t csi_status = {0};
        csi_monitor_get_status(&csi_status);
        if (!s_voice_session_active) {
            sleep_ui_update_sensor_status(&radar, &csi_status);
        }
        voice_session_refresh_screen(now_us);
        sleep_assist_status_t latest_assist_status = {0};
        sleep_assist_get_status(&latest_assist_status);
        sleep_score_from_bio(&bio_state, &latest_assist_status, &features, &assessment);
        if (s_slept_test_mode && latest_assist_status.active && latest_assist_status.sleep_locked) {
            if (!bio_state.presence) {
                assessment.state = SLEEP_STATE_OUT_OF_BED;
                assessment.sleep_score = 0;
                assessment.wake_risk = 95;
                assessment.stable_sleep_index = 0;
                snprintf(assessment.reason, sizeof(assessment.reason), "%s",
                         "demo sleep lock: no presence, out of bed");
            } else {
                /* The person in a live demonstration is only pretending to
                 * sleep, so awake heart/breath trends must not be interpreted
                 * as arousal.  Presence establishes a quiet deep-sleep demo
                 * baseline; apply_sustained_turning_risk() remains responsible
                 * for raising risk from actual repeated turning afterwards. */
                features.data_quality_ok = true;
                assessment.state = SLEEP_STATE_DEEP_TREND;
                assessment.sleep_score = 90;
                assessment.wake_risk = 20;
                assessment.stable_sleep_index = 80;
                snprintf(assessment.reason, sizeof(assessment.reason), "%s",
                         "demo sleep lock: simulated stable deep baseline; turning enabled");
            }
        }
        apply_sustained_turning_risk(radar_motion_index,
                                     radar.phase_update_us,
                                     radar.human_present,
                                     &latest_assist_status,
                                     &assessment,
                                     now_us);
        if (sleep_log_session_is_active()) {
            maybe_send_sleep_start_report(now_us, false);
            if (latest_assist_status.sleep_locked) {
                maybe_send_sleep_onset_report(&bio_state, &assessment, now_us);
            }
            maybe_send_snore_event_report(now_us);
        }
        intervention_decision_t mid_auto_decision = {0};
        if (!sleep_stop_requested &&
            maybe_start_out_of_bed_path(&radar,
                                        &bio_state, &assessment, &latest_assist_status,
                                        &mid_auto_decision, now_us)) {
            decision = mid_auto_decision;
        } else if (!sleep_stop_requested &&
                   maybe_restart_assist_after_return(&bio_state, &assessment,
                                                     &mid_auto_decision, now_us)) {
            decision = mid_auto_decision;
        } else if (!sleep_stop_requested &&
            maybe_auto_mid_sleep_intervention(&features, &bio_state,
                                              &assessment, &latest_assist_status,
                                              &mid_auto_decision, now_us)) {
            decision = mid_auto_decision;
        } else if (mid_sleep_intervention_active(now_us) && s_mid_sleep_action != INTERVENTION_ACTION_NONE) {
            memset(&decision, 0, sizeof(decision));
            decision.action = s_mid_sleep_action;
            decision.brightness_percent = s_mid_sleep_brightness_percent;
            snprintf(decision.skill_name, sizeof(decision.skill_name), "mid_sleep.%s",
                     intervention_action_to_name(s_mid_sleep_action));
            snprintf(decision.reason, sizeof(decision.reason), "%s", s_mid_sleep_reason);
        } else if (s_mid_sleep_intervention_until_us > 0 && !mid_sleep_intervention_active(now_us)) {
            clear_mid_sleep_intervention();
        }
        sleep_assist_get_status(&latest_assist_status);

        if (sleep_log_session_is_active()) {
            sleep_log_session_update(&bio_state, &assessment, &latest_assist_status, &decision, now_us);
        }

        const char *auto_stop_reason = NULL;
        if (!sleep_stop_requested &&
            maybe_auto_finish_sleep_session(&bio_state, &assessment, &local_time,
                                            local_time_valid, now_us, &auto_stop_reason)) {
            memset(&decision, 0, sizeof(decision));
            snprintf(decision.skill_name, sizeof(decision.skill_name), "%s", "sleep.stop.auto_morning");
            snprintf(decision.reason, sizeof(decision.reason), "%s",
                     auto_stop_reason ? auto_stop_reason : "auto morning wake");
            stop_sleep_mode_with_reason(now_us, auto_stop_reason ? auto_stop_reason : "auto morning wake");
            sleep_stop_requested = true;
            sleep_assist_get_status(&latest_assist_status);
        }
        maybe_send_periodic_sleep_report(&bio_state, &assessment,
                                         &latest_assist_status, &decision, now_us);
        mobile_app_telemetry_t telemetry = {
            .radar = radar,
            .features = features,
            .assessment = assessment,
            .bio = bio_state,
            .assist = latest_assist_status,
            .csi = csi_status,
            .decision = decision,
            .uptime_sec = (uint32_t)(now_us / 1000000LL),
        };
        mobile_app_update(&telemetry);

        ai_bridge_sample_t ai_sample = {
            .presence = radar.human_present,
            .bio_valid = bio_state.valid,
            .breath_bpm = bio_state.breath_bpm_smooth,
            .heart_bpm = bio_state.heart_bpm_smooth,
            .motion = bio_state.motion_smooth,
            .stability = bio_state.stability_score,
            .sleep_score = assessment.sleep_score,
            .wake_risk = assessment.wake_risk,
            .sleep_state = assessment.state,
            .assist_state = latest_assist_status.state,
            .sleep_locked = latest_assist_status.sleep_locked,
            .action = decision.skill_name,
            .uptime_sec = telemetry.uptime_sec,
        };
        ai_bridge_update(&ai_sample, now_us);

        sleep_log_append_summary(&radar, &features, &assessment, &decision);

        vTaskDelay(pdMS_TO_TICKS(DG_APP_LOOP_PERIOD_MS));
    }
}
