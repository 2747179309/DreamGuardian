#include "voice_sr.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../audio_monitor/include/audio_monitor.h"
#include "bsp/esp-bsp.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_timer.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "dg_debug_control.h"
#include "model_path.h"
#include "nvs.h"
#include "sdkconfig.h"

#define VOICE_SR_SAMPLE_RATE 16000U
#define VOICE_SR_BITS 16U
#define VOICE_SR_MIC_CHANNELS 2U
#define VOICE_SR_FEED_CHANNELS 3U
#define VOICE_SR_MIC_GAIN_DB 36.0f
#define VOICE_SR_FEED_STACK 4096
#define VOICE_SR_DETECT_STACK 12288
#define VOICE_SR_TASK_PRIORITY 3
#define VOICE_SR_WAKE_TEMPLATE_MAX 64
#define VOICE_SR_WAKE_TEMPLATE_BINS 32
#define VOICE_SR_WAKE_RING_SAMPLES (VOICE_SR_SAMPLE_RATE * 4)
#define VOICE_SR_WAKE_TEMPLATE_THRESHOLD 0.54f
#define VOICE_SR_WAKE_TEMPLATE_CONFIRM_THRESHOLD 0.52f
#define VOICE_SR_WAKE_TEMPLATE_MIN_CONFIRMATIONS 2
#define VOICE_SR_WAKE_TEMPLATE_ZCR_WEIGHT 0.35f
// Keep the experimental "Hi Xiaomeng" template detector opt-in.  The loose
// template matcher can false-trigger on room noise and must not run alongside
// the production WakeNet model unless it is explicitly enabled for testing.
#define VOICE_SR_WAKE_TEMPLATE_AUTO_ENABLE 0
#define VOICE_SR_WAKE_TEMPLATE_ONLY_WHEN_READY 1
#define VOICE_SR_WAKE_TEMPLATE_FILE "/spiffs/wake_xm.tmpl"
#define VOICE_SR_WAKE_TEMPLATE_LIVE_FILE "/spiffs/wake_live.tmpl"
#define VOICE_SR_WAKE_TEMPLATE_FILE_MAGIC "DGWT"
#define VOICE_SR_WAKE_TEMPLATE_FILE_VERSION 1
#define VOICE_SR_WAKE_TEMPLATE_MIN_SAMPLES ((VOICE_SR_SAMPLE_RATE * 35U) / 100U)
#define VOICE_SR_WAKE_TEMPLATE_MAX_SAMPLES (VOICE_SR_SAMPLE_RATE * 4)
#define VOICE_SR_WAKE_TEMPLATE_COOLDOWN_US 900000LL
#define VOICE_SR_WAKE_TEMPLATE_CHECK_US 250000LL
#define VOICE_SR_WAKE_TEMPLATE_PHRASE_END_US 500000LL
#define VOICE_SR_WAKE_TEMPLATE_PHRASE_MIN_US 60000LL
#define VOICE_SR_WAKE_TEMPLATE_PHRASE_MAX_US 3500000LL
#define VOICE_SR_WAKE_TEMPLATE_PHRASE_MIN_FRAMES 1U
#define VOICE_SR_WAKE_TEMPLATE_EXPERIMENT 1
#define VOICE_SR_WAKE_TEMPLATE_MISS_LOG_THRESHOLD 0.35f
#define VOICE_SR_WAKE_TEMPLATE_LEARN_MAX 20U
#define VOICE_SR_COMMAND_LISTEN_DELAY_US 200000LL
#define VOICE_SR_COMMAND_WINDOW_US 8000000LL
#define VOICE_SR_MN_CMD_SLEEP_BUILTIN 41
#define VOICE_SR_MN_CMD_STOP_BUILTIN_1 154
#define VOICE_SR_MN_CMD_STOP_BUILTIN_2 191
#define VOICE_SR_MN_CMD_SLEEP_CUSTOM_1 314
#define VOICE_SR_MN_CMD_SLEEP_CUSTOM_2 315
#define VOICE_SR_MN_CMD_SLEEP_CUSTOM_3 316
#define VOICE_SR_MN_CMD_SLEEP_CUSTOM_4 317
#define VOICE_SR_MN_CMD_SLEEP_CUSTOM_5 318
#define VOICE_SR_MN_CMD_SLEEP_CUSTOM_6 319
#define VOICE_SR_MN_CMD_STOP_CUSTOM_1 320
#define VOICE_SR_MN_CMD_STOP_CUSTOM_2 321
#define VOICE_SR_MN_CMD_STOP_CUSTOM_3 322
#define VOICE_SR_MN_CMD_LIGHT_ON 323
#define VOICE_SR_MN_CMD_LIGHT_OFF 324
#define VOICE_SR_MN_CMD_LIGHT_BRIGHTER 325
#define VOICE_SR_MN_CMD_LIGHT_DIMMER 326
#define VOICE_SR_WAKE_NVS_NAMESPACE "dg_wake"
#define VOICE_SR_WAKE_NVS_TEMPLATE_THRESHOLD "tmpl_thr"
#define VOICE_SR_WAKE_NVS_TEMPLATE_CONFIRM "tmpl_conf"
#define VOICE_SR_WAKE_NVS_TEMPLATE_MIN_CONFIRM "tmpl_min"
#define VOICE_SR_DEBUG_CAPTURE_MAX_SEC 10U
#define VOICE_SR_DEBUG_CAPTURE_DEFAULT_SEC 5U
#define VOICE_SR_DEBUG_CAPTURE_STACK 4096
#define VOICE_SR_COMMAND_AUDIO_MAX_SEC 8U
#define VOICE_SR_COMMAND_AUDIO_DEFAULT_SEC 5U

static const char *TAG = "voice_sr";

typedef struct {
    bool valid;
    uint32_t samples;
    char name[24];
    float bins[VOICE_SR_WAKE_TEMPLATE_BINS];
    float zcr_bins[VOICE_SR_WAKE_TEMPLATE_BINS];
} wake_template_t;

#if CONFIG_DEBUG_AUDIO_CAPTURE
typedef struct {
    int64_t timestamp_us;
    float audio_rms;
    int16_t audio_peak;
    int vad_state;
    float wake_score;
    float threshold;
    uint32_t consecutive_hit_frames;
    uint32_t inference_time_us;
    uint32_t i2s_overflow_count;
    uint32_t audio_drop_count;
} debug_capture_score_record_t;
#endif

typedef struct {
    voice_sr_status_t status;
    esp_codec_dev_handle_t mic;
    const esp_afe_sr_iface_t *afe;
    esp_afe_sr_data_t *afe_data;
    const esp_mn_iface_t *multinet;
    model_iface_data_t *mn_data;
    srmodel_list_t *models;
    TaskHandle_t feed_task;
    TaskHandle_t detect_task;
    int16_t *feed_buffer;
    int16_t *wake_ring;
    int16_t *template_window;
    wake_template_t templates[VOICE_SR_WAKE_TEMPLATE_MAX];
    wake_template_t learn_templates[VOICE_SR_WAKE_TEMPLATE_LEARN_MAX];
    int feed_chunk;
    int fetch_chunk;
    uint32_t wake_ring_pos;
    uint32_t wake_ring_filled;
    int64_t template_last_check_us;
    int64_t template_last_wake_us;
    int64_t template_expires_us;
    int64_t template_phrase_start_us;
    int64_t template_phrase_last_speech_us;
    float template_threshold;
    float template_confirm_threshold;
    uint32_t template_min_confirmations;
    uint32_t template_phrase_frames;
    uint32_t template_learn_target;
    uint32_t template_learn_count;
    int64_t last_feed_us;
    int64_t last_fetch_us;
#if CONFIG_DEBUG_AUDIO_CAPTURE
    int16_t *capture_raw;
    int16_t *capture_afe;
    TaskHandle_t capture_writer_task;
    uint32_t capture_limit_samples;
    uint32_t capture_raw_samples;
    uint32_t capture_afe_samples;
    uint32_t capture_score_frames;
    uint32_t capture_raw_drops;
    uint32_t capture_afe_drops;
    uint32_t capture_score_drops;
    uint32_t capture_seconds;
    int64_t capture_end_us;
    bool capture_active;
    bool capture_write_pending;
    bool capture_quiet_was_enabled;
    char capture_label[80];
    char capture_raw_path[96];
    char capture_afe_path[96];
    char capture_score_path[96];
    debug_capture_score_record_t *capture_scores;
    uint32_t capture_score_limit;
#endif
    int16_t *command_audio;
    uint32_t command_audio_limit_samples;
    uint32_t command_audio_samples;
    uint32_t command_audio_drops;
    uint32_t command_audio_seconds;
    int64_t command_audio_end_us;
    bool command_audio_active;
    bool command_audio_ready;
    bool started;
    bool wake_pending;
    voice_sr_command_t command_pending;
    bool template_experiment_enabled;
    bool template_phrase_active;
    bool template_learning;
    bool assistant_suppressed;
    int64_t pause_until_us;
    portMUX_TYPE lock;
} voice_sr_context_t;

static voice_sr_context_t s_sr = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static void set_result_locked(esp_err_t err, const char *result)
{
    s_sr.status.last_error = err;
    snprintf(s_sr.status.last_result, sizeof(s_sr.status.last_result),
             "%s", result ? result : esp_err_to_name(err));
}

static void set_result(esp_err_t err, const char *result)
{
    portENTER_CRITICAL(&s_sr.lock);
    set_result_locked(err, result);
    portEXIT_CRITICAL(&s_sr.lock);
}

static BaseType_t create_sr_task(TaskFunction_t task_func, const char *name,
                                 configSTACK_DEPTH_TYPE stack_size, TaskHandle_t *task_handle,
                                 BaseType_t core_id)
{
    /* Feed/detect loops do not access NVS or flash while running. Prefer
     * PSRAM for their 4 KiB + 12 KiB stacks so HTTPS/TLS still has contiguous
     * internal RAM when sleep audio and Feishu reporting run together. */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(task_func, name, stack_size, NULL,
                                                    VOICE_SR_TASK_PRIORITY, task_handle,
                                                    core_id, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreatePinnedToCoreWithCaps(task_func, name, stack_size, NULL,
                                            VOICE_SR_TASK_PRIORITY, task_handle,
                                            core_id, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ok;
}

static char *select_wakenet_model(srmodel_list_t *models)
{
#ifdef CONFIG_SR_WN_WN9_CUSTOMWORD
    char *custom = esp_srmodel_filter(models, ESP_WN_PREFIX, "customword");
    if (custom) {
        ESP_LOGI(TAG, "selected custom WakeNet model=%s", custom);
        return custom;
    }
    ESP_LOGW(TAG, "custom WakeNet selected in sdkconfig but model not found");
#endif

    char *xiaozhi = esp_srmodel_filter(models, ESP_WN_PREFIX, "xiaozhi");
    if (xiaozhi) {
        ESP_LOGI(TAG, "selected fallback WakeNet model=%s", xiaozhi);
        return xiaozhi;
    }

    char *hiesp = esp_srmodel_filter(models, ESP_WN_PREFIX, "hiesp");
    if (hiesp) {
        ESP_LOGI(TAG, "selected fallback WakeNet model=%s", hiesp);
        return hiesp;
    }

    return esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
}

static char *select_multinet_model(srmodel_list_t *models)
{
    char *cn = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (cn) {
        ESP_LOGI(TAG, "selected Chinese MultiNet model=%s", cn);
        return cn;
    }
    char *any = esp_srmodel_filter(models, ESP_MN_PREFIX, NULL);
    if (any) {
        ESP_LOGW(TAG, "Chinese MultiNet model not found; fallback model=%s", any);
    }
    return any;
}

typedef struct {
    int command_id;
    char *phrase;
} voice_sr_mn_phrase_t;

static esp_err_t voice_sr_multinet_configure_commands(const esp_mn_iface_t *mn,
                                                       model_iface_data_t *mn_data)
{
    static const voice_sr_mn_phrase_t phrases[] = {
        { VOICE_SR_MN_CMD_SLEEP_CUSTOM_1, "shui jiao" },
        { VOICE_SR_MN_CMD_SLEEP_CUSTOM_2, "shui jiao le" },
        { VOICE_SR_MN_CMD_SLEEP_CUSTOM_3, "shui jiao ba" },
        { VOICE_SR_MN_CMD_SLEEP_CUSTOM_4, "wo yao shui jiao" },
        { VOICE_SR_MN_CMD_SLEEP_CUSTOM_5, "kai shi shui mian" },
        { VOICE_SR_MN_CMD_SLEEP_CUSTOM_6, "kai shi zhu mian" },
        { VOICE_SR_MN_CMD_STOP_CUSTOM_1, "ting zhi" },
        { VOICE_SR_MN_CMD_STOP_CUSTOM_2, "tui chu" },
        { VOICE_SR_MN_CMD_STOP_CUSTOM_3, "guan bi" },
        { VOICE_SR_MN_CMD_LIGHT_ON, "kai deng" },
        { VOICE_SR_MN_CMD_LIGHT_ON, "da kai deng" },
        { VOICE_SR_MN_CMD_LIGHT_ON, "bang wo kai deng" },
        { VOICE_SR_MN_CMD_LIGHT_ON, "da kai dian deng" },
        { VOICE_SR_MN_CMD_LIGHT_OFF, "guan deng" },
        { VOICE_SR_MN_CMD_LIGHT_OFF, "guan bi deng" },
        { VOICE_SR_MN_CMD_LIGHT_OFF, "bang wo guan deng" },
        { VOICE_SR_MN_CMD_LIGHT_OFF, "guan bi dian deng" },
        { VOICE_SR_MN_CMD_LIGHT_BRIGHTER, "deng guang bian liang" },
        { VOICE_SR_MN_CMD_LIGHT_BRIGHTER, "tiao liang deng guang" },
        { VOICE_SR_MN_CMD_LIGHT_BRIGHTER, "deng guang tiao liang" },
        { VOICE_SR_MN_CMD_LIGHT_DIMMER, "deng guang bian an" },
        { VOICE_SR_MN_CMD_LIGHT_DIMMER, "tiao an deng guang" },
        { VOICE_SR_MN_CMD_LIGHT_DIMMER, "deng guang tiao an" },
    };

    esp_err_t err = esp_mn_commands_alloc(mn, mn_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MultiNet command allocation failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t accepted = 0;
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); ++i) {
        err = esp_mn_commands_add(phrases[i].command_id, phrases[i].phrase);
        if (err == ESP_OK) {
            accepted++;
        } else {
            ESP_LOGW(TAG, "MultiNet phrase rejected id=%d phrase=%s err=%s",
                     phrases[i].command_id, phrases[i].phrase, esp_err_to_name(err));
        }
    }

    esp_mn_error_t *update_errors = esp_mn_commands_update();
    if (update_errors && update_errors->num > 0) {
        ESP_LOGE(TAG, "MultiNet command update rejected %d phrase(s)", update_errors->num);
        for (int i = 0; i < update_errors->num; ++i) {
            const esp_mn_phrase_t *phrase = update_errors->phrases[i];
            if (phrase) {
                ESP_LOGE(TAG, "MultiNet update rejected id=%d phrase=%s",
                         phrase->command_id, phrase->string ? phrase->string : "");
            }
        }
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "MultiNet local commands ready accepted=%u light_ids=%d/%d/%d/%d",
             (unsigned)accepted, VOICE_SR_MN_CMD_LIGHT_ON, VOICE_SR_MN_CMD_LIGHT_OFF,
             VOICE_SR_MN_CMD_LIGHT_BRIGHTER, VOICE_SR_MN_CMD_LIGHT_DIMMER);
    return accepted > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t voice_sr_multinet_init(char *mn_name)
{
    if (!mn_name) {
        set_result(ESP_ERR_NOT_FOUND, "multinet_model_not_found");
        return ESP_ERR_NOT_FOUND;
    }

    esp_mn_iface_t *mn = esp_mn_handle_from_name(mn_name);
    if (!mn) {
        set_result(ESP_ERR_NOT_FOUND, "multinet_handle_not_found");
        return ESP_ERR_NOT_FOUND;
    }

    model_iface_data_t *mn_data = mn->create(mn_name, 5760);
    if (!mn_data) {
        set_result(ESP_ERR_NO_MEM, "multinet_create_failed");
        return ESP_ERR_NO_MEM;
    }

    s_sr.multinet = mn;
    s_sr.mn_data = mn_data;
    snprintf(s_sr.status.command_model, sizeof(s_sr.status.command_model), "%s", mn_name);

    esp_err_t commands_err = voice_sr_multinet_configure_commands(mn, mn_data);
    if (commands_err != ESP_OK) {
        ESP_LOGW(TAG, "custom MultiNet commands unavailable; packaged commands remain active err=%s",
                 esp_err_to_name(commands_err));
    }

    ESP_LOGI(TAG, "MultiNet ready model=%s builtin_commands sleep=%d stop=%d/%d custom_err=%s",
             mn_name, VOICE_SR_MN_CMD_SLEEP_BUILTIN,
             VOICE_SR_MN_CMD_STOP_BUILTIN_1, VOICE_SR_MN_CMD_STOP_BUILTIN_2,
             esp_err_to_name(commands_err));
    return ESP_OK;
}

static voice_sr_command_t command_from_mn_id(int command_id)
{
    switch (command_id) {
    case VOICE_SR_MN_CMD_SLEEP_BUILTIN:
    case VOICE_SR_MN_CMD_SLEEP_CUSTOM_1:
    case VOICE_SR_MN_CMD_SLEEP_CUSTOM_2:
    case VOICE_SR_MN_CMD_SLEEP_CUSTOM_3:
    case VOICE_SR_MN_CMD_SLEEP_CUSTOM_4:
    case VOICE_SR_MN_CMD_SLEEP_CUSTOM_5:
    case VOICE_SR_MN_CMD_SLEEP_CUSTOM_6:
        return VOICE_SR_COMMAND_SLEEP;
    case VOICE_SR_MN_CMD_STOP_BUILTIN_1:
    case VOICE_SR_MN_CMD_STOP_BUILTIN_2:
    case VOICE_SR_MN_CMD_STOP_CUSTOM_1:
    case VOICE_SR_MN_CMD_STOP_CUSTOM_2:
    case VOICE_SR_MN_CMD_STOP_CUSTOM_3:
        return VOICE_SR_COMMAND_STOP;
    case VOICE_SR_MN_CMD_LIGHT_ON:
        return VOICE_SR_COMMAND_LIGHT_ON;
    case VOICE_SR_MN_CMD_LIGHT_OFF:
        return VOICE_SR_COMMAND_LIGHT_OFF;
    case VOICE_SR_MN_CMD_LIGHT_BRIGHTER:
        return VOICE_SR_COMMAND_LIGHT_BRIGHTER;
    case VOICE_SR_MN_CMD_LIGHT_DIMMER:
        return VOICE_SR_COMMAND_LIGHT_DIMMER;
    default:
        return VOICE_SR_COMMAND_NONE;
    }
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffU);
    p[1] = (uint8_t)((v >> 8) & 0xffU);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffU);
    p[1] = (uint8_t)((v >> 8) & 0xffU);
    p[2] = (uint8_t)((v >> 16) & 0xffU);
    p[3] = (uint8_t)((v >> 24) & 0xffU);
}

static void status_update_max_u32(uint32_t *field, uint32_t value)
{
    if (field && value > *field) {
        *field = value;
    }
}

#if CONFIG_DEBUG_AUDIO_CAPTURE
static void debug_capture_sanitize_label(const char *label, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    size_t pos = 0;
    const char *src = (label && label[0]) ? label : "capture";
    for (size_t i = 0; src[i] && pos + 1 < out_size; ++i) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out[pos++] = c;
        } else {
            out[pos++] = '_';
        }
    }
    out[pos] = '\0';
}

static esp_err_t debug_capture_write_wav(const char *path, const int16_t *samples, uint32_t sample_count)
{
    if (!path || !samples || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        return ESP_FAIL;
    }

    uint32_t data_bytes = sample_count * sizeof(int16_t);
    uint8_t header[44] = {0};
    memcpy(header, "RIFF", 4);
    write_le32(header + 4, 36U + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    write_le32(header + 16, 16);
    write_le16(header + 20, 1);
    write_le16(header + 22, 1);
    write_le32(header + 24, VOICE_SR_SAMPLE_RATE);
    write_le32(header + 28, VOICE_SR_SAMPLE_RATE * sizeof(int16_t));
    write_le16(header + 32, sizeof(int16_t));
    write_le16(header + 34, VOICE_SR_BITS);
    memcpy(header + 36, "data", 4);
    write_le32(header + 40, data_bytes);

    esp_err_t err = ESP_OK;
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && fwrite(samples, sizeof(int16_t), sample_count, file) != sample_count) {
        err = ESP_FAIL;
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    return err;
}

static esp_err_t debug_capture_write_score_csv(const char *path,
                                               const debug_capture_score_record_t *records,
                                               uint32_t record_count)
{
    if (!path || !records) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        return ESP_FAIL;
    }
    fprintf(file,
            "timestamp_us,audio_rms,audio_peak,vad_state,wake_score,threshold,"
            "consecutive_hit_frames,inference_time_ms,i2s_overflow_count,audio_drop_count\r\n");
    for (uint32_t i = 0; i < record_count; ++i) {
        const debug_capture_score_record_t *r = &records[i];
        fprintf(file, "%lld,%.3f,%d,%d,",
                (long long)r->timestamp_us,
                (double)r->audio_rms,
                (int)r->audio_peak,
                r->vad_state);
        if (r->wake_score > -9.0f) {
            fprintf(file, "%.5f", (double)r->wake_score);
        }
        fprintf(file, ",%.5f,%u,%.3f,%u,%u\r\n",
                (double)r->threshold,
                (unsigned)r->consecutive_hit_frames,
                (double)r->inference_time_us / 1000.0,
                (unsigned)r->i2s_overflow_count,
                (unsigned)r->audio_drop_count);
    }
    esp_err_t err = ferror(file) ? ESP_FAIL : ESP_OK;
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    return err;
}

static void debug_capture_writer_task(void *arg)
{
    (void)arg;
    uint32_t raw_count = s_sr.capture_raw_samples;
    uint32_t afe_count = s_sr.capture_afe_samples;
    uint32_t score_count = s_sr.capture_score_frames;
    char raw_path[sizeof(s_sr.capture_raw_path)];
    char afe_path[sizeof(s_sr.capture_afe_path)];
    char score_path[sizeof(s_sr.capture_score_path)];
    snprintf(raw_path, sizeof(raw_path), "%s", s_sr.capture_raw_path);
    snprintf(afe_path, sizeof(afe_path), "%s", s_sr.capture_afe_path);
    snprintf(score_path, sizeof(score_path), "%s", s_sr.capture_score_path);

    uint16_t raw_peak = 0;
    uint64_t raw_energy = 0;
    for (uint32_t i = 0; i < raw_count; ++i) {
        int32_t sample = s_sr.capture_raw[i];
        uint16_t magnitude = (uint16_t)(sample < 0 ? -sample : sample);
        if (magnitude > raw_peak) {
            raw_peak = magnitude;
        }
        raw_energy += (uint64_t)(sample * sample);
    }
    float raw_rms = raw_count > 0 ? sqrtf((float)raw_energy / (float)raw_count) : 0.0f;

    esp_err_t raw_err = debug_capture_write_wav(raw_path, s_sr.capture_raw, raw_count);
    esp_err_t afe_err = debug_capture_write_wav(afe_path, s_sr.capture_afe, afe_count);
    esp_err_t score_err = debug_capture_write_score_csv(score_path, s_sr.capture_scores, score_count);

    portENTER_CRITICAL(&s_sr.lock);
    s_sr.capture_write_pending = false;
    s_sr.status.debug_capture_write_pending = false;
    s_sr.status.debug_capture_raw_samples = raw_count;
    s_sr.status.debug_capture_afe_samples = afe_count;
    s_sr.status.debug_capture_score_frames = score_count;
    snprintf(s_sr.status.debug_capture_raw_path, sizeof(s_sr.status.debug_capture_raw_path), "%s", raw_path);
    snprintf(s_sr.status.debug_capture_afe_path, sizeof(s_sr.status.debug_capture_afe_path), "%s", afe_path);
    snprintf(s_sr.status.debug_capture_score_path, sizeof(s_sr.status.debug_capture_score_path),
             "%s", score_path);
    bool saved = raw_err == ESP_OK && afe_err == ESP_OK && score_err == ESP_OK;
    if (saved) {
        set_result_locked(ESP_OK, "debug_capture_saved");
    } else {
        set_result_locked(ESP_FAIL, "debug_capture_save_failed");
    }
    portEXIT_CRITICAL(&s_sr.lock);

    if (saved) {
        printf("REC SAVED label=%s raw=%s afe=%s score=%s raw_samples=%u afe_samples=%u score_frames=%u peak=%u rms=%.1f drops=%u/%u/%u\r\n",
               s_sr.capture_label,
               raw_path,
               afe_path,
               score_path,
               (unsigned)raw_count,
               (unsigned)afe_count,
               (unsigned)score_count,
               (unsigned)raw_peak,
               (double)raw_rms,
               (unsigned)s_sr.capture_raw_drops,
               (unsigned)s_sr.capture_afe_drops,
               (unsigned)s_sr.capture_score_drops);
    } else {
        printf("REC FAILED label=%s raw_err=%s afe_err=%s score_err=%s raw=%s afe=%s score=%s drops=%u/%u/%u\r\n",
               s_sr.capture_label,
               esp_err_to_name(raw_err),
               esp_err_to_name(afe_err),
               esp_err_to_name(score_err),
               raw_path,
               afe_path,
               score_path,
               (unsigned)s_sr.capture_raw_drops,
               (unsigned)s_sr.capture_afe_drops,
               (unsigned)s_sr.capture_score_drops);
    }
    dg_debug_quiet_set(s_sr.capture_quiet_was_enabled);

    s_sr.capture_writer_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void debug_capture_finish_if_due(int64_t now_us)
{
    bool start_writer = false;
    portENTER_CRITICAL(&s_sr.lock);
    if (s_sr.capture_active && now_us >= s_sr.capture_end_us) {
        s_sr.capture_active = false;
        s_sr.capture_write_pending = true;
        s_sr.status.debug_capture_active = false;
        s_sr.status.debug_capture_write_pending = true;
        s_sr.status.debug_capture_raw_samples = s_sr.capture_raw_samples;
        s_sr.status.debug_capture_afe_samples = s_sr.capture_afe_samples;
        s_sr.status.debug_capture_score_frames = s_sr.capture_score_frames;
        s_sr.status.debug_capture_raw_drops = s_sr.capture_raw_drops;
        s_sr.status.debug_capture_afe_drops = s_sr.capture_afe_drops;
        s_sr.status.debug_capture_score_drops = s_sr.capture_score_drops;
        start_writer = true;
    }
    portEXIT_CRITICAL(&s_sr.lock);

    if (start_writer && !s_sr.capture_writer_task) {
        /* SPIFFS writes suspend the external-memory cache, so this writer
         * must never execute on a PSRAM stack. */
        BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(debug_capture_writer_task,
                                                        "audio_cap_writer",
                                                        VOICE_SR_DEBUG_CAPTURE_STACK,
                                                        NULL, 1,
                                                        &s_sr.capture_writer_task,
                                                        1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ok != pdPASS) {
            portENTER_CRITICAL(&s_sr.lock);
            s_sr.capture_write_pending = false;
            s_sr.status.debug_capture_write_pending = false;
            s_sr.status.audio_drop_count++;
            set_result_locked(ESP_ERR_NO_MEM, "debug_capture_writer_failed");
            dg_debug_quiet_set(s_sr.capture_quiet_was_enabled);
            portEXIT_CRITICAL(&s_sr.lock);
        }
    }
}

static void debug_capture_append_raw(const int16_t *interleaved, uint32_t frames, int64_t now_us)
{
    if (!s_sr.capture_active || !interleaved || !s_sr.capture_raw) {
        return;
    }
    uint32_t pos = s_sr.capture_raw_samples;
    uint32_t remain = pos < s_sr.capture_limit_samples ? s_sr.capture_limit_samples - pos : 0;
    uint32_t to_copy = frames < remain ? frames : remain;
    for (uint32_t i = 0; i < to_copy; ++i) {
        s_sr.capture_raw[pos + i] = interleaved[i * VOICE_SR_MIC_CHANNELS];
    }
    s_sr.capture_raw_samples = pos + to_copy;
    if (to_copy < frames) {
        s_sr.capture_raw_drops += frames - to_copy;
        s_sr.status.audio_drop_count += frames - to_copy;
    }
    debug_capture_finish_if_due(now_us);
}

static void debug_capture_append_afe(const int16_t *samples, uint32_t count, int64_t now_us)
{
    if (!s_sr.capture_active || !samples || !s_sr.capture_afe) {
        return;
    }
    uint32_t pos = s_sr.capture_afe_samples;
    uint32_t remain = pos < s_sr.capture_limit_samples ? s_sr.capture_limit_samples - pos : 0;
    uint32_t to_copy = count < remain ? count : remain;
    if (to_copy > 0) {
        memcpy(s_sr.capture_afe + pos, samples, to_copy * sizeof(int16_t));
    }
    s_sr.capture_afe_samples = pos + to_copy;
    if (to_copy < count) {
        s_sr.capture_afe_drops += count - to_copy;
        s_sr.status.audio_drop_count += count - to_copy;
    }
    debug_capture_finish_if_due(now_us);
}

static void debug_capture_append_score(const int16_t *samples, uint32_t count,
                                       int64_t timestamp_us, int vad_state,
                                       float wake_score, float threshold,
                                       uint32_t consecutive_hit_frames,
                                       uint32_t inference_time_us)
{
    if (!s_sr.capture_active || !s_sr.capture_scores) {
        return;
    }
    uint32_t pos = s_sr.capture_score_frames;
    if (pos >= s_sr.capture_score_limit) {
        s_sr.capture_score_drops++;
        s_sr.status.audio_drop_count++;
        return;
    }
    double sum = 0.0;
    int32_t peak = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int16_t v = samples ? samples[i] : 0;
        int32_t av = v < 0 ? -(int32_t)v : (int32_t)v;
        if (av > peak) {
            peak = av;
        }
        sum += (double)v * (double)v;
    }
    debug_capture_score_record_t *r = &s_sr.capture_scores[pos];
    r->timestamp_us = timestamp_us;
    r->audio_rms = count ? sqrtf((float)(sum / (double)count)) : 0.0f;
    r->audio_peak = peak > 32767 ? 32767 : (int16_t)peak;
    r->vad_state = vad_state;
    r->wake_score = wake_score;
    r->threshold = threshold;
    r->consecutive_hit_frames = consecutive_hit_frames;
    r->inference_time_us = inference_time_us;
    r->i2s_overflow_count = s_sr.status.i2s_overflow_count;
    r->audio_drop_count = s_sr.status.audio_drop_count;
    s_sr.capture_score_frames = pos + 1;
}
#else
static void debug_capture_append_raw(const int16_t *interleaved, uint32_t frames, int64_t now_us)
{
    (void)interleaved;
    (void)frames;
    (void)now_us;
}

static void debug_capture_append_afe(const int16_t *samples, uint32_t count, int64_t now_us)
{
    (void)samples;
    (void)count;
    (void)now_us;
}

static void debug_capture_append_score(const int16_t *samples, uint32_t count,
                                       int64_t timestamp_us, int vad_state,
                                       float wake_score, float threshold,
                                       uint32_t consecutive_hit_frames,
                                       uint32_t inference_time_us)
{
    (void)samples;
    (void)count;
    (void)timestamp_us;
    (void)vad_state;
    (void)wake_score;
    (void)threshold;
    (void)consecutive_hit_frames;
    (void)inference_time_us;
}
#endif

static void command_audio_finish_locked(const char *reason)
{
    (void)reason;
    s_sr.command_audio_active = false;
    s_sr.command_audio_ready = s_sr.command_audio_samples > 0;
    s_sr.status.command_audio_active = false;
    s_sr.status.command_audio_ready = s_sr.command_audio_ready;
    s_sr.status.command_audio_samples = s_sr.command_audio_samples;
    s_sr.status.command_audio_drops = s_sr.command_audio_drops;
    set_result_locked(s_sr.command_audio_ready ? ESP_OK : ESP_ERR_INVALID_SIZE,
                      s_sr.command_audio_ready ? "command_audio_ready" : "command_audio_empty");
}

static void command_audio_append_afe(const int16_t *samples, uint32_t count, int64_t now_us)
{
    if (!samples || count == 0) {
        return;
    }
    const char *finish_reason = NULL;
    uint32_t finish_samples = 0;
    uint32_t finish_drops = 0;
    portENTER_CRITICAL(&s_sr.lock);
    if (!s_sr.command_audio_active || !s_sr.command_audio) {
        portEXIT_CRITICAL(&s_sr.lock);
        return;
    }
    uint32_t pos = s_sr.command_audio_samples;
    uint32_t remain = pos < s_sr.command_audio_limit_samples ?
                      s_sr.command_audio_limit_samples - pos : 0;
    uint32_t to_copy = count < remain ? count : remain;
    if (to_copy > 0) {
        memcpy(s_sr.command_audio + pos, samples, to_copy * sizeof(int16_t));
        s_sr.command_audio_samples = pos + to_copy;
    }
    if (to_copy < count) {
        s_sr.command_audio_drops += count - to_copy;
        s_sr.status.audio_drop_count += count - to_copy;
    }
    if (s_sr.command_audio_samples >= s_sr.command_audio_limit_samples) {
        command_audio_finish_locked("full");
        finish_reason = "full";
        finish_samples = s_sr.command_audio_samples;
        finish_drops = s_sr.command_audio_drops;
    } else if (now_us >= s_sr.command_audio_end_us) {
        command_audio_finish_locked("timeout");
        finish_reason = "timeout";
        finish_samples = s_sr.command_audio_samples;
        finish_drops = s_sr.command_audio_drops;
    }
    portEXIT_CRITICAL(&s_sr.lock);
    if (finish_reason) {
        printf("DG ASR: listen ready reason=%s samples=%u drops=%u\r\n",
               finish_reason,
               (unsigned)finish_samples,
               (unsigned)finish_drops);
    }
}

static bool wake_extract_features(const int16_t *samples, uint32_t sample_count,
                                  float *bins, float *zcr_bins, uint32_t *trimmed_samples)
{
    if (!samples || !bins || sample_count < VOICE_SR_WAKE_TEMPLATE_MIN_SAMPLES) {
        return false;
    }

    int16_t peak = 0;
    for (uint32_t i = 0; i < sample_count; ++i) {
        int16_t v = samples[i] < 0 ? (int16_t)-samples[i] : samples[i];
        if (v > peak) {
            peak = v;
        }
    }
    if (peak < 300) {
        return false;
    }

    int threshold = peak / 8;
    if (threshold < 350) {
        threshold = 350;
    }
    uint32_t start = 0;
    uint32_t end = sample_count;
    while (start < sample_count && abs(samples[start]) < threshold) {
        start++;
    }
    while (end > start && abs(samples[end - 1]) < threshold) {
        end--;
    }
    uint32_t pad = VOICE_SR_SAMPLE_RATE / 10;
    start = start > pad ? start - pad : 0;
    end = end + pad < sample_count ? end + pad : sample_count;
    if (end <= start || end - start < VOICE_SR_WAKE_TEMPLATE_MIN_SAMPLES) {
        return false;
    }
    uint32_t len = end - start;
    if (len > VOICE_SR_WAKE_TEMPLATE_MAX_SAMPLES) {
        start = end - VOICE_SR_WAKE_TEMPLATE_MAX_SAMPLES;
        len = VOICE_SR_WAKE_TEMPLATE_MAX_SAMPLES;
    }

    float mean = 0.0f;
    for (uint32_t b = 0; b < VOICE_SR_WAKE_TEMPLATE_BINS; ++b) {
        uint32_t a = start + (uint32_t)(((uint64_t)b * len) / VOICE_SR_WAKE_TEMPLATE_BINS);
        uint32_t z = start + (uint32_t)(((uint64_t)(b + 1) * len) / VOICE_SR_WAKE_TEMPLATE_BINS);
        if (z <= a) {
            z = a + 1;
        }
        double sum = 0.0;
        uint32_t count = 0;
        uint32_t zero_cross = 0;
        int16_t prev = samples[a];
        for (uint32_t i = a; i < z && i < sample_count; ++i) {
            float x = (float)samples[i] / 32768.0f;
            sum += (double)x * (double)x;
            if (i > a && ((prev < 0 && samples[i] >= 0) || (prev >= 0 && samples[i] < 0))) {
                zero_cross++;
            }
            prev = samples[i];
            count++;
        }
        float rms = count ? sqrtf((float)(sum / count)) : 0.0f;
        bins[b] = log10f(rms + 0.00003f);
        if (zcr_bins) {
            zcr_bins[b] = count > 1 ? (float)zero_cross / (float)(count - 1U) : 0.0f;
        }
        mean += bins[b];
    }
    mean /= (float)VOICE_SR_WAKE_TEMPLATE_BINS;
    float var = 0.0f;
    for (uint32_t b = 0; b < VOICE_SR_WAKE_TEMPLATE_BINS; ++b) {
        bins[b] -= mean;
        var += bins[b] * bins[b];
    }
    float scale = sqrtf(var) + 0.0001f;
    for (uint32_t b = 0; b < VOICE_SR_WAKE_TEMPLATE_BINS; ++b) {
        bins[b] /= scale;
    }
    if (trimmed_samples) {
        *trimmed_samples = len;
    }
    return true;
}

static bool wake_load_wav_template(int index, wake_template_t *tmpl)
{
    if (!tmpl) {
        return false;
    }
    char path[64];
    snprintf(tmpl->name, sizeof(tmpl->name), "wake_xm_%02d.wav", index);
    snprintf(path, sizeof(path), "/spiffs/%s", tmpl->name);

    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fclose(file);
        return false;
    }

    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    uint32_t data_bytes = 0;
    long data_offset = 0;
    while (!feof(file)) {
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) {
            break;
        }
        uint32_t size = read_le32(chunk + 4);
        long next = ftell(file) + (long)size + (long)(size & 1U);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[32] = {0};
            size_t to_read = size < sizeof(fmt) ? size : sizeof(fmt);
            if (fread(fmt, 1, to_read, file) == to_read && to_read >= 16) {
                channels = read_le16(fmt + 2);
                sample_rate = read_le32(fmt + 4);
                bits = read_le16(fmt + 14);
            }
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_offset = ftell(file);
            data_bytes = size;
        }
        if (fseek(file, next, SEEK_SET) != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        if (channels && sample_rate && bits && data_offset && data_bytes) {
            break;
        }
    }

    if (channels < 1 || sample_rate != VOICE_SR_SAMPLE_RATE || bits != VOICE_SR_BITS ||
        data_offset <= 0 || data_bytes < 2) {
        fclose(file);
        return false;
    }

    uint32_t frames = data_bytes / (uint32_t)(channels * sizeof(int16_t));
    if (frames > VOICE_SR_WAKE_TEMPLATE_MAX_SAMPLES) {
        frames = VOICE_SR_WAKE_TEMPLATE_MAX_SAMPLES;
    }
    int16_t *pcm = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        pcm = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_8BIT);
    }
    if (!pcm) {
        fclose(file);
        return false;
    }

    if (fseek(file, data_offset, SEEK_SET) != 0) {
        free(pcm);
        fclose(file);
        return false;
    }
    size_t frame_bytes = (size_t)channels * sizeof(int16_t);
    size_t raw_bytes = (size_t)frames * frame_bytes;
    uint8_t *raw = heap_caps_malloc(raw_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) {
        raw = heap_caps_malloc(raw_bytes, MALLOC_CAP_8BIT);
    }
    if (!raw) {
        free(pcm);
        fclose(file);
        return false;
    }
    size_t read_bytes = fread(raw, 1, raw_bytes, file);
    frames = (uint32_t)(read_bytes / frame_bytes);
    for (uint32_t i = 0; i < frames; ++i) {
        pcm[i] = (int16_t)read_le16(raw + (size_t)i * frame_bytes);
        if ((i & 0x7ffU) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    free(raw);
    fclose(file);

    uint32_t trimmed = 0;
    bool ok = wake_extract_features(pcm, frames, tmpl->bins, tmpl->zcr_bins, &trimmed);
    free(pcm);
    tmpl->valid = ok;
    tmpl->samples = trimmed;
    return ok;
}

static uint32_t wake_load_feature_templates_from_file(const char *path, const char *source)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return 0;
    }

    uint8_t header[12] = {0};
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, VOICE_SR_WAKE_TEMPLATE_FILE_MAGIC, 4) != 0 ||
        read_le16(header + 4) != VOICE_SR_WAKE_TEMPLATE_FILE_VERSION ||
        read_le16(header + 6) != VOICE_SR_WAKE_TEMPLATE_BINS) {
        fclose(file);
        ESP_LOGW(TAG, "wake template feature file invalid: %s", path);
        return 0;
    }

    uint32_t requested = read_le16(header + 8);
    if (requested > VOICE_SR_WAKE_TEMPLATE_MAX) {
        requested = VOICE_SR_WAKE_TEMPLATE_MAX;
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < requested; ++i) {
        uint8_t meta[38] = {0};
        wake_template_t *tmpl = &s_sr.templates[count];
        memset(tmpl, 0, sizeof(*tmpl));
        if (fread(meta, 1, sizeof(meta), file) != sizeof(meta)) {
            break;
        }
        tmpl->samples = read_le32(meta);
        size_t name_len = strnlen((const char *)(meta + 6), 32);
        if (name_len >= sizeof(tmpl->name)) {
            name_len = sizeof(tmpl->name) - 1;
        }
        memcpy(tmpl->name, meta + 6, name_len);
        tmpl->name[name_len] = '\0';
        if (fread(tmpl->bins, sizeof(float), VOICE_SR_WAKE_TEMPLATE_BINS, file) !=
            VOICE_SR_WAKE_TEMPLATE_BINS ||
            fread(tmpl->zcr_bins, sizeof(float), VOICE_SR_WAKE_TEMPLATE_BINS, file) !=
            VOICE_SR_WAKE_TEMPLATE_BINS) {
            break;
        }
        if (tmpl->samples >= VOICE_SR_WAKE_TEMPLATE_MIN_SAMPLES &&
            tmpl->samples <= VOICE_SR_WAKE_TEMPLATE_MAX_SAMPLES &&
            tmpl->name[0]) {
            tmpl->valid = true;
            count++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    fclose(file);
    if (count > 0) {
        portENTER_CRITICAL(&s_sr.lock);
        snprintf(s_sr.status.template_source, sizeof(s_sr.status.template_source),
                 "%s", source ? source : "compact");
        portEXIT_CRITICAL(&s_sr.lock);
        ESP_LOGI(TAG, "loaded %lu compact wake templates from %s",
                 (unsigned long)count, path);
        printf("DG Wake: loaded compact templates count=%lu source=%s file=%s\r\n",
               (unsigned long)count, source ? source : "compact", path);
    }
    return count;
}

static void wake_templates_load(void)
{
    memset(s_sr.templates, 0, sizeof(s_sr.templates));
    portENTER_CRITICAL(&s_sr.lock);
    s_sr.status.template_source[0] = '\0';
    portEXIT_CRITICAL(&s_sr.lock);

    uint32_t count = wake_load_feature_templates_from_file(VOICE_SR_WAKE_TEMPLATE_LIVE_FILE, "device");
    if (count == 0) {
        count = wake_load_feature_templates_from_file(VOICE_SR_WAKE_TEMPLATE_FILE, "pc");
    }
    if (count == 0) {
        for (int i = 1; i <= VOICE_SR_WAKE_TEMPLATE_MAX; ++i) {
            wake_template_t *tmpl = &s_sr.templates[count];
            memset(tmpl, 0, sizeof(*tmpl));
            if (wake_load_wav_template(i, tmpl)) {
                count++;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (count > 0) {
            portENTER_CRITICAL(&s_sr.lock);
            snprintf(s_sr.status.template_source, sizeof(s_sr.status.template_source), "%s", "wav");
            portEXIT_CRITICAL(&s_sr.lock);
        }
    }
    portENTER_CRITICAL(&s_sr.lock);
    s_sr.status.template_count = count;
    s_sr.status.template_threshold = s_sr.template_threshold;
    s_sr.status.template_confirm_threshold = s_sr.template_confirm_threshold;
    s_sr.status.template_min_confirmations = s_sr.template_min_confirmations;
    portEXIT_CRITICAL(&s_sr.lock);
    ESP_LOGI(TAG, "loaded %lu wake templates", (unsigned long)count);
}

static esp_err_t wake_save_feature_templates(const char *path,
                                             const wake_template_t *templates,
                                             uint32_t count)
{
    if (!path || !templates || count == 0 || count > VOICE_SR_WAKE_TEMPLATE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        return ESP_FAIL;
    }

    uint8_t header[12] = {0};
    memcpy(header, VOICE_SR_WAKE_TEMPLATE_FILE_MAGIC, 4);
    write_le16(header + 4, VOICE_SR_WAKE_TEMPLATE_FILE_VERSION);
    write_le16(header + 6, VOICE_SR_WAKE_TEMPLATE_BINS);
    write_le16(header + 8, (uint16_t)count);
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return ESP_FAIL;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (!templates[i].valid) {
            fclose(file);
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t meta[38] = {0};
        write_le32(meta, templates[i].samples);
        snprintf((char *)(meta + 6), 32, "%s", templates[i].name);
        if (fwrite(meta, 1, sizeof(meta), file) != sizeof(meta) ||
            fwrite(templates[i].bins, sizeof(float), VOICE_SR_WAKE_TEMPLATE_BINS, file) !=
            VOICE_SR_WAKE_TEMPLATE_BINS ||
            fwrite(templates[i].zcr_bins, sizeof(float), VOICE_SR_WAKE_TEMPLATE_BINS, file) !=
            VOICE_SR_WAKE_TEMPLATE_BINS) {
            fclose(file);
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    int close_err = fclose(file);
    return close_err == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t wake_template_runtime_prepare_locked(void)
{
    if (!s_sr.wake_ring) {
        s_sr.wake_ring = heap_caps_malloc(VOICE_SR_WAKE_RING_SAMPLES * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_sr.wake_ring) {
            s_sr.wake_ring = heap_caps_malloc(VOICE_SR_WAKE_RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
        }
        if (!s_sr.wake_ring) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_sr.template_window) {
        s_sr.template_window = heap_caps_malloc(VOICE_SR_WAKE_TEMPLATE_MAX_SAMPLES * sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_sr.template_window) {
            s_sr.template_window = heap_caps_malloc(VOICE_SR_WAKE_TEMPLATE_MAX_SAMPLES * sizeof(int16_t),
                                                    MALLOC_CAP_8BIT);
        }
        if (!s_sr.template_window) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(s_sr.wake_ring, 0, VOICE_SR_WAKE_RING_SAMPLES * sizeof(int16_t));
    s_sr.wake_ring_pos = 0;
    s_sr.wake_ring_filled = 0;
    s_sr.template_last_check_us = 0;
    s_sr.template_last_wake_us = esp_timer_get_time();
    s_sr.template_phrase_active = false;
    s_sr.template_phrase_start_us = 0;
    s_sr.template_phrase_last_speech_us = 0;
    s_sr.template_phrase_frames = 0;
    return ESP_OK;
}

static void wake_ring_append(const int16_t *samples, uint32_t count)
{
    if (!s_sr.wake_ring || !samples || count == 0) {
        return;
    }
    if (count > VOICE_SR_WAKE_RING_SAMPLES) {
        samples += count - VOICE_SR_WAKE_RING_SAMPLES;
        count = VOICE_SR_WAKE_RING_SAMPLES;
    }
    for (uint32_t i = 0; i < count; ++i) {
        s_sr.wake_ring[s_sr.wake_ring_pos] = samples[i];
        s_sr.wake_ring_pos = (s_sr.wake_ring_pos + 1) % VOICE_SR_WAKE_RING_SAMPLES;
        if (s_sr.wake_ring_filled < VOICE_SR_WAKE_RING_SAMPLES) {
            s_sr.wake_ring_filled++;
        }
    }
}

static bool wake_ring_copy_recent(uint32_t samples, int16_t *out)
{
    if (!s_sr.wake_ring || !out || samples == 0 ||
        samples > VOICE_SR_WAKE_RING_SAMPLES ||
        samples > s_sr.wake_ring_filled) {
        return false;
    }
    uint32_t start = (s_sr.wake_ring_pos + VOICE_SR_WAKE_RING_SAMPLES - samples) %
                     VOICE_SR_WAKE_RING_SAMPLES;
    for (uint32_t i = 0; i < samples; ++i) {
        out[i] = s_sr.wake_ring[(start + i) % VOICE_SR_WAKE_RING_SAMPLES];
    }
    return true;
}

static bool wake_capture_live_template(int64_t phrase_us)
{
    if (!s_sr.template_learning || s_sr.template_learn_count >= s_sr.template_learn_target ||
        !s_sr.template_window || s_sr.wake_ring_filled < VOICE_SR_WAKE_TEMPLATE_MIN_SAMPLES) {
        return false;
    }

    int64_t capture_us = phrase_us + VOICE_SR_WAKE_TEMPLATE_PHRASE_END_US + 300000LL;
    if (capture_us < 900000LL) {
        capture_us = 900000LL;
    }
    uint32_t samples = (uint32_t)((capture_us * (int64_t)VOICE_SR_SAMPLE_RATE) / 1000000LL);
    if (samples > VOICE_SR_WAKE_TEMPLATE_MAX_SAMPLES) {
        samples = VOICE_SR_WAKE_TEMPLATE_MAX_SAMPLES;
    }
    if (samples > s_sr.wake_ring_filled) {
        samples = s_sr.wake_ring_filled;
    }
    if (samples < VOICE_SR_WAKE_TEMPLATE_MIN_SAMPLES ||
        !wake_ring_copy_recent(samples, s_sr.template_window)) {
        return false;
    }

    wake_template_t tmpl = {0};
    uint32_t trimmed = 0;
    if (!wake_extract_features(s_sr.template_window, samples, tmpl.bins, tmpl.zcr_bins, &trimmed)) {
        printf("DG Wake Learn: rejected phrase samples=%u phrase_us=%lld reason=features\r\n",
               (unsigned)samples, (long long)phrase_us);
        return false;
    }

    uint32_t index = s_sr.template_learn_count;
    if (index >= VOICE_SR_WAKE_TEMPLATE_LEARN_MAX) {
        return false;
    }
    tmpl.valid = true;
    tmpl.samples = trimmed;
    snprintf(tmpl.name, sizeof(tmpl.name), "live_%02u", (unsigned)(index + 1U));
    s_sr.learn_templates[index] = tmpl;
    s_sr.template_learn_count++;

    bool finished = s_sr.template_learn_count >= s_sr.template_learn_target;
    printf("DG Wake Learn: captured %u/%u name=%s samples=%u phrase_us=%lld\r\n",
           (unsigned)s_sr.template_learn_count,
           (unsigned)s_sr.template_learn_target,
           tmpl.name,
           (unsigned)tmpl.samples,
           (long long)phrase_us);

    if (finished) {
        uint32_t count = s_sr.template_learn_count;
        esp_err_t err = wake_save_feature_templates(VOICE_SR_WAKE_TEMPLATE_LIVE_FILE,
                                                    s_sr.learn_templates, count);
        if (err == ESP_OK) {
            s_sr.template_learning = false;
            s_sr.template_learn_target = 0;
            s_sr.template_learn_count = 0;
            wake_templates_load();
            set_result(ESP_OK, "template_learning_saved");
            printf("DG Wake Learn: saved count=%u file=%s; device templates active\r\n",
                   (unsigned)count, VOICE_SR_WAKE_TEMPLATE_LIVE_FILE);
        } else {
            set_result(err, "template_learning_save_failed");
            printf("DG Wake Learn: save failed err=%s\r\n", esp_err_to_name(err));
        }
    }
    return true;
}

static void wake_template_rearm(int64_t now_us)
{
    if (s_sr.wake_ring) {
        memset(s_sr.wake_ring, 0, VOICE_SR_WAKE_RING_SAMPLES * sizeof(int16_t));
    }
    s_sr.wake_ring_pos = 0;
    s_sr.wake_ring_filled = 0;
    s_sr.template_last_check_us = 0;
    s_sr.template_last_wake_us = now_us - VOICE_SR_WAKE_TEMPLATE_COOLDOWN_US;
    s_sr.template_phrase_active = false;
    s_sr.template_phrase_start_us = 0;
    s_sr.template_phrase_last_speech_us = 0;
    s_sr.template_phrase_frames = 0;
}

static float clampf_local(float value, float lo, float hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static void wake_template_settings_sanitize(float *threshold, float *confirm_threshold,
                                            uint32_t *min_confirmations)
{
    float threshold_value = threshold ? *threshold : VOICE_SR_WAKE_TEMPLATE_THRESHOLD;
    float confirm_value = confirm_threshold ? *confirm_threshold : VOICE_SR_WAKE_TEMPLATE_CONFIRM_THRESHOLD;
    uint32_t min_value = min_confirmations ? *min_confirmations : VOICE_SR_WAKE_TEMPLATE_MIN_CONFIRMATIONS;

    threshold_value = clampf_local(threshold_value, 0.40f, 0.80f);
    if (confirm_value <= 0.0f) {
        confirm_value = threshold_value - 0.02f;
    }
    confirm_value = clampf_local(confirm_value, 0.35f, threshold_value);
    if (min_value < 1U) {
        min_value = 1U;
    } else if (min_value > 8U) {
        min_value = 8U;
    }

    if (threshold) {
        *threshold = threshold_value;
    }
    if (confirm_threshold) {
        *confirm_threshold = confirm_value;
    }
    if (min_confirmations) {
        *min_confirmations = min_value;
    }
}

static void wake_template_settings_apply_locked(float threshold, float confirm_threshold,
                                                uint32_t min_confirmations)
{
    wake_template_settings_sanitize(&threshold, &confirm_threshold, &min_confirmations);
    s_sr.template_threshold = threshold;
    s_sr.template_confirm_threshold = confirm_threshold;
    s_sr.template_min_confirmations = min_confirmations;
    s_sr.status.template_threshold = threshold;
    s_sr.status.template_confirm_threshold = confirm_threshold;
    s_sr.status.template_min_confirmations = min_confirmations;
}

static uint16_t wake_template_threshold_to_nvs(float value)
{
    value = clampf_local(value, 0.0f, 1.0f);
    return (uint16_t)(value * 1000.0f + 0.5f);
}

static float wake_template_threshold_from_nvs(uint16_t value, float fallback)
{
    if (value < 100U || value > 1000U) {
        return fallback;
    }
    return (float)value / 1000.0f;
}

static void wake_template_settings_load(void)
{
    float threshold = VOICE_SR_WAKE_TEMPLATE_THRESHOLD;
    float confirm_threshold = VOICE_SR_WAKE_TEMPLATE_CONFIRM_THRESHOLD;
    uint32_t min_confirmations = VOICE_SR_WAKE_TEMPLATE_MIN_CONFIRMATIONS;

    nvs_handle_t nvs;
    if (nvs_open(VOICE_SR_WAKE_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint16_t stored_threshold = 0;
        uint16_t stored_confirm = 0;
        uint8_t stored_min = 0;
        if (nvs_get_u16(nvs, VOICE_SR_WAKE_NVS_TEMPLATE_THRESHOLD, &stored_threshold) == ESP_OK) {
            threshold = wake_template_threshold_from_nvs(stored_threshold, threshold);
        }
        if (nvs_get_u16(nvs, VOICE_SR_WAKE_NVS_TEMPLATE_CONFIRM, &stored_confirm) == ESP_OK) {
            confirm_threshold = wake_template_threshold_from_nvs(stored_confirm, confirm_threshold);
        }
        if (nvs_get_u8(nvs, VOICE_SR_WAKE_NVS_TEMPLATE_MIN_CONFIRM, &stored_min) == ESP_OK) {
            min_confirmations = stored_min;
        }
        nvs_close(nvs);
    }

    portENTER_CRITICAL(&s_sr.lock);
    wake_template_settings_apply_locked(threshold, confirm_threshold, min_confirmations);
    portEXIT_CRITICAL(&s_sr.lock);
}

static float wake_feature_score(const float *a, const float *b,
                                const float *zcr_a, const float *zcr_b)
{
    float envelope_score = 0.0f;
    for (uint32_t i = 0; i < VOICE_SR_WAKE_TEMPLATE_BINS; ++i) {
        envelope_score += a[i] * b[i];
    }
    if (!zcr_a || !zcr_b) {
        return envelope_score;
    }

    float zcr_diff = 0.0f;
    for (uint32_t i = 0; i < VOICE_SR_WAKE_TEMPLATE_BINS; ++i) {
        zcr_diff += fabsf(zcr_a[i] - zcr_b[i]);
    }
    zcr_diff /= (float)VOICE_SR_WAKE_TEMPLATE_BINS;
    float zcr_score = 1.0f - clampf_local(zcr_diff / 0.10f, 0.0f, 1.0f);
    return envelope_score * (1.0f - VOICE_SR_WAKE_TEMPLATE_ZCR_WEIGHT) +
           zcr_score * VOICE_SR_WAKE_TEMPLATE_ZCR_WEIGHT;
}

static bool wake_template_match_live(int64_t now_us)
{
    if (!s_sr.template_experiment_enabled || !s_sr.wake_ring || s_sr.status.template_count == 0 ||
        now_us - s_sr.template_last_check_us < VOICE_SR_WAKE_TEMPLATE_CHECK_US ||
        now_us - s_sr.template_last_wake_us < VOICE_SR_WAKE_TEMPLATE_COOLDOWN_US) {
        return false;
    }
    s_sr.template_last_check_us = now_us;

    float template_threshold = VOICE_SR_WAKE_TEMPLATE_THRESHOLD;
    float confirm_threshold = VOICE_SR_WAKE_TEMPLATE_CONFIRM_THRESHOLD;
    uint32_t min_confirmations = VOICE_SR_WAKE_TEMPLATE_MIN_CONFIRMATIONS;
    portENTER_CRITICAL(&s_sr.lock);
    template_threshold = s_sr.template_threshold > 0.0f ? s_sr.template_threshold : VOICE_SR_WAKE_TEMPLATE_THRESHOLD;
    confirm_threshold = s_sr.template_confirm_threshold > 0.0f ?
                        s_sr.template_confirm_threshold : VOICE_SR_WAKE_TEMPLATE_CONFIRM_THRESHOLD;
    min_confirmations = s_sr.template_min_confirmations > 0U ?
                        s_sr.template_min_confirmations : VOICE_SR_WAKE_TEMPLATE_MIN_CONFIRMATIONS;
    portEXIT_CRITICAL(&s_sr.lock);

    int16_t *window = s_sr.template_window;
    if (!window) {
        return false;
    }

    float best = -2.0f;
    float second_best = -2.0f;
    uint32_t confirmations = 0;
    char best_name[24] = {0};
    float live_bins[VOICE_SR_WAKE_TEMPLATE_BINS];
    float live_zcr_bins[VOICE_SR_WAKE_TEMPLATE_BINS];
    uint32_t count = s_sr.status.template_count;
    for (uint32_t i = 0; i < count; ++i) {
        wake_template_t *tmpl = &s_sr.templates[i];
        if (!tmpl->valid || tmpl->samples > s_sr.wake_ring_filled ||
            tmpl->samples < VOICE_SR_WAKE_TEMPLATE_MIN_SAMPLES) {
            continue;
        }
        if (!wake_ring_copy_recent(tmpl->samples, window)) {
            continue;
        }
        if (!wake_extract_features(window, tmpl->samples, live_bins, live_zcr_bins, NULL)) {
            continue;
        }
        float score = wake_feature_score(live_bins, tmpl->bins,
                                         live_zcr_bins, tmpl->zcr_bins);
        if (score >= confirm_threshold) {
            confirmations++;
        }
        if (score > best) {
            second_best = best;
            best = score;
            snprintf(best_name, sizeof(best_name), "%s", tmpl->name);
        } else if (score > second_best) {
            second_best = score;
        }
    }
    bool matched = best >= template_threshold &&
                   confirmations >= min_confirmations;
    portENTER_CRITICAL(&s_sr.lock);
    s_sr.status.template_checks++;
    s_sr.status.template_best_score = best;
    snprintf(s_sr.status.template_best_name, sizeof(s_sr.status.template_best_name), "%s", best_name);
    if (matched) {
        s_sr.status.template_wake_events++;
        s_sr.status.wake_events++;
        s_sr.wake_pending = true;
        set_result_locked(ESP_OK, "template_wake_detected");
    }
    portEXIT_CRITICAL(&s_sr.lock);

    if (matched) {
        s_sr.template_last_wake_us = now_us;
        wake_template_rearm(now_us);
        ESP_LOGI(TAG, "template wake detected score=%.3f second=%.3f confirmations=%lu template=%s",
                 best, second_best, (unsigned long)confirmations, best_name);
        printf("DG Wake: source=template score=%.3f second=%.3f confirmations=%lu template=%s\r\n",
               (double)best, (double)second_best, (unsigned long)confirmations, best_name);
    } else if (best >= VOICE_SR_WAKE_TEMPLATE_MISS_LOG_THRESHOLD && !dg_debug_quiet_enabled()) {
        printf("DG Wake: template miss score=%.3f second=%.3f confirmations=%lu template=%s threshold=%.2f/%u\r\n",
               (double)best,
               (double)second_best,
               (unsigned long)confirmations,
               best_name,
               (double)template_threshold,
               (unsigned)min_confirmations);
    }
    return matched;
}

static void mark_command_window_locked(const char *result)
{
    s_sr.wake_pending = true;
    set_result_locked(ESP_OK, result);
}

static void prepare_command_engine(void)
{
    if (s_sr.multinet && s_sr.mn_data) {
        s_sr.multinet->clean(s_sr.mn_data);
        if (s_sr.afe && s_sr.afe->disable_wakenet) {
            s_sr.afe->disable_wakenet(s_sr.afe_data);
        }
    }
}

static void rearm_wake_detection_after_command(int64_t now_us, const char *reason)
{
    if (s_sr.afe && s_sr.afe_data && s_sr.afe->reset_buffer) {
        (void)s_sr.afe->reset_buffer(s_sr.afe_data);
    }
    wake_template_rearm(now_us);
    printf("DG Wake: template rearmed reason=%s cooldown_ms=%u\r\n",
           reason ? reason : "unknown",
           (unsigned)(VOICE_SR_WAKE_TEMPLATE_COOLDOWN_US / 1000LL));
}

static bool voice_sr_is_paused(void)
{
    int64_t pause_until = 0;
    portENTER_CRITICAL(&s_sr.lock);
    pause_until = s_sr.pause_until_us;
    portEXIT_CRITICAL(&s_sr.lock);
    return pause_until > esp_timer_get_time();
}

static bool voice_sr_assistant_suppressed(void)
{
    bool suppressed = false;
    portENTER_CRITICAL(&s_sr.lock);
    suppressed = s_sr.assistant_suppressed;
    portEXIT_CRITICAL(&s_sr.lock);
    return suppressed;
}

static void voice_sr_feed_task(void *arg)
{
    (void)arg;
    int feed_chunk = s_sr.feed_chunk;
    int16_t *audio_buffer = s_sr.feed_buffer;
    if (!audio_buffer || feed_chunk <= 0) {
        set_result(ESP_ERR_INVALID_STATE, "feed_not_ready");
        vTaskDeleteWithCaps(NULL);
        return;
    }

    portENTER_CRITICAL(&s_sr.lock);
    s_sr.status.feed_task_running = true;
    s_sr.status.feed_chunk = (uint32_t)feed_chunk;
    portEXIT_CRITICAL(&s_sr.lock);

    while (true) {
        if (voice_sr_is_paused()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int64_t feed_start_us = esp_timer_get_time();
        if (s_sr.last_feed_us != 0 && feed_start_us > s_sr.last_feed_us) {
            uint32_t interval = (uint32_t)(feed_start_us - s_sr.last_feed_us);
            portENTER_CRITICAL(&s_sr.lock);
            status_update_max_u32(&s_sr.status.feed_interval_max_us, interval);
            portEXIT_CRITICAL(&s_sr.lock);
        }
        s_sr.last_feed_us = feed_start_us;

        int read_err = esp_codec_dev_read(s_sr.mic, audio_buffer,
                                          feed_chunk * VOICE_SR_MIC_CHANNELS * sizeof(int16_t));
        if (read_err != ESP_CODEC_DEV_OK) {
            portENTER_CRITICAL(&s_sr.lock);
            s_sr.status.read_errors++;
            set_result_locked((esp_err_t)read_err, "mic_read_failed");
            portEXIT_CRITICAL(&s_sr.lock);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        debug_capture_append_raw(audio_buffer, (uint32_t)feed_chunk, feed_start_us);

        uint16_t peak_ch0 = 0;
        uint16_t peak_ch1 = 0;
        for (int i = 0; i < feed_chunk; ++i) {
            int32_t sample_ch0 = audio_buffer[i * VOICE_SR_MIC_CHANNELS];
            int32_t sample_ch1 = audio_buffer[i * VOICE_SR_MIC_CHANNELS + 1];
            uint16_t abs_ch0 = (uint16_t)(sample_ch0 < 0 ? -sample_ch0 : sample_ch0);
            uint16_t abs_ch1 = (uint16_t)(sample_ch1 < 0 ? -sample_ch1 : sample_ch1);
            if (abs_ch0 > peak_ch0) {
                peak_ch0 = abs_ch0;
            }
            if (abs_ch1 > peak_ch1) {
                peak_ch1 = abs_ch1;
            }
        }
        portENTER_CRITICAL(&s_sr.lock);
        s_sr.status.mic_peak_ch0 = peak_ch0;
        s_sr.status.mic_peak_ch1 = peak_ch1;
        portEXIT_CRITICAL(&s_sr.lock);

        for (int i = feed_chunk - 1; i >= 0; --i) {
            audio_buffer[i * VOICE_SR_FEED_CHANNELS + 2] = 0;
            audio_buffer[i * VOICE_SR_FEED_CHANNELS + 1] = audio_buffer[i * VOICE_SR_MIC_CHANNELS + 1];
            audio_buffer[i * VOICE_SR_FEED_CHANNELS + 0] = audio_buffer[i * VOICE_SR_MIC_CHANNELS + 0];
        }
        s_sr.afe->feed(s_sr.afe_data, audio_buffer);

        portENTER_CRITICAL(&s_sr.lock);
        s_sr.status.feed_frames++;
        uint32_t feed_process_us = (uint32_t)(esp_timer_get_time() - feed_start_us);
        status_update_max_u32(&s_sr.status.feed_process_max_us, feed_process_us);
        portEXIT_CRITICAL(&s_sr.lock);
    }
}

static void voice_sr_detect_task(void *arg)
{
    (void)arg;
    int fetch_chunk = s_sr.fetch_chunk;
    bool command_listening = false;
    bool paused_last_loop = false;
    int64_t command_listen_start_us = 0;
    int64_t command_listen_until_us = 0;

    portENTER_CRITICAL(&s_sr.lock);
    s_sr.status.detect_task_running = true;
    s_sr.status.fetch_chunk = (uint32_t)fetch_chunk;
    portEXIT_CRITICAL(&s_sr.lock);

    while (true) {
        if (voice_sr_is_paused()) {
            paused_last_loop = true;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int64_t now_us = esp_timer_get_time();
        if (paused_last_loop) {
            paused_last_loop = false;
            rearm_wake_detection_after_command(now_us, "pause_resume");
        }
        if (s_sr.last_fetch_us != 0 && now_us > s_sr.last_fetch_us) {
            uint32_t interval = (uint32_t)(now_us - s_sr.last_fetch_us);
            portENTER_CRITICAL(&s_sr.lock);
            status_update_max_u32(&s_sr.status.fetch_interval_max_us, interval);
            portEXIT_CRITICAL(&s_sr.lock);
        }
        s_sr.last_fetch_us = now_us;
        if (s_sr.template_experiment_enabled && s_sr.template_expires_us > 0 &&
            now_us > s_sr.template_expires_us) {
            portENTER_CRITICAL(&s_sr.lock);
            s_sr.template_experiment_enabled = false;
            s_sr.status.template_experiment_enabled = false;
            s_sr.template_expires_us = 0;
            portEXIT_CRITICAL(&s_sr.lock);
        }
        afe_fetch_result_t *res = s_sr.afe->fetch(s_sr.afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        bool assistant_suppressed = voice_sr_assistant_suppressed();
        int samples = res->data_size / (int)sizeof(int16_t);
        if (res->data && samples > 0) {
            debug_capture_append_afe(res->data, (uint32_t)samples, now_us);
            command_audio_append_afe(res->data, (uint32_t)samples, now_us);
            audio_monitor_process_shared_samples(res->data, (size_t)samples,
                                                 res->vad_state == AFE_VAD_SPEECH);
        }
        bool online_command_audio_active = false;
        portENTER_CRITICAL(&s_sr.lock);
        online_command_audio_active = s_sr.command_audio_active;
        portEXIT_CRITICAL(&s_sr.lock);
        bool template_detected = false;
        int64_t inference_start_us = esp_timer_get_time();
        if (VOICE_SR_WAKE_TEMPLATE_EXPERIMENT && s_sr.template_experiment_enabled &&
            !assistant_suppressed && !command_listening && !online_command_audio_active &&
            res->data && samples > 0) {
            wake_ring_append(res->data, (uint32_t)samples);
            if (res->vad_state == AFE_VAD_SPEECH) {
                if (!s_sr.template_phrase_active) {
                    s_sr.template_phrase_active = true;
                    s_sr.template_phrase_start_us = now_us;
                    s_sr.template_phrase_frames = 0;
                }
                s_sr.template_phrase_last_speech_us = now_us;
                s_sr.template_phrase_frames++;
                int64_t phrase_us = now_us - s_sr.template_phrase_start_us;
                if (!s_sr.template_learning &&
                    phrase_us >= VOICE_SR_WAKE_TEMPLATE_PHRASE_MIN_US &&
                    phrase_us <= VOICE_SR_WAKE_TEMPLATE_PHRASE_MAX_US &&
                    s_sr.template_phrase_frames >= VOICE_SR_WAKE_TEMPLATE_PHRASE_MIN_FRAMES) {
                    template_detected = wake_template_match_live(now_us);
                } else if (phrase_us > VOICE_SR_WAKE_TEMPLATE_PHRASE_MAX_US) {
                    if (!s_sr.template_learning &&
                        s_sr.template_phrase_frames >= VOICE_SR_WAKE_TEMPLATE_PHRASE_MIN_FRAMES) {
                        template_detected = wake_template_match_live(now_us);
                    }
                    if (!template_detected) {
                        if (!dg_debug_quiet_enabled()) {
                            printf("DG Wake: template phrase reset us=%lld frames=%u reason=long_vad\r\n",
                                   (long long)phrase_us,
                                   (unsigned)s_sr.template_phrase_frames);
                        }
                        s_sr.template_phrase_active = false;
                        s_sr.template_phrase_start_us = 0;
                        s_sr.template_phrase_last_speech_us = 0;
                        s_sr.template_phrase_frames = 0;
                    }
                }
            } else if (s_sr.template_phrase_active &&
                       now_us - s_sr.template_phrase_last_speech_us >=
                       VOICE_SR_WAKE_TEMPLATE_PHRASE_END_US) {
                int64_t phrase_us = s_sr.template_phrase_last_speech_us -
                                     s_sr.template_phrase_start_us;
                bool phrase_length_ok = phrase_us >= VOICE_SR_WAKE_TEMPLATE_PHRASE_MIN_US &&
                                        phrase_us <= VOICE_SR_WAKE_TEMPLATE_PHRASE_MAX_US;
                if (s_sr.template_learning && phrase_length_ok &&
                    s_sr.template_phrase_frames >= VOICE_SR_WAKE_TEMPLATE_PHRASE_MIN_FRAMES) {
                    (void)wake_capture_live_template(phrase_us);
                } else if (phrase_length_ok &&
                    s_sr.template_phrase_frames >= VOICE_SR_WAKE_TEMPLATE_PHRASE_MIN_FRAMES) {
                    template_detected = wake_template_match_live(now_us);
                } else {
                    if (!dg_debug_quiet_enabled()) {
                        printf("DG Wake: template phrase ignored us=%lld frames=%u\r\n",
                               (long long)phrase_us,
                               (unsigned)s_sr.template_phrase_frames);
                    }
                }
                s_sr.template_phrase_active = false;
                s_sr.template_phrase_start_us = 0;
                s_sr.template_phrase_last_speech_us = 0;
                s_sr.template_phrase_frames = 0;
            }
        } else if (s_sr.template_phrase_active) {
            s_sr.template_phrase_active = false;
            s_sr.template_phrase_start_us = 0;
            s_sr.template_phrase_last_speech_us = 0;
            s_sr.template_phrase_frames = 0;
        }

        bool start_command = false;
        int wakenet_state = 0;
        bool wakenet_ignored = false;
        uint32_t active_template_count = 0;
        uint32_t inference_us = (uint32_t)(esp_timer_get_time() - inference_start_us);
        uint32_t fetch_process_us = (uint32_t)(esp_timer_get_time() - now_us);
        float score_for_log = -10.0f;
        float threshold_for_log = 0.0f;
        uint32_t hits_for_log = 0;
        portENTER_CRITICAL(&s_sr.lock);
        s_sr.status.fetch_frames++;
        status_update_max_u32(&s_sr.status.inference_time_max_us, inference_us);
        status_update_max_u32(&s_sr.status.fetch_process_max_us, fetch_process_us);
        if (s_sr.template_experiment_enabled) {
            score_for_log = s_sr.status.template_best_score;
            threshold_for_log = s_sr.template_threshold;
            hits_for_log = template_detected ? s_sr.template_min_confirmations : 0;
        }
        if (!assistant_suppressed &&
            (res->wakeup_state == WAKENET_DETECTED ||
             res->wakeup_state == WAKENET_CHANNEL_VERIFIED)) {
            wakenet_state = (int)res->wakeup_state;
            active_template_count = s_sr.status.template_count;
            s_sr.status.wake_events++;
            mark_command_window_locked(res->wakeup_state == WAKENET_DETECTED ?
                                       "wakenet_detected" : "wakenet_channel_verified");
            if (s_sr.multinet && s_sr.mn_data) {
                start_command = true;
            }
        } else if (template_detected) {
            mark_command_window_locked("template_wake_detected");
            if (s_sr.multinet && s_sr.mn_data) {
                start_command = true;
            }
        }
        portEXIT_CRITICAL(&s_sr.lock);
        debug_capture_append_score(res->data, samples > 0 ? (uint32_t)samples : 0U,
                                   now_us, (int)res->vad_state,
                                   score_for_log, threshold_for_log, hits_for_log,
                                   inference_us);

        if (wakenet_state != 0) {
            if (wakenet_ignored) {
                if (!dg_debug_quiet_enabled()) {
                    printf("DG Wake: source=wakenet state=%d ignored=generic templates=%u\r\n",
                           wakenet_state, (unsigned)active_template_count);
                }
            } else {
                printf("DG Wake: source=wakenet state=%d\r\n", wakenet_state);
            }
        }

        if (assistant_suppressed) {
            if (command_listening) {
                command_listening = false;
                if (s_sr.afe->enable_wakenet) {
                    s_sr.afe->enable_wakenet(s_sr.afe_data);
                }
                rearm_wake_detection_after_command(now_us, "suppressed");
            }
            continue;
        }

        if (start_command) {
            prepare_command_engine();
            command_listening = true;
            command_listen_start_us = now_us + VOICE_SR_COMMAND_LISTEN_DELAY_US;
            command_listen_until_us = now_us + VOICE_SR_COMMAND_WINDOW_US;
            printf("DG Voice: native command window open delay_ms=%u window_ms=%u\r\n",
                   (unsigned)(VOICE_SR_COMMAND_LISTEN_DELAY_US / 1000LL),
                   (unsigned)(VOICE_SR_COMMAND_WINDOW_US / 1000LL));
            continue;
        }

        if (command_listening && s_sr.multinet && s_sr.mn_data) {
            if (now_us > command_listen_until_us) {
                command_listening = false;
                if (s_sr.afe->enable_wakenet) {
                    s_sr.afe->enable_wakenet(s_sr.afe_data);
                }
                rearm_wake_detection_after_command(now_us, "command_timeout");
                portENTER_CRITICAL(&s_sr.lock);
                set_result_locked(ESP_ERR_TIMEOUT, "command_timeout");
                portEXIT_CRITICAL(&s_sr.lock);
                printf("DG Voice: native command timeout\r\n");
                continue;
            }
            if (now_us < command_listen_start_us) {
                continue;
            }

            esp_mn_state_t mn_state = s_sr.multinet->detect(s_sr.mn_data, res->data);
            if (mn_state == ESP_MN_STATE_DETECTING) {
                continue;
            }
            if (mn_state == ESP_MN_STATE_TIMEOUT) {
                command_listening = false;
                if (s_sr.afe->enable_wakenet) {
                    s_sr.afe->enable_wakenet(s_sr.afe_data);
                }
                rearm_wake_detection_after_command(now_us, "multinet_timeout");
                portENTER_CRITICAL(&s_sr.lock);
                set_result_locked(ESP_ERR_TIMEOUT, "multinet_timeout");
                portEXIT_CRITICAL(&s_sr.lock);
                continue;
            }
            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *mn_result = s_sr.multinet->get_results(s_sr.mn_data);
                int command_id = (mn_result && mn_result->num > 0) ? mn_result->command_id[0] : 0;
                voice_sr_command_t command = command_from_mn_id(command_id);
                ESP_LOGI(TAG, "MultiNet command detected id=%d mapped=%d",
                         command_id, (int)command);
                printf("DG Voice: multinet detected id=%d mapped=%d num=%d\r\n",
                       command_id, (int)command, mn_result ? mn_result->num : 0);
                command_listening = false;
                if (s_sr.afe->enable_wakenet) {
                    s_sr.afe->enable_wakenet(s_sr.afe_data);
                }
                rearm_wake_detection_after_command(now_us, "command_detected");
                portENTER_CRITICAL(&s_sr.lock);
                s_sr.status.last_command_id = command_id;
                if (command != VOICE_SR_COMMAND_NONE) {
                    s_sr.command_pending = command;
                    s_sr.status.command_events++;
                    const char *result = "command_detected";
                    switch (command) {
                    case VOICE_SR_COMMAND_SLEEP: result = "command_sleep_detected"; break;
                    case VOICE_SR_COMMAND_STOP: result = "command_stop_detected"; break;
                    case VOICE_SR_COMMAND_LIGHT_ON: result = "command_light_on_detected"; break;
                    case VOICE_SR_COMMAND_LIGHT_OFF: result = "command_light_off_detected"; break;
                    case VOICE_SR_COMMAND_LIGHT_BRIGHTER: result = "command_light_brighter_detected"; break;
                    case VOICE_SR_COMMAND_LIGHT_DIMMER: result = "command_light_dimmer_detected"; break;
                    default: break;
                    }
                    set_result_locked(ESP_OK, result);
                } else {
                    set_result_locked(ESP_ERR_NOT_FOUND, "command_unmapped");
                }
                portEXIT_CRITICAL(&s_sr.lock);
                if (command == VOICE_SR_COMMAND_NONE) {
                    printf("DG Voice: multinet command unmapped id=%d\r\n", command_id);
                }
            }
        }
    }
}

esp_err_t voice_sr_start(void)
{
    if (s_sr.started) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_sr.status, 0, sizeof(s_sr.status));
    s_sr.status.enabled = true;
    set_result(ESP_OK, "starting");
    wake_template_settings_load();

    s_sr.mic = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(s_sr.mic, ESP_ERR_NOT_FOUND, TAG, "microphone codec init failed");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = VOICE_SR_SAMPLE_RATE,
        .channel = VOICE_SR_MIC_CHANNELS,
        .bits_per_sample = VOICE_SR_BITS,
    };
    int open_err = esp_codec_dev_open(s_sr.mic, &fs);
    ESP_RETURN_ON_FALSE(open_err == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "mic open failed err=%d", open_err);
    int gain_err = esp_codec_dev_set_in_gain(s_sr.mic, VOICE_SR_MIC_GAIN_DB);
    printf("DG Voice: microphone codec open err=%d gain_db=%.1f gain_err=%d\r\n",
           open_err, (double)VOICE_SR_MIC_GAIN_DB, gain_err);
    esp_err_t duplex_err = bsp_audio_restart_duplex();
    printf("DG Voice: shared duplex restart err=%s\r\n", esp_err_to_name(duplex_err));
    ESP_RETURN_ON_ERROR(duplex_err, TAG, "shared duplex restart failed");

    s_sr.models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(s_sr.models, ESP_ERR_NOT_FOUND, TAG, "ESP-SR model partition not ready");

    s_sr.afe = &ESP_AFE_SR_HANDLE;
    afe_config_t afe_config = AFE_CONFIG_DEFAULT();
    char *wn_name = select_wakenet_model(s_sr.models);
    ESP_RETURN_ON_FALSE(wn_name, ESP_ERR_NOT_FOUND, TAG, "no WakeNet model selected");
    afe_config.wakenet_model_name = wn_name;
    afe_config.aec_init = false;

    s_sr.afe_data = s_sr.afe->create_from_config(&afe_config);
    ESP_RETURN_ON_FALSE(s_sr.afe_data, ESP_ERR_NO_MEM, TAG, "AFE create failed");

    s_sr.feed_chunk = s_sr.afe->get_feed_chunksize(s_sr.afe_data);
    s_sr.fetch_chunk = s_sr.afe->get_fetch_chunksize(s_sr.afe_data);
    ESP_RETURN_ON_FALSE(s_sr.feed_chunk > 0 && s_sr.fetch_chunk > 0,
                        ESP_ERR_INVALID_STATE, TAG, "invalid AFE chunk size");

    size_t feed_bytes = (size_t)s_sr.feed_chunk * VOICE_SR_FEED_CHANNELS * sizeof(int16_t);
    s_sr.feed_buffer = heap_caps_malloc(feed_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_sr.feed_buffer) {
        s_sr.feed_buffer = heap_caps_malloc(feed_bytes, MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_sr.feed_buffer, ESP_ERR_NO_MEM, TAG, "feed buffer allocation failed");

    if (VOICE_SR_WAKE_TEMPLATE_EXPERIMENT && VOICE_SR_WAKE_TEMPLATE_AUTO_ENABLE) {
        esp_err_t template_err = wake_template_runtime_prepare_locked();
        if (template_err == ESP_OK) {
            wake_templates_load();
            if (s_sr.status.template_count > 0) {
                portENTER_CRITICAL(&s_sr.lock);
                s_sr.template_experiment_enabled = true;
                s_sr.status.template_experiment_enabled = true;
                s_sr.template_expires_us = 0;
                set_result_locked(ESP_OK, "template_auto_enabled");
                portEXIT_CRITICAL(&s_sr.lock);
                ESP_LOGI(TAG, "template wake auto enabled with %lu samples threshold=%.2f",
                         (unsigned long)s_sr.status.template_count,
                         (double)s_sr.template_threshold);
            } else {
                ESP_LOGW(TAG, "template wake auto enable skipped: no wake_xm samples");
            }
        } else {
            ESP_LOGW(TAG, "template wake auto enable skipped: %s", esp_err_to_name(template_err));
        }
    }

    portENTER_CRITICAL(&s_sr.lock);
    s_sr.status.template_threshold = s_sr.template_threshold;
    s_sr.status.template_confirm_threshold = s_sr.template_confirm_threshold;
    s_sr.status.template_min_confirmations = s_sr.template_min_confirmations;
    if (!s_sr.template_experiment_enabled) {
        set_result_locked(ESP_OK, "template_experiment_disabled");
    }
    portEXIT_CRITICAL(&s_sr.lock);

    portENTER_CRITICAL(&s_sr.lock);
    s_sr.status.initialized = true;
    s_sr.status.models_loaded = true;
    s_sr.status.wakenet_loaded = true;
    s_sr.status.afe_created = true;
    s_sr.status.feed_chunk = (uint32_t)s_sr.feed_chunk;
    s_sr.status.fetch_chunk = (uint32_t)s_sr.fetch_chunk;
    snprintf(s_sr.status.wake_model, sizeof(s_sr.status.wake_model), "%s", wn_name);
    set_result_locked(ESP_OK, "ready");
    portEXIT_CRITICAL(&s_sr.lock);

    BaseType_t ok = create_sr_task(voice_sr_feed_task, "voice_sr_feed",
                                   VOICE_SR_FEED_STACK, &s_sr.feed_task, 0);
    if (ok != pdPASS) {
        set_result(ESP_ERR_NO_MEM, "feed_task_create_failed");
        return ESP_ERR_NO_MEM;
    }

    ok = create_sr_task(voice_sr_detect_task, "voice_sr_detect",
                        VOICE_SR_DETECT_STACK, &s_sr.detect_task, 1);
    if (ok != pdPASS) {
        set_result(ESP_ERR_NO_MEM, "detect_task_create_failed");
        return ESP_ERR_NO_MEM;
    }

    s_sr.started = true;
    portENTER_CRITICAL(&s_sr.lock);
    s_sr.status.running = true;
    portEXIT_CRITICAL(&s_sr.lock);

    uint32_t shared_frame_ms = ((uint32_t)s_sr.fetch_chunk * 1000U +
                                VOICE_SR_SAMPLE_RATE - 1U) /
                               VOICE_SR_SAMPLE_RATE;
    esp_err_t monitor_err = audio_monitor_shared_stream_init(
        VOICE_SR_SAMPLE_RATE, (uint16_t)shared_frame_ms);
    if (monitor_err != ESP_OK) {
        ESP_LOGW(TAG, "shared snore monitor unavailable: %s", esp_err_to_name(monitor_err));
    }

    ESP_LOGI(TAG, "ESP-SR WakeNet ready model=%s", wn_name);

    char *mn_name = select_multinet_model(s_sr.models);
    esp_err_t mn_err = voice_sr_multinet_init(mn_name);
    if (mn_err != ESP_OK) {
        ESP_LOGW(TAG, "MultiNet command recognition disabled: %s", esp_err_to_name(mn_err));
    }

    return ESP_OK;
}

void voice_sr_get_status(voice_sr_status_t *status)
{
    if (!status) {
        return;
    }
    portENTER_CRITICAL(&s_sr.lock);
    s_sr.status.template_learning = s_sr.template_learning;
    s_sr.status.template_learn_count = s_sr.template_learn_count;
    s_sr.status.template_learn_target = s_sr.template_learn_target;
    s_sr.status.template_threshold = s_sr.template_threshold;
    s_sr.status.template_confirm_threshold = s_sr.template_confirm_threshold;
    s_sr.status.template_min_confirmations = s_sr.template_min_confirmations;
#if CONFIG_DEBUG_AUDIO_CAPTURE
    s_sr.status.debug_capture_enabled = true;
    s_sr.status.debug_capture_active = s_sr.capture_active;
    s_sr.status.debug_capture_write_pending = s_sr.capture_write_pending;
    s_sr.status.debug_capture_seconds = s_sr.capture_seconds;
    s_sr.status.debug_capture_raw_samples = s_sr.capture_raw_samples;
    s_sr.status.debug_capture_afe_samples = s_sr.capture_afe_samples;
    s_sr.status.debug_capture_score_frames = s_sr.capture_score_frames;
    s_sr.status.debug_capture_raw_drops = s_sr.capture_raw_drops;
    s_sr.status.debug_capture_afe_drops = s_sr.capture_afe_drops;
    s_sr.status.debug_capture_score_drops = s_sr.capture_score_drops;
    snprintf(s_sr.status.debug_capture_label, sizeof(s_sr.status.debug_capture_label),
             "%s", s_sr.capture_label);
    snprintf(s_sr.status.debug_capture_raw_path, sizeof(s_sr.status.debug_capture_raw_path),
             "%s", s_sr.capture_raw_path);
    snprintf(s_sr.status.debug_capture_afe_path, sizeof(s_sr.status.debug_capture_afe_path),
             "%s", s_sr.capture_afe_path);
    snprintf(s_sr.status.debug_capture_score_path, sizeof(s_sr.status.debug_capture_score_path),
             "%s", s_sr.capture_score_path);
#else
    s_sr.status.debug_capture_enabled = false;
#endif
    s_sr.status.command_audio_active = s_sr.command_audio_active;
    s_sr.status.command_audio_ready = s_sr.command_audio_ready;
    s_sr.status.command_audio_seconds = s_sr.command_audio_seconds;
    s_sr.status.command_audio_samples = s_sr.command_audio_samples;
    s_sr.status.command_audio_drops = s_sr.command_audio_drops;
    *status = s_sr.status;
    portEXIT_CRITICAL(&s_sr.lock);
}

bool voice_sr_take_wake_request(void)
{
    bool pending = false;
    portENTER_CRITICAL(&s_sr.lock);
    pending = s_sr.wake_pending;
    s_sr.wake_pending = false;
    portEXIT_CRITICAL(&s_sr.lock);
    return pending;
}

bool voice_sr_take_command(voice_sr_command_t *command)
{
    voice_sr_command_t pending = VOICE_SR_COMMAND_NONE;
    portENTER_CRITICAL(&s_sr.lock);
    pending = s_sr.command_pending;
    s_sr.command_pending = VOICE_SR_COMMAND_NONE;
    portEXIT_CRITICAL(&s_sr.lock);
    if (command) {
        *command = pending;
    }
    return pending != VOICE_SR_COMMAND_NONE;
}

void voice_sr_pause(uint32_t duration_ms)
{
    if (duration_ms == 0) {
        portENTER_CRITICAL(&s_sr.lock);
        s_sr.pause_until_us = 0;
        portEXIT_CRITICAL(&s_sr.lock);
        return;
    }
    int64_t until = esp_timer_get_time() + (int64_t)duration_ms * 1000LL;
    portENTER_CRITICAL(&s_sr.lock);
    if (until > s_sr.pause_until_us) {
        s_sr.pause_until_us = until;
    }
    portEXIT_CRITICAL(&s_sr.lock);
}

void voice_sr_set_assistant_suppressed(bool suppressed)
{
    portENTER_CRITICAL(&s_sr.lock);
    s_sr.assistant_suppressed = suppressed;
    s_sr.status.assistant_suppressed = suppressed;
    if (suppressed) {
        s_sr.wake_pending = false;
        s_sr.command_pending = VOICE_SR_COMMAND_NONE;
        set_result_locked(ESP_OK, "assistant_suppressed");
    } else {
        set_result_locked(ESP_OK, "assistant_resumed");
    }
    portEXIT_CRITICAL(&s_sr.lock);
}

esp_err_t voice_sr_template_experiment_enable(bool enabled, uint32_t duration_sec)
{
    if (!VOICE_SR_WAKE_TEMPLATE_EXPERIMENT) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_sr.started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!enabled) {
        portENTER_CRITICAL(&s_sr.lock);
        s_sr.template_experiment_enabled = false;
        s_sr.status.template_experiment_enabled = false;
        s_sr.template_expires_us = 0;
        set_result_locked(ESP_OK, "template_experiment_disabled");
        portEXIT_CRITICAL(&s_sr.lock);
        return ESP_OK;
    }

    if (duration_sec == 0 || duration_sec > 300) {
        duration_sec = 120;
    }

    esp_err_t err = wake_template_runtime_prepare_locked();
    if (err != ESP_OK) {
        set_result(err, "template_alloc_failed");
        return err;
    }
    if (s_sr.status.template_count == 0) {
        wake_templates_load();
    }
    if (s_sr.status.template_count == 0) {
        set_result(ESP_ERR_NOT_FOUND, "template_samples_not_found");
        return ESP_ERR_NOT_FOUND;
    }

    portENTER_CRITICAL(&s_sr.lock);
    s_sr.template_experiment_enabled = true;
    s_sr.status.template_experiment_enabled = true;
    s_sr.status.template_best_score = 0.0f;
    s_sr.status.template_checks = 0;
    s_sr.template_expires_us = esp_timer_get_time() + (int64_t)duration_sec * 1000000LL;
    set_result_locked(ESP_OK, "template_experiment_enabled");
    portEXIT_CRITICAL(&s_sr.lock);
    ESP_LOGI(TAG, "template wake experiment enabled for %lu sec", (unsigned long)duration_sec);
    return ESP_OK;
}

esp_err_t voice_sr_template_match_configure(float threshold, float confirm_threshold,
                                            uint32_t min_confirmations)
{
    wake_template_settings_sanitize(&threshold, &confirm_threshold, &min_confirmations);

    nvs_handle_t nvs = 0;
    bool nvs_opened = false;
    esp_err_t err = nvs_open(VOICE_SR_WAKE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_opened = true;
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, VOICE_SR_WAKE_NVS_TEMPLATE_THRESHOLD,
                          wake_template_threshold_to_nvs(threshold));
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, VOICE_SR_WAKE_NVS_TEMPLATE_CONFIRM,
                          wake_template_threshold_to_nvs(confirm_threshold));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, VOICE_SR_WAKE_NVS_TEMPLATE_MIN_CONFIRM,
                         (uint8_t)min_confirmations);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_INITIALIZED || err == ESP_ERR_NVS_NOT_FOUND) {
        portENTER_CRITICAL(&s_sr.lock);
        wake_template_settings_apply_locked(threshold, confirm_threshold, min_confirmations);
        set_result_locked(ESP_OK, "template_match_configured");
        portEXIT_CRITICAL(&s_sr.lock);
        printf("DG Wake: template config threshold=%.3f confirm=%.3f min=%u saved=%s\r\n",
               (double)threshold,
               (double)confirm_threshold,
               (unsigned)min_confirmations,
               err == ESP_OK ? "yes" : "no");
    }
    if (err == ESP_OK) {
        if (nvs_opened) {
            nvs_close(nvs);
        }
        return ESP_OK;
    }
    if (nvs_opened) {
        nvs_close(nvs);
    }
    return err;
}

esp_err_t voice_sr_template_learning_start(uint32_t target_count)
{
    if (!s_sr.started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_sr.template_experiment_enabled || !s_sr.wake_ring || !s_sr.template_window) {
        return ESP_ERR_INVALID_STATE;
    }
    if (target_count == 0) {
        target_count = 8;
    }
    if (target_count > VOICE_SR_WAKE_TEMPLATE_LEARN_MAX) {
        target_count = VOICE_SR_WAKE_TEMPLATE_LEARN_MAX;
    }

    memset(s_sr.learn_templates, 0, sizeof(s_sr.learn_templates));
    portENTER_CRITICAL(&s_sr.lock);
    s_sr.template_learning = true;
    s_sr.template_learn_target = target_count;
    s_sr.template_learn_count = 0;
    s_sr.status.template_learning = true;
    s_sr.status.template_learn_target = target_count;
    s_sr.status.template_learn_count = 0;
    s_sr.wake_pending = false;
    s_sr.command_pending = VOICE_SR_COMMAND_NONE;
    set_result_locked(ESP_OK, "template_learning_started");
    portEXIT_CRITICAL(&s_sr.lock);

    wake_template_rearm(esp_timer_get_time());
    printf("DG Wake Learn: started target=%u. Say hi xiaomeng %u times near the device.\r\n",
           (unsigned)target_count, (unsigned)target_count);
    return ESP_OK;
}

esp_err_t voice_sr_debug_capture_start(const char *label, uint32_t seconds)
{
#if CONFIG_DEBUG_AUDIO_CAPTURE
    if (!s_sr.started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (seconds == 0) {
        seconds = VOICE_SR_DEBUG_CAPTURE_DEFAULT_SEC;
    }
    if (seconds > VOICE_SR_DEBUG_CAPTURE_MAX_SEC) {
        seconds = VOICE_SR_DEBUG_CAPTURE_MAX_SEC;
    }

    portENTER_CRITICAL(&s_sr.lock);
    bool busy = s_sr.capture_active || s_sr.capture_write_pending;
    portEXIT_CRITICAL(&s_sr.lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t limit_samples = VOICE_SR_SAMPLE_RATE * seconds;
    uint32_t score_limit = seconds * 200U;
    if (!s_sr.capture_raw || s_sr.capture_limit_samples < limit_samples) {
        free(s_sr.capture_raw);
        s_sr.capture_raw = heap_caps_malloc(limit_samples * sizeof(int16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_sr.capture_raw) {
            s_sr.capture_raw = heap_caps_malloc(limit_samples * sizeof(int16_t), MALLOC_CAP_8BIT);
        }
    }
    if (!s_sr.capture_afe || s_sr.capture_limit_samples < limit_samples) {
        free(s_sr.capture_afe);
        s_sr.capture_afe = heap_caps_malloc(limit_samples * sizeof(int16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_sr.capture_afe) {
            s_sr.capture_afe = heap_caps_malloc(limit_samples * sizeof(int16_t), MALLOC_CAP_8BIT);
        }
    }
    if (!s_sr.capture_scores || s_sr.capture_score_limit < score_limit) {
        free(s_sr.capture_scores);
        s_sr.capture_scores = heap_caps_malloc(score_limit * sizeof(debug_capture_score_record_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_sr.capture_scores) {
            s_sr.capture_scores = heap_caps_malloc(score_limit * sizeof(debug_capture_score_record_t),
                                                   MALLOC_CAP_8BIT);
        }
    }
    if (!s_sr.capture_raw || !s_sr.capture_afe || !s_sr.capture_scores) {
        set_result(ESP_ERR_NO_MEM, "debug_capture_alloc_failed");
        printf("REC FAILED label=%s err=%s reason=alloc\r\n",
               label ? label : "", esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }

    char safe_label[56];
    debug_capture_sanitize_label(label, safe_label, sizeof(safe_label));
    memset(s_sr.capture_raw, 0, limit_samples * sizeof(int16_t));
    memset(s_sr.capture_afe, 0, limit_samples * sizeof(int16_t));
    memset(s_sr.capture_scores, 0, score_limit * sizeof(debug_capture_score_record_t));
    bool quiet_was_enabled = dg_debug_quiet_enabled();
    dg_debug_quiet_set(true);

    portENTER_CRITICAL(&s_sr.lock);
    s_sr.capture_limit_samples = limit_samples;
    s_sr.capture_score_limit = score_limit;
    s_sr.capture_raw_samples = 0;
    s_sr.capture_afe_samples = 0;
    s_sr.capture_score_frames = 0;
    s_sr.capture_raw_drops = 0;
    s_sr.capture_afe_drops = 0;
    s_sr.capture_score_drops = 0;
    s_sr.capture_seconds = seconds;
    s_sr.capture_active = true;
    s_sr.capture_write_pending = false;
    s_sr.capture_quiet_was_enabled = quiet_was_enabled;
    s_sr.capture_end_us = esp_timer_get_time() + (int64_t)seconds * 1000000LL;
    snprintf(s_sr.capture_label, sizeof(s_sr.capture_label), "%s", safe_label);
    snprintf(s_sr.capture_raw_path, sizeof(s_sr.capture_raw_path),
             "/spiffs/capture_%s_raw.wav", safe_label);
    snprintf(s_sr.capture_afe_path, sizeof(s_sr.capture_afe_path),
             "/spiffs/capture_%s_afe.wav", safe_label);
    snprintf(s_sr.capture_score_path, sizeof(s_sr.capture_score_path),
             "/spiffs/capture_%s_score.csv", safe_label);
    s_sr.status.debug_capture_enabled = true;
    s_sr.status.debug_capture_active = true;
    s_sr.status.debug_capture_write_pending = false;
    s_sr.status.debug_capture_seconds = seconds;
    s_sr.status.debug_capture_raw_samples = 0;
    s_sr.status.debug_capture_afe_samples = 0;
    s_sr.status.debug_capture_score_frames = 0;
    s_sr.status.debug_capture_raw_drops = 0;
    s_sr.status.debug_capture_afe_drops = 0;
    s_sr.status.debug_capture_score_drops = 0;
    snprintf(s_sr.status.debug_capture_label, sizeof(s_sr.status.debug_capture_label),
             "%s", s_sr.capture_label);
    snprintf(s_sr.status.debug_capture_raw_path, sizeof(s_sr.status.debug_capture_raw_path),
             "%s", s_sr.capture_raw_path);
    snprintf(s_sr.status.debug_capture_afe_path, sizeof(s_sr.status.debug_capture_afe_path),
             "%s", s_sr.capture_afe_path);
    snprintf(s_sr.status.debug_capture_score_path, sizeof(s_sr.status.debug_capture_score_path),
             "%s", s_sr.capture_score_path);
    set_result_locked(ESP_OK, "debug_capture_started");
    portEXIT_CRITICAL(&s_sr.lock);

    printf("REC START label=%s seconds=%u\r\n", safe_label, (unsigned)seconds);
    printf("REC SPEAK_NOW label=%s phrase=hi_xiaomeng\r\n", safe_label);
    return ESP_OK;
#else
    (void)label;
    (void)seconds;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t voice_sr_command_audio_start(uint32_t seconds)
{
    if (!s_sr.started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (seconds == 0) {
        seconds = VOICE_SR_COMMAND_AUDIO_DEFAULT_SEC;
    }
    if (seconds > VOICE_SR_COMMAND_AUDIO_MAX_SEC) {
        seconds = VOICE_SR_COMMAND_AUDIO_MAX_SEC;
    }

    portENTER_CRITICAL(&s_sr.lock);
    bool busy = s_sr.command_audio_active || s_sr.command_audio_ready;
    portEXIT_CRITICAL(&s_sr.lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t limit_samples = VOICE_SR_SAMPLE_RATE * seconds;
    if (!s_sr.command_audio || s_sr.command_audio_limit_samples < limit_samples) {
        free(s_sr.command_audio);
        s_sr.command_audio = heap_caps_malloc(limit_samples * sizeof(int16_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_sr.command_audio) {
            s_sr.command_audio = heap_caps_malloc(limit_samples * sizeof(int16_t), MALLOC_CAP_8BIT);
        }
        if (!s_sr.command_audio) {
            set_result(ESP_ERR_NO_MEM, "command_audio_alloc_failed");
            return ESP_ERR_NO_MEM;
        }
        s_sr.command_audio_limit_samples = limit_samples;
    }

    memset(s_sr.command_audio, 0, limit_samples * sizeof(int16_t));
    portENTER_CRITICAL(&s_sr.lock);
    s_sr.command_audio_samples = 0;
    s_sr.command_audio_drops = 0;
    s_sr.command_audio_seconds = seconds;
    s_sr.command_audio_end_us = esp_timer_get_time() + (int64_t)seconds * 1000000LL;
    s_sr.command_audio_active = true;
    s_sr.command_audio_ready = false;
    s_sr.status.command_audio_active = true;
    s_sr.status.command_audio_ready = false;
    s_sr.status.command_audio_seconds = seconds;
    s_sr.status.command_audio_samples = 0;
    s_sr.status.command_audio_drops = 0;
    set_result_locked(ESP_OK, "command_audio_started");
    portEXIT_CRITICAL(&s_sr.lock);

    printf("DG ASR: listen start seconds=%u\r\n", (unsigned)seconds);
    printf("DG ASR: speak command now\r\n");
    return ESP_OK;
}

void voice_sr_command_audio_cancel(void)
{
    bool was_active = false;
    portENTER_CRITICAL(&s_sr.lock);
    was_active = s_sr.command_audio_active || s_sr.command_audio_ready;
    s_sr.command_audio_active = false;
    s_sr.command_audio_ready = false;
    s_sr.command_audio_samples = 0;
    s_sr.command_audio_drops = 0;
    s_sr.command_audio_end_us = 0;
    s_sr.status.command_audio_active = false;
    s_sr.status.command_audio_ready = false;
    s_sr.status.command_audio_samples = 0;
    s_sr.status.command_audio_drops = 0;
    if (was_active) {
        set_result_locked(ESP_OK, "command_audio_cancelled");
    }
    portEXIT_CRITICAL(&s_sr.lock);
    if (was_active) {
        printf("DG ASR: capture cancelled by native command\r\n");
    }
}

bool voice_sr_take_command_audio(int16_t **samples, uint32_t *sample_count)
{
    if (!samples || !sample_count) {
        return false;
    }
    *samples = NULL;
    *sample_count = 0;

    portENTER_CRITICAL(&s_sr.lock);
    if (!s_sr.command_audio_ready || !s_sr.command_audio || s_sr.command_audio_samples == 0) {
        portEXIT_CRITICAL(&s_sr.lock);
        return false;
    }
    *samples = s_sr.command_audio;
    *sample_count = s_sr.command_audio_samples;
    s_sr.command_audio = NULL;
    s_sr.command_audio_limit_samples = 0;
    s_sr.command_audio_samples = 0;
    s_sr.command_audio_ready = false;
    s_sr.status.command_audio_ready = false;
    s_sr.status.command_audio_samples = 0;
    portEXIT_CRITICAL(&s_sr.lock);
    return true;
}
