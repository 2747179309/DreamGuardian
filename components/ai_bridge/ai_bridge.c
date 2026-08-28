#include "ai_bridge.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define AI_NVS_NS "dg_ai"
#define AI_NVS_ENABLED "enabled"
#define AI_NVS_WEBHOOK "webhook"
#define AI_NVS_INTERVAL "interval"
#define AI_NVS_PENDING "pending"
#define AI_QUEUE_DEPTH 2
#define AI_TEXT_MAX 1024
#define AI_WORKER_STACK 5120
#define AI_WORKER_STACK_FALLBACK 4608
#define AI_FIRST_SEND_DELAY_MS 1000U
#define AI_RETRY_MIN_MS 5000U
#define AI_RETRY_MAX_MS 60000U
#define AI_OUTBOX_POLL_MS 5000U

static const char *TAG = "ai_bridge";

typedef struct {
    char text[AI_TEXT_MAX];
    bool persistent;
    uint8_t retry_count;
} ai_event_t;

typedef struct {
    ai_bridge_config_t cfg;
    ai_bridge_status_t status;
    QueueHandle_t queue;
    TaskHandle_t worker;
    int64_t last_send_us;
    sleep_assist_state_t last_assist_state;
    bool last_presence;
    bool risk_latched;
    portMUX_TYPE lock;
} ai_ctx_t;

static ai_ctx_t s_ai = {
    .last_assist_state = SLEEP_ASSIST_OFF,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

void ai_bridge_get_default_config(ai_bridge_config_t *config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->enabled = false;
    config->min_interval_sec = 180;
}

static void copy_cfg(ai_bridge_config_t *dst, const ai_bridge_config_t *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->enabled = src->enabled;
    dst->min_interval_sec = src->min_interval_sec ? src->min_interval_sec : 180;
    snprintf(dst->feishu_webhook_url, sizeof(dst->feishu_webhook_url), "%s", src->feishu_webhook_url);
}

static esp_err_t ensure_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t load_cfg(ai_bridge_config_t *cfg)
{
    ai_bridge_get_default_config(cfg);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(AI_NVS_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    uint8_t enabled = 0;
    uint32_t interval = 0;
    size_t url_len = sizeof(cfg->feishu_webhook_url);
    if (nvs_get_u8(nvs, AI_NVS_ENABLED, &enabled) == ESP_OK) cfg->enabled = enabled != 0;
    if (nvs_get_u32(nvs, AI_NVS_INTERVAL, &interval) == ESP_OK && interval >= 30) cfg->min_interval_sec = interval;
    (void)nvs_get_str(nvs, AI_NVS_WEBHOOK, cfg->feishu_webhook_url, &url_len);
    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t save_cfg(const ai_bridge_config_t *cfg)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(AI_NVS_NS, NVS_READWRITE, &nvs), TAG, "open nvs");
    esp_err_t err = nvs_set_u8(nvs, AI_NVS_ENABLED, cfg->enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u32(nvs, AI_NVS_INTERVAL, cfg->min_interval_sec);
    if (err == ESP_OK) err = nvs_set_str(nvs, AI_NVS_WEBHOOK, cfg->feishu_webhook_url);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t save_pending_text(const char *text)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(AI_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, AI_NVS_PENDING, text);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t load_pending_text(char *text, size_t text_size)
{
    if (!text || text_size == 0) return ESP_ERR_INVALID_ARG;
    text[0] = '\0';
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(AI_NVS_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    size_t required = text_size;
    err = nvs_get_str(nvs, AI_NVS_PENDING, text, &required);
    nvs_close(nvs);
    return err;
}

static esp_err_t clear_pending_text_if_match(const char *sent_text)
{
    char pending[AI_TEXT_MAX];
    esp_err_t err = load_pending_text(pending, sizeof(pending));
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (strcmp(pending, sent_text) != 0) return ESP_OK;

    nvs_handle_t nvs;
    err = nvs_open(AI_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(nvs, AI_NVS_PENDING);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static bool ready(const ai_bridge_config_t *cfg)
{
    return cfg && cfg->enabled && cfg->feishu_webhook_url[0] != '\0';
}

static void set_result(const char *text, int http_status)
{
    portENTER_CRITICAL(&s_ai.lock);
    snprintf(s_ai.status.last_result, sizeof(s_ai.status.last_result), "%s", text ? text : "");
    s_ai.status.last_http_status = http_status;
    portEXIT_CRITICAL(&s_ai.lock);
}

static esp_err_t feishu_post_text(const ai_bridge_config_t *cfg, const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *content = cJSON_CreateObject();
    if (!root || !content) {
        cJSON_Delete(root);
        cJSON_Delete(content);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "msg_type", "text");
    cJSON_AddStringToObject(content, "text", text);
    cJSON_AddItemToObject(root, "content", content);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    esp_http_client_config_t http_cfg = {
        .url = cfg->feishu_webhook_url,
        .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .addr_type = HTTP_ADDR_TYPE_INET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        cJSON_free(body);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int tls_error = 0;
    int tls_flags = 0;
    (void)esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
    esp_http_client_cleanup(client);
    cJSON_free(body);

    if (err == ESP_OK && status >= 200 && status < 300) {
        printf("DG FeishuWebhook: sent http=%d internal=%u largest=%u\r\n",
               status,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        set_result("sent", status);
        return ESP_OK;
    }
    char msg[AI_BRIDGE_RESULT_MAX];
    snprintf(msg, sizeof(msg), "send_failed err=%s status=%d tls=%d flags=0x%x",
             esp_err_to_name(err), status, tls_error, tls_flags);
    printf("DG FeishuWebhook: failed err=%s http=%d tls=%d flags=0x%x internal=%u largest=%u\r\n",
           esp_err_to_name(err), status, tls_error, tls_flags,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    set_result(msg, status);
    return err == ESP_OK ? ESP_FAIL : err;
}

static void wait_before_persistent_retry(ai_event_t *event)
{
    uint8_t shift = event->retry_count < 4 ? event->retry_count : 4;
    uint32_t delay_ms = AI_RETRY_MIN_MS << shift;
    if (delay_ms > AI_RETRY_MAX_MS) delay_ms = AI_RETRY_MAX_MS;
    if (event->retry_count < UINT8_MAX) event->retry_count++;
    ESP_LOGW(TAG, "persistent report retry in %" PRIu32 " ms (attempt=%u)",
             delay_ms, (unsigned)event->retry_count);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static void worker_task(void *arg)
{
    (void)arg;
    ai_event_t *event = heap_caps_calloc(1, sizeof(*event), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *pending = heap_caps_malloc(AI_TEXT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!event || !pending) {
        free(event);
        free(pending);
        printf("DG FeishuWebhook: worker PSRAM buffer allocation failed\r\n");
        portENTER_CRITICAL(&s_ai.lock);
        s_ai.status.worker_running = false;
        s_ai.worker = NULL;
        portEXIT_CRITICAL(&s_ai.lock);
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        /* Always inspect the NVS outbox before ordinary RAM telemetry. This
         * prevents frequent status reports from starving a sleep milestone
         * when the queue was full during a hotspot outage. */
        memset(event, 0, sizeof(*event));
        event->persistent = true;
        esp_err_t outbox_err = load_pending_text(event->text, sizeof(event->text));
        if (outbox_err == ESP_OK && event->text[0]) {
            ESP_LOGW(TAG, "delivering persisted report with priority");
        } else {
            memset(event, 0, sizeof(*event));
            if (xQueueReceive(s_ai.queue, event, pdMS_TO_TICKS(AI_OUTBOX_POLL_MS)) != pdTRUE) {
                continue;
            }
        }
        if (event->persistent) {
            memset(pending, 0, AI_TEXT_MAX);
            esp_err_t pending_err = load_pending_text(pending, AI_TEXT_MAX);
            if (pending_err == ESP_ERR_NVS_NOT_FOUND) {
                /* A duplicate queue item whose NVS record was already delivered. */
                continue;
            }
            if (pending_err == ESP_OK && strcmp(pending, event->text) != 0) {
                snprintf(event->text, sizeof(event->text), "%s", pending);
                event->retry_count = 0;
            }
        }
        while (true) {
            ai_bridge_config_t cfg;
            ai_bridge_get_config(&cfg);
            if (!ready(&cfg)) {
                set_result("disabled_or_not_configured", 0);
                if (event->persistent) {
                    wait_before_persistent_retry(event);
                    continue;
                }
                break;
            }
            if (event->persistent && event->retry_count == 0) {
                /* Let sleep-mode audio/UI shutdown finish before TLS allocates memory. */
                vTaskDelay(pdMS_TO_TICKS(AI_FIRST_SEND_DELAY_MS));
            }
            esp_err_t err = feishu_post_text(&cfg, event->text);
            portENTER_CRITICAL(&s_ai.lock);
            if (err == ESP_OK) {
                s_ai.status.sent_count++;
            } else if (!event->persistent) {
                s_ai.status.dropped_count++;
            }
            portEXIT_CRITICAL(&s_ai.lock);
            if (err == ESP_OK && event->persistent) {
                esp_err_t clear_err = clear_pending_text_if_match(event->text);
                if (clear_err != ESP_OK) {
                    ESP_LOGW(TAG, "report sent but pending record clear failed: %s",
                             esp_err_to_name(clear_err));
                } else {
                    ESP_LOGI(TAG, "persistent sleep report sent");
                }
            }
            if (err == ESP_OK || !event->persistent) {
                break;
            }
            wait_before_persistent_retry(event);
        }
    }
}

static esp_err_t enqueue_text(const char *text, bool force, int64_t now_us)
{
    ai_bridge_config_t cfg;
    ai_bridge_get_config(&cfg);
    if (!ready(&cfg) || !s_ai.queue) {
        set_result("disabled_or_not_configured", 0);
        return ESP_ERR_INVALID_STATE;
    }
    if (!force && s_ai.last_send_us > 0 &&
        now_us - s_ai.last_send_us < (int64_t)cfg.min_interval_sec * 1000000LL) {
        return ESP_ERR_INVALID_STATE;
    }
    ai_event_t event = {0};
    snprintf(event.text, sizeof(event.text), "%s", text ? text : "");
    if (xQueueSend(s_ai.queue, &event, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_ai.lock);
        s_ai.status.dropped_count++;
        portEXIT_CRITICAL(&s_ai.lock);
        set_result("queue_full", 0);
        return ESP_ERR_NO_MEM;
    }
    if (!force) s_ai.last_send_us = now_us;
    portENTER_CRITICAL(&s_ai.lock);
    s_ai.status.queued_count++;
    portEXIT_CRITICAL(&s_ai.lock);
    set_result("queued", 0);
    return ESP_OK;
}

esp_err_t ai_bridge_init(const ai_bridge_config_t *config)
{
    ESP_RETURN_ON_ERROR(ensure_nvs(), TAG, "nvs");
    ai_bridge_config_t cfg;
    if (load_cfg(&cfg) != ESP_OK) ai_bridge_get_default_config(&cfg);
    if (config) copy_cfg(&cfg, config);

    portENTER_CRITICAL(&s_ai.lock);
    copy_cfg(&s_ai.cfg, &cfg);
    s_ai.status.enabled = s_ai.cfg.enabled;
    s_ai.status.configured = s_ai.cfg.feishu_webhook_url[0] != '\0';
    portEXIT_CRITICAL(&s_ai.lock);

    if (!s_ai.queue) {
        s_ai.queue = xQueueCreate(AI_QUEUE_DEPTH, sizeof(ai_event_t));
        if (!s_ai.queue) return ESP_ERR_NO_MEM;
    }
    if (!s_ai.worker) {
        ai_event_t pending = {
            .persistent = true,
        };
        esp_err_t pending_err = load_pending_text(pending.text, sizeof(pending.text));
        if (pending_err == ESP_OK && pending.text[0] != '\0') {
            if (xQueueSend(s_ai.queue, &pending, 0) == pdTRUE) {
                portENTER_CRITICAL(&s_ai.lock);
                s_ai.status.queued_count++;
                portEXIT_CRITICAL(&s_ai.lock);
                ESP_LOGW(TAG, "replaying persisted sleep report after restart");
            }
        } else if (pending_err != ESP_ERR_NVS_NOT_FOUND && pending_err != ESP_OK) {
            ESP_LOGW(TAG, "load pending report failed: %s", esp_err_to_name(pending_err));
        }
    }
    if (!s_ai.worker) {
        BaseType_t created = xTaskCreate(worker_task, "ai_bridge", AI_WORKER_STACK,
                                         NULL, 4, &s_ai.worker);
        uint32_t stack_bytes = AI_WORKER_STACK;
        if (created != pdPASS) {
            printf("DG FeishuWebhook: worker stack=%u create failed, retrying stack=%u\r\n",
                   (unsigned)AI_WORKER_STACK, (unsigned)AI_WORKER_STACK_FALLBACK);
            created = xTaskCreate(worker_task, "ai_bridge", AI_WORKER_STACK_FALLBACK,
                                  NULL, 4, &s_ai.worker);
            stack_bytes = AI_WORKER_STACK_FALLBACK;
        }
        if (created != pdPASS) {
            printf("DG FeishuWebhook: worker create failed internal=%u largest=%u\r\n",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_ai.lock);
        s_ai.status.worker_running = true;
        portEXIT_CRITICAL(&s_ai.lock);
        printf("DG FeishuWebhook: worker created stack=%u\r\n", (unsigned)stack_bytes);
    }
    ESP_LOGI(TAG, "ready enabled=%d configured=%d", s_ai.cfg.enabled, s_ai.cfg.feishu_webhook_url[0] != '\0');
    printf("DG FeishuWebhook: ready enabled=%d configured=%d pending_queue=%d\r\n",
           s_ai.cfg.enabled ? 1 : 0,
           s_ai.cfg.feishu_webhook_url[0] != '\0' ? 1 : 0,
           s_ai.queue ? 1 : 0);
    return ESP_OK;
}

esp_err_t ai_bridge_get_config(ai_bridge_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_ai.lock);
    copy_cfg(config, &s_ai.cfg);
    portEXIT_CRITICAL(&s_ai.lock);
    return ESP_OK;
}

esp_err_t ai_bridge_set_config(const ai_bridge_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    ai_bridge_config_t cfg;
    copy_cfg(&cfg, config);
    if (cfg.min_interval_sec < 30) cfg.min_interval_sec = 30;
    if (cfg.feishu_webhook_url[0] &&
        strncmp(cfg.feishu_webhook_url, "http://", 7) != 0 &&
        strncmp(cfg.feishu_webhook_url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(save_cfg(&cfg), TAG, "save");
    portENTER_CRITICAL(&s_ai.lock);
    copy_cfg(&s_ai.cfg, &cfg);
    s_ai.status.enabled = cfg.enabled;
    s_ai.status.configured = s_ai.cfg.feishu_webhook_url[0] != '\0';
    portEXIT_CRITICAL(&s_ai.lock);
    return ESP_OK;
}

void ai_bridge_get_status(ai_bridge_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_ai.lock);
    *status = s_ai.status;
    status->enabled = s_ai.cfg.enabled;
    status->configured = s_ai.cfg.feishu_webhook_url[0] != '\0';
    portEXIT_CRITICAL(&s_ai.lock);
}

static void format_message(char *out, size_t size, const char *title, const ai_bridge_sample_t *s)
{
    snprintf(out, size,
             "DreamGuardian Go\n%s\nPresence: %s\nSleep state: %s\nSleep score: %u\nWake risk: %u\nBreath: %.1f bpm\nHeart: %.1f bpm\nMotion: %.2f\nStability: %.2f\nAssist: %s\nAction: %s\nUptime: %" PRIu32 "s",
             title,
             s->presence ? "yes" : "no",
             sleep_state_to_name(s->sleep_state),
             s->sleep_score,
             s->wake_risk,
             (double)s->breath_bpm,
             (double)s->heart_bpm,
             (double)s->motion,
             (double)s->stability,
             sleep_assist_state_name(s->assist_state),
             s->action ? s->action : "-",
             s->uptime_sec);
}

void ai_bridge_update(const ai_bridge_sample_t *sample, int64_t now_us)
{
    if (!sample) return;
    const char *title = NULL;
    bool force = false;
    bool intervention_reported = sample->action &&
        (strstr(sample->action, "pink_noise") ||
         strstr(sample->action, "stereo_breathing") ||
         strstr(sample->action, "reduce_arousal") ||
         strstr(sample->action, "return_weak") ||
         strstr(sample->action, "night_path"));

    /* Sleep-session start/onset/finish reports are emitted explicitly by the
     * application.  Only remember this state here so generic telemetry cannot
     * duplicate those three user-visible group messages. */
    if (sample->assist_state != s_ai.last_assist_state) {
        if (sample->assist_state == SLEEP_ASSIST_ABORT) {
            title = "Sleep assist aborted by abnormal condition.";
            force = true;
        }
        s_ai.last_assist_state = sample->assist_state;
    }

    if (!title && !intervention_reported && sample->presence &&
        sample->wake_risk >= 75 && !s_ai.risk_latched) {
        title = "Wake risk is high; keep a low-stimulation environment.";
        s_ai.risk_latched = true;
    }
    if (sample->wake_risk < 55) s_ai.risk_latched = false;

    if (!title && !intervention_reported && !sample->presence && s_ai.last_presence) {
        title = "Presence lost or radar has no valid human target.";
    }
    s_ai.last_presence = sample->presence;

    if (title) {
        char msg[AI_TEXT_MAX];
        format_message(msg, sizeof(msg), title, sample);
        (void)enqueue_text(msg, force, now_us);
    }
}

esp_err_t ai_bridge_send_test(const char *text)
{
    char msg[AI_TEXT_MAX];
    snprintf(msg, sizeof(msg), "DreamGuardian Go\n%s",
             (text && text[0]) ? text : "Feishu event upload test succeeded.");

    return ai_bridge_send_text(msg);
}

esp_err_t ai_bridge_send_text(const char *text)
{
    char msg[AI_TEXT_MAX];
    snprintf(msg, sizeof(msg), "%s", (text && text[0]) ? text : "DreamGuardian Go");

    ai_bridge_config_t cfg;
    ai_bridge_get_config(&cfg);
    if (!ready(&cfg)) {
        set_result("disabled_or_not_configured", 0);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = feishu_post_text(&cfg, msg);
    portENTER_CRITICAL(&s_ai.lock);
    if (err == ESP_OK) {
        s_ai.status.sent_count++;
    } else {
        s_ai.status.dropped_count++;
    }
    portEXIT_CRITICAL(&s_ai.lock);
    return err;
}

esp_err_t ai_bridge_send_queued_text(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    return enqueue_text(text, true, 0);
}

esp_err_t ai_bridge_send_reliable_text(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;

    if (!s_ai.queue) {
        set_result("worker_not_initialized", 0);
        return ESP_ERR_INVALID_STATE;
    }

    /* This API is called from app_main's relatively small task stack. Keep the
     * 1 KiB event payload in PSRAM; a stack-local event combined with sleep
     * report formatting and NVS calls can corrupt the main task stack. */
    ai_event_t *event = heap_caps_calloc(1, sizeof(*event),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!event) {
        set_result("reliable_event_alloc_failed", 0);
        return ESP_ERR_NO_MEM;
    }
    event->persistent = true;
    snprintf(event->text, sizeof(event->text), "%s", text);
    esp_err_t err = save_pending_text(event->text);
    if (err != ESP_OK) {
        free(event);
        set_result("persist_failed", 0);
        return err;
    }

    BaseType_t queued = xQueueSendToFront(s_ai.queue, event, pdMS_TO_TICKS(1000));
    free(event);
    if (queued != pdTRUE) {
        /* The NVS record is intentionally retained for replay after a restart. */
        set_result("persisted_queue_full", 0);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_ai.lock);
    s_ai.status.queued_count++;
    portEXIT_CRITICAL(&s_ai.lock);
    set_result("persisted_queued", 0);
    return ESP_OK;
}
