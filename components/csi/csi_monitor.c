#include "csi_monitor.h"

#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CSI_ENV_SAMPLES 300
#define CSI_SAMPLE_PERIOD_MS 200

static const char *TAG = "csi_monitor";

typedef struct {
    bool enabled;
    uint32_t packet_count;
    int8_t last_rssi;
    float amplitude_ema;
    float motion_ema;
    float stability_score;
    float breath_proxy_bpm;
    bool breath_proxy_valid;
    float env[CSI_ENV_SAMPLES];
    uint16_t env_index;
    uint16_t env_count;
    TaskHandle_t task;
    portMUX_TYPE lock;
} csi_monitor_ctx_t;

static csi_monitor_ctx_t s_csi = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static float clampf_local(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void csi_rx_cb(void *ctx, wifi_csi_info_t *data)
{
    (void)ctx;
    if (!data || !data->buf || data->len < 4) {
        return;
    }

    uint16_t start = data->first_word_invalid && data->len > 4 ? 4 : 0;
    uint32_t pairs = 0;
    float sum_amp = 0.0f;
    for (uint16_t i = start; i + 1 < data->len; i += 2) {
        float im = (float)data->buf[i];
        float re = (float)data->buf[i + 1];
        sum_amp += sqrtf(re * re + im * im);
        pairs++;
    }
    if (pairs == 0) {
        return;
    }

    float amp = sum_amp / (float)pairs;
    portENTER_CRITICAL(&s_csi.lock);
    if (s_csi.packet_count == 0) {
        s_csi.amplitude_ema = amp;
    }
    float delta = fabsf(amp - s_csi.amplitude_ema);
    s_csi.amplitude_ema = s_csi.amplitude_ema * 0.94f + amp * 0.06f;
    s_csi.motion_ema = s_csi.motion_ema * 0.92f + delta * 0.08f;
    s_csi.packet_count++;
    s_csi.last_rssi = data->rx_ctrl.rssi;
    portEXIT_CRITICAL(&s_csi.lock);
}

static void estimate_breath_proxy_locked(void)
{
    if (s_csi.env_count < 90) {
        s_csi.breath_proxy_valid = false;
        s_csi.breath_proxy_bpm = 0.0f;
        s_csi.stability_score = 0.0f;
        return;
    }

    uint16_t count = s_csi.env_count;
    float mean = 0.0f;
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t idx = (s_csi.env_index + CSI_ENV_SAMPLES - count + i) % CSI_ENV_SAMPLES;
        mean += s_csi.env[idx];
    }
    mean /= (float)count;

    float variance = 0.0f;
    uint16_t upward_crossings = 0;
    float prev = 0.0f;
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t idx = (s_csi.env_index + CSI_ENV_SAMPLES - count + i) % CSI_ENV_SAMPLES;
        float centered = s_csi.env[idx] - mean;
        variance += centered * centered;
        if (i > 0 && prev < 0.0f && centered >= 0.0f) {
            upward_crossings++;
        }
        prev = centered;
    }
    variance /= (float)count;

    float window_sec = (float)count * ((float)CSI_SAMPLE_PERIOD_MS / 1000.0f);
    float bpm = ((float)upward_crossings * 60.0f) / window_sec;
    bool valid = bpm >= 6.0f && bpm <= 30.0f && variance > 0.01f;
    s_csi.breath_proxy_valid = valid;
    s_csi.breath_proxy_bpm = valid ? bpm : 0.0f;

    float motion_norm = clampf_local(s_csi.motion_ema / 20.0f, 0.0f, 1.0f);
    float packet_quality = s_csi.packet_count > 50 ? 1.0f : (float)s_csi.packet_count / 50.0f;
    s_csi.stability_score = clampf_local((1.0f - motion_norm) * packet_quality, 0.0f, 1.0f);
}

static void csi_task(void *arg)
{
    (void)arg;
    while (true) {
        portENTER_CRITICAL(&s_csi.lock);
        s_csi.env[s_csi.env_index] = s_csi.amplitude_ema;
        s_csi.env_index = (s_csi.env_index + 1) % CSI_ENV_SAMPLES;
        if (s_csi.env_count < CSI_ENV_SAMPLES) {
            s_csi.env_count++;
        }
        estimate_breath_proxy_locked();
        portEXIT_CRITICAL(&s_csi.lock);
        vTaskDelay(pdMS_TO_TICKS(CSI_SAMPLE_PERIOD_MS));
    }
}

esp_err_t csi_monitor_start(void)
{
    if (s_csi.enabled) {
        return ESP_OK;
    }

    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = false,
        .shift = 0,
        .dump_ack_en = false,
    };

    ESP_RETURN_ON_ERROR(esp_wifi_set_csi_rx_cb(csi_rx_cb, NULL), TAG, "set CSI callback");
    ESP_RETURN_ON_ERROR(esp_wifi_set_csi_config(&csi_config), TAG, "set CSI config");
    ESP_RETURN_ON_ERROR(esp_wifi_set_csi(true), TAG, "enable CSI");

    s_csi.enabled = true;
    if (!s_csi.task) {
        xTaskCreate(csi_task, "csi_monitor", 3072, NULL, 4, &s_csi.task);
    }
    ESP_LOGI(TAG, "CSI monitor enabled without promiscuous mode; use router/phone traffic to feed CSI packets");
    return ESP_OK;
}

void csi_monitor_reset(void)
{
    portENTER_CRITICAL(&s_csi.lock);
    uint32_t packets = s_csi.packet_count;
    memset(s_csi.env, 0, sizeof(s_csi.env));
    s_csi.packet_count = 0;
    s_csi.last_rssi = 0;
    s_csi.amplitude_ema = 0.0f;
    s_csi.motion_ema = 0.0f;
    s_csi.stability_score = 0.0f;
    s_csi.breath_proxy_bpm = 0.0f;
    s_csi.breath_proxy_valid = false;
    s_csi.env_index = 0;
    s_csi.env_count = 0;
    (void)packets;
    portEXIT_CRITICAL(&s_csi.lock);
}

void csi_monitor_get_status(csi_monitor_status_t *status)
{
    if (!status) {
        return;
    }
    portENTER_CRITICAL(&s_csi.lock);
    status->enabled = s_csi.enabled;
    status->breath_proxy_valid = s_csi.breath_proxy_valid;
    status->packet_count = s_csi.packet_count;
    status->last_rssi = s_csi.last_rssi;
    status->amplitude_ema = s_csi.amplitude_ema;
    status->motion_index = clampf_local(s_csi.motion_ema / 20.0f, 0.0f, 1.0f);
    status->stability_score = s_csi.stability_score;
    status->breath_proxy_bpm = s_csi.breath_proxy_bpm;
    portEXIT_CRITICAL(&s_csi.lock);
}