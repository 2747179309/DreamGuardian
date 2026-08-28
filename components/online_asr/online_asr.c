#include "online_asr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"

#define ASR_NVS_NAMESPACE "dg_claw"
#define ASR_NVS_ENABLED "asr_enabled"
#define ASR_NVS_MODEL "asr_model"
#define ASR_NVS_BASE_URL "asr_base_url"
#define ASR_NVS_API_KEY "asr_api_key"
#define ASR_NVS_LANGUAGE "asr_language"
#define ASR_NVS_TIMEOUT_MS "asr_timeout_ms"
#define ASR_NVS_LLM_API_KEY "api_key"

#define ASR_SAMPLE_RATE 16000U
#define ASR_BITS 16U
#define ASR_MODEL_MAX 64
#define ASR_URL_MAX 256
#define ASR_API_KEY_MAX 256
#define ASR_LANG_MAX 12
#define ASR_LABEL_MAX 40
#define ASR_RESPONSE_MAX 2048
#define ASR_QUEUE_DEPTH 1
#define ASR_WORKER_STACK 9216
#define ASR_MAX_SECONDS 8U
#define ASR_MAX_SAMPLES (ASR_SAMPLE_RATE * ASR_MAX_SECONDS)

static const char *TAG = "online_asr";

typedef struct {
    bool enabled;
    char model[ASR_MODEL_MAX];
    char base_url[ASR_URL_MAX];
    char api_key[ASR_API_KEY_MAX];
    char language[ASR_LANG_MAX];
    uint32_t timeout_ms;
} asr_config_t;

typedef struct {
    int16_t *samples;
    uint32_t sample_count;
    char label[ASR_LABEL_MAX];
} asr_job_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

typedef struct {
    bool qwen_json;
    size_t pcm_len;
    size_t wav_len;
    size_t b64_len;
    const char *prefix;
    size_t prefix_len;
    const char *suffix;
    size_t suffix_len;
} qwen_body_plan_t;

typedef struct {
    QueueHandle_t queue;
    TaskHandle_t worker;
    online_asr_status_t status;
    bool transcript_ready;
    char pending_text[ONLINE_ASR_TEXT_MAX];
    portMUX_TYPE lock;
} asr_ctx_t;

static asr_ctx_t s_asr = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static esp_err_t ensure_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs");
        err = nvs_flash_init();
    }
    return err;
}

static void nvs_get_str_default(nvs_handle_t nvs, const char *key,
                                char *out, size_t out_size, const char *fallback)
{
    if (!out || out_size == 0) return;
    size_t len = out_size;
    if (nvs_get_str(nvs, key, out, &len) != ESP_OK) {
        snprintf(out, out_size, "%s", fallback ? fallback : "");
    }
}

static void set_status(esp_err_t err, int http_status, const char *result)
{
    portENTER_CRITICAL(&s_asr.lock);
    s_asr.status.last_error = err;
    s_asr.status.last_http_status = http_status;
    snprintf(s_asr.status.last_result, sizeof(s_asr.status.last_result), "%s",
             result ? result : esp_err_to_name(err));
    portEXIT_CRITICAL(&s_asr.lock);
}

static void print_heap_line(const char *prefix)
{
    printf("%s heap=%u internal=%u internal_largest=%u psram=%u psram_largest=%u\r\n",
           prefix ? prefix : "DG ASR:",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static esp_err_t load_config(asr_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = true;
    snprintf(cfg->model, sizeof(cfg->model), "%s", "gpt-4o-mini-transcribe");
    snprintf(cfg->base_url, sizeof(cfg->base_url), "%s", "https://api.openai.com/v1");
    snprintf(cfg->language, sizeof(cfg->language), "%s", "zh");
    cfg->timeout_ms = 30000;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ASR_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t enabled = 1;
        if (nvs_get_u8(nvs, ASR_NVS_ENABLED, &enabled) == ESP_OK) {
            cfg->enabled = enabled != 0;
        }
        nvs_get_str_default(nvs, ASR_NVS_MODEL, cfg->model, sizeof(cfg->model), cfg->model);
        nvs_get_str_default(nvs, ASR_NVS_BASE_URL, cfg->base_url, sizeof(cfg->base_url), cfg->base_url);
        nvs_get_str_default(nvs, ASR_NVS_API_KEY, cfg->api_key, sizeof(cfg->api_key), "");
        if (!cfg->api_key[0]) {
            nvs_get_str_default(nvs, ASR_NVS_LLM_API_KEY, cfg->api_key, sizeof(cfg->api_key), "");
        }
        nvs_get_str_default(nvs, ASR_NVS_LANGUAGE, cfg->language, sizeof(cfg->language), cfg->language);
        uint32_t timeout_ms = 0;
        if (nvs_get_u32(nvs, ASR_NVS_TIMEOUT_MS, &timeout_ms) == ESP_OK && timeout_ms >= 5000) {
            cfg->timeout_ms = timeout_ms;
        }
        nvs_close(nvs);
    }

    bool configured = cfg->enabled && cfg->api_key[0] && cfg->model[0] && cfg->base_url[0];
    portENTER_CRITICAL(&s_asr.lock);
    s_asr.status.enabled = cfg->enabled;
    s_asr.status.configured = configured;
    snprintf(s_asr.status.model, sizeof(s_asr.status.model), "%s", cfg->model);
    snprintf(s_asr.status.base_url, sizeof(s_asr.status.base_url), "%s", cfg->base_url);
    portEXIT_CRITICAL(&s_asr.lock);
    return configured ? ESP_OK : ESP_ERR_INVALID_STATE;
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

static void wav_header(uint8_t *header, uint32_t sample_count)
{
    uint32_t data_bytes = sample_count * sizeof(int16_t);
    memset(header, 0, 44);
    memcpy(header, "RIFF", 4);
    write_le32(header + 4, 36U + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    write_le32(header + 16, 16);
    write_le16(header + 20, 1);
    write_le16(header + 22, 1);
    write_le32(header + 24, ASR_SAMPLE_RATE);
    write_le32(header + 28, ASR_SAMPLE_RATE * sizeof(int16_t));
    write_le16(header + 32, sizeof(int16_t));
    write_le16(header + 34, ASR_BITS);
    memcpy(header + 36, "data", 4);
    write_le32(header + 40, data_bytes);
}

static void join_api_path(char *out, size_t out_size, const char *base_url, const char *suffix)
{
    const char *base = base_url ? base_url : "";
    size_t len = strlen(base);
    if (strstr(base, suffix)) {
        snprintf(out, out_size, "%s", base);
    } else if (len > 0 && base[len - 1] == '/') {
        snprintf(out, out_size, "%s%s", base, suffix[0] == '/' ? suffix + 1 : suffix);
    } else {
        snprintf(out, out_size, "%s%s", base, suffix);
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !resp || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (resp->len + (size_t)evt->data_len + 1 > ASR_RESPONSE_MAX) {
        return ESP_FAIL;
    }
    if (resp->len + (size_t)evt->data_len + 1 > resp->cap) {
        size_t new_cap = resp->cap ? resp->cap * 2 : 512;
        while (new_cap < resp->len + (size_t)evt->data_len + 1) {
            new_cap *= 2;
        }
        if (new_cap > ASR_RESPONSE_MAX) {
            new_cap = ASR_RESPONSE_MAX;
        }
        char *new_buf = realloc(resp->buf, new_cap);
        if (!new_buf) return ESP_FAIL;
        resp->buf = new_buf;
        resp->cap = new_cap;
    }
    memcpy(resp->buf + resp->len, evt->data, evt->data_len);
    resp->len += (size_t)evt->data_len;
    resp->buf[resp->len] = '\0';
    return ESP_OK;
}

static esp_err_t resp_append(http_resp_t *resp, const char *data, size_t len)
{
    if (!resp || !data || len == 0) return ESP_OK;
    if (resp->len + len + 1 > ASR_RESPONSE_MAX) {
        return ESP_FAIL;
    }
    if (resp->len + len + 1 > resp->cap) {
        size_t new_cap = resp->cap ? resp->cap * 2 : 512;
        while (new_cap < resp->len + len + 1) {
            new_cap *= 2;
        }
        if (new_cap > ASR_RESPONSE_MAX) {
            new_cap = ASR_RESPONSE_MAX;
        }
        char *new_buf = realloc(resp->buf, new_cap);
        if (!new_buf) return ESP_FAIL;
        resp->buf = new_buf;
        resp->cap = new_cap;
    }
    memcpy(resp->buf + resp->len, data, len);
    resp->len += len;
    resp->buf[resp->len] = '\0';
    return ESP_OK;
}

static esp_err_t build_multipart_body(const asr_config_t *cfg, const int16_t *samples,
                                      uint32_t sample_count, uint8_t **out_body,
                                      size_t *out_len, const char **out_content_type)
{
    static const char boundary[] = "----DreamGuardianAsrBoundary7MA4YWxkTrZu0gW";
    static char content_type[96];
    char prefix[768];
    char suffix[96];
    uint8_t header[44];

    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    snprintf(prefix, sizeof(prefix),
             "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n%s\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"command.wav\"\r\n"
             "Content-Type: audio/wav\r\n\r\n",
             boundary, cfg->model, boundary, cfg->language, boundary, boundary);
    snprintf(suffix, sizeof(suffix), "\r\n--%s--\r\n", boundary);
    wav_header(header, sample_count);

    size_t prefix_len = strlen(prefix);
    size_t suffix_len = strlen(suffix);
    size_t pcm_len = sample_count * sizeof(int16_t);
    size_t total = prefix_len + sizeof(header) + pcm_len + suffix_len;
    const char *alloc_kind = "psram";
    uint8_t *body = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        alloc_kind = "8bit";
        body = heap_caps_malloc(total, MALLOC_CAP_8BIT);
    }
    if (!body) return ESP_ERR_NO_MEM;
    printf("DG ASR: multipart bytes=%u alloc=%s\r\n", (unsigned)total, alloc_kind);

    size_t pos = 0;
    memcpy(body + pos, prefix, prefix_len);
    pos += prefix_len;
    memcpy(body + pos, header, sizeof(header));
    pos += sizeof(header);
    memcpy(body + pos, samples, pcm_len);
    pos += pcm_len;
    memcpy(body + pos, suffix, suffix_len);

    *out_body = body;
    *out_len = total;
    *out_content_type = content_type;
    return ESP_OK;
}

static esp_err_t parse_text_response(const char *response, char *out, size_t out_size)
{
    if (!response || !out || out_size == 0) return ESP_ERR_INVALID_ARG;
    if (strncmp(response, "data:", 5) == 0) {
        const char *p = response;
        while ((p = strstr(p, "data:")) != NULL) {
            p += 5;
            while (*p == ' ') p++;
            const char *line_end = strstr(p, "\n");
            size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
            while (line_len > 0 && (p[line_len - 1] == '\r' || p[line_len - 1] == '\n')) {
                line_len--;
            }
            if (line_len >= 6 && strncmp(p, "[DONE]", 6) == 0) {
                break;
            }
            char *line = heap_caps_malloc(line_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!line) line = heap_caps_malloc(line_len + 1, MALLOC_CAP_8BIT);
            if (!line) return ESP_ERR_NO_MEM;
            memcpy(line, p, line_len);
            line[line_len] = '\0';
            esp_err_t line_err = parse_text_response(line, out, out_size);
            free(line);
            if (line_err == ESP_OK) {
                return ESP_OK;
            }
            if (!line_end) break;
            p = line_end + 1;
        }
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(response);
    if (!root) return ESP_FAIL;

    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
        snprintf(out, out_size, "%s", text->valuestring);
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON *output = cJSON_GetObjectItem(root, "output");
    text = output ? cJSON_GetObjectItem(output, "text") : NULL;
    if (cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
        snprintf(out, out_size, "%s", text->valuestring);
        cJSON_Delete(root);
        return ESP_OK;
    }
    cJSON *nested_output = output ? cJSON_GetObjectItem(output, "output") : NULL;
    cJSON *sentence = nested_output ? cJSON_GetObjectItem(nested_output, "sentence") : NULL;
    text = sentence ? cJSON_GetObjectItem(sentence, "text") : NULL;
    if (cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
        snprintf(out, out_size, "%s", text->valuestring);
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice ? cJSON_GetObjectItem(choice, "message") : NULL;
    text = message ? cJSON_GetObjectItem(message, "content") : NULL;
    if (cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
        snprintf(out, out_size, "%s", text->valuestring);
        cJSON_Delete(root);
        return ESP_OK;
    }
    if (cJSON_IsArray(text)) {
        cJSON *first = cJSON_GetArrayItem(text, 0);
        cJSON *array_text = first ? cJSON_GetObjectItem(first, "text") : NULL;
        if (cJSON_IsString(array_text) && array_text->valuestring && array_text->valuestring[0]) {
            snprintf(out, out_size, "%s", array_text->valuestring);
            cJSON_Delete(root);
            return ESP_OK;
        }
    }

    if (output) {
        choices = cJSON_GetObjectItem(output, "choices");
        choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        message = choice ? cJSON_GetObjectItem(choice, "message") : NULL;
        text = message ? cJSON_GetObjectItem(message, "content") : NULL;
        if (cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
            snprintf(out, out_size, "%s", text->valuestring);
            cJSON_Delete(root);
            return ESP_OK;
        }
        if (cJSON_IsArray(text)) {
            cJSON *first = cJSON_GetArrayItem(text, 0);
            cJSON *array_text = first ? cJSON_GetObjectItem(first, "text") : NULL;
            if (cJSON_IsString(array_text) && array_text->valuestring && array_text->valuestring[0]) {
                snprintf(out, out_size, "%s", array_text->valuestring);
                cJSON_Delete(root);
                return ESP_OK;
            }
        }
    }

    cJSON_Delete(root);
    return ESP_FAIL;
}

static void print_response_preview(const char *response)
{
    if (!response) {
        printf("DG ASR: response empty\r\n");
        return;
    }
    size_t len = strlen(response);
    size_t n = len < 360 ? len : 360;
    char preview[361];
    memcpy(preview, response, n);
    preview[n] = '\0';
    for (size_t i = 0; i < n; ++i) {
        if (preview[i] == '\r' || preview[i] == '\n') preview[i] = ' ';
    }
    printf("DG ASR: response len=%u preview=\"%s%s\"\r\n",
           (unsigned)len, preview, len > n ? "..." : "");
}

static bool response_has_empty_message_content(const char *response)
{
    if (!response) return false;
    cJSON *root = cJSON_Parse(response);
    if (!root) return strstr(response, "\"content\":\"\"") != NULL ||
                       strstr(response, "\"content\": \"\"") != NULL;
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice ? cJSON_GetObjectItem(choice, "message") : NULL;
    cJSON *content = message ? cJSON_GetObjectItem(message, "content") : NULL;
    bool empty = cJSON_IsString(content) && content->valuestring && content->valuestring[0] == '\0';
    cJSON_Delete(root);
    return empty;
}

static bool model_is_qwen_asr(const char *model)
{
    return model && (strstr(model, "qwen") != NULL || strstr(model, "Qwen") != NULL) &&
           strstr(model, "asr") != NULL;
}

static int http_write_all(esp_http_client_handle_t client, const char *data, size_t len)
{
    size_t written_total = 0;
    while (written_total < len) {
        int written = esp_http_client_write(client, data + written_total, len - written_total);
        if (written <= 0) {
            return written;
        }
        written_total += (size_t)written;
    }
    return (int)written_total;
}

static esp_err_t write_qwen_b64_chunk(esp_http_client_handle_t client,
                                      const uint8_t *data, size_t len)
{
    if (len == 0) return ESP_OK;
    size_t b64_cap = ((len + 2) / 3) * 4 + 1;
    uint8_t *b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b64) b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_8BIT);
    if (!b64) return ESP_ERR_NO_MEM;
    size_t b64_len = 0;
    int ret = mbedtls_base64_encode(b64, b64_cap, &b64_len, data, len);
    if (ret != 0) {
        free(b64);
        return ESP_FAIL;
    }
    int written = http_write_all(client, (const char *)b64, b64_len);
    free(b64);
    return written == (int)b64_len ? ESP_OK : ESP_FAIL;
}

static esp_err_t write_qwen_wav_base64(esp_http_client_handle_t client,
                                       const int16_t *samples, uint32_t sample_count)
{
    uint8_t header[44];
    wav_header(header, sample_count);

    uint8_t carry[3];
    size_t carry_len = 0;
    const uint8_t *parts[2] = { header, (const uint8_t *)samples };
    size_t part_lens[2] = { sizeof(header), sample_count * sizeof(int16_t) };

    for (size_t part = 0; part < 2; ++part) {
        const uint8_t *p = parts[part];
        size_t len = part_lens[part];
        if (carry_len > 0) {
            while (carry_len < 3 && len > 0) {
                carry[carry_len++] = *p++;
                len--;
            }
            if (carry_len == 3) {
                ESP_RETURN_ON_ERROR(write_qwen_b64_chunk(client, carry, 3), TAG, "b64 carry");
                carry_len = 0;
            }
        }

        const size_t chunk_bytes = 3072;
        while (len >= 3) {
            size_t chunk = len > chunk_bytes ? chunk_bytes : len;
            chunk -= chunk % 3;
            ESP_RETURN_ON_ERROR(write_qwen_b64_chunk(client, p, chunk), TAG, "b64 chunk");
            p += chunk;
            len -= chunk;
        }
        while (len > 0) {
            carry[carry_len++] = *p++;
            len--;
        }
    }

    if (carry_len > 0) {
        ESP_RETURN_ON_ERROR(write_qwen_b64_chunk(client, carry, carry_len), TAG, "b64 tail");
    }
    return ESP_OK;
}

static esp_err_t prepare_qwen_body_plan(const asr_config_t *cfg, uint32_t sample_count,
                                        qwen_body_plan_t *plan, char *prefix, size_t prefix_size)
{
    if (!cfg || !plan || !prefix || prefix_size == 0) return ESP_ERR_INVALID_ARG;
    memset(plan, 0, sizeof(*plan));
    plan->qwen_json = true;
    plan->pcm_len = sample_count * sizeof(int16_t);
    plan->wav_len = sizeof(uint8_t) * 44U + plan->pcm_len;
    plan->b64_len = ((plan->wav_len + 2U) / 3U) * 4U;
    int written = snprintf(prefix, prefix_size,
        "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:audio/wav;base64,",
        cfg->model);
    if (written <= 0 || (size_t)written >= prefix_size) return ESP_ERR_NO_MEM;
    plan->prefix = prefix;
    plan->prefix_len = (size_t)written;
    plan->suffix = "\"}}]}],\"stream\":false,\"asr_options\":{\"language\":\"zh\",\"enable_itn\":false}}";
    plan->suffix_len = strlen(plan->suffix);
    return ESP_OK;
}

static esp_err_t transcribe_pcm(const asr_config_t *cfg, const int16_t *samples,
                                uint32_t sample_count, char *out_text, size_t out_text_size,
                                int *out_http_status)
{
    if (!cfg || !samples || sample_count == 0 || !out_text) return ESP_ERR_INVALID_ARG;
    if (sample_count > ASR_MAX_SAMPLES) return ESP_ERR_INVALID_SIZE;

    char url[ASR_URL_MAX + 32];
    join_api_path(url, sizeof(url), cfg->base_url,
                  model_is_qwen_asr(cfg->model) ? "/chat/completions" : "/audio/transcriptions");

    uint8_t *body = NULL;
    size_t body_len = 0;
    const char *content_type = NULL;
    qwen_body_plan_t qwen_plan = {0};
    char qwen_prefix[192];
    if (model_is_qwen_asr(cfg->model)) {
        ESP_RETURN_ON_ERROR(prepare_qwen_body_plan(cfg, sample_count, &qwen_plan,
                                                   qwen_prefix, sizeof(qwen_prefix)),
                            TAG, "qwen plan");
        body_len = qwen_plan.prefix_len + qwen_plan.b64_len + qwen_plan.suffix_len;
        content_type = "application/json; charset=utf-8";
        printf("DG ASR: qwen stream bytes=%u b64=%u\r\n",
               (unsigned)body_len, (unsigned)qwen_plan.b64_len);
    } else {
        ESP_RETURN_ON_ERROR(build_multipart_body(cfg, samples, sample_count, &body, &body_len, &content_type),
                            TAG, "multipart");
    }

    http_resp_t resp = {0};
    esp_http_client_config_t http_cfg = {
        .url = url,
        .event_handler = qwen_plan.qwen_json ? NULL : http_event_handler,
        .user_data = qwen_plan.qwen_json ? NULL : &resp,
        .timeout_ms = (int)cfg->timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .addr_type = HTTP_ADDR_TYPE_INET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        if (body) free(body);
        return ESP_ERR_NO_MEM;
    }
    char auth[ASR_API_KEY_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = ESP_OK;
    if (qwen_plan.qwen_json) {
        err = esp_http_client_open(client, (int)body_len);
        if (err == ESP_OK) {
            int written = http_write_all(client, qwen_plan.prefix, qwen_plan.prefix_len);
            if (written != (int)qwen_plan.prefix_len) {
                err = ESP_FAIL;
            }
        }
        if (err == ESP_OK) {
            err = write_qwen_wav_base64(client, samples, sample_count);
        }
        if (err == ESP_OK) {
            int written = http_write_all(client, qwen_plan.suffix, qwen_plan.suffix_len);
            if (written != (int)qwen_plan.suffix_len) {
                err = ESP_FAIL;
            }
        }
        if (err == ESP_OK) {
            (void)esp_http_client_fetch_headers(client);
            char rx[256];
            while (true) {
                int r = esp_http_client_read(client, rx, sizeof(rx));
                if (r < 0) {
                    err = ESP_FAIL;
                    break;
                }
                if (r == 0) {
                    break;
                }
                err = resp_append(&resp, rx, (size_t)r);
                if (err != ESP_OK) {
                    break;
                }
            }
        }
    } else {
        esp_http_client_set_post_field(client, (const char *)body, (int)body_len);
        err = esp_http_client_perform(client);
    }
    int status = esp_http_client_get_status_code(client);
    int tls_error = 0;
    int tls_flags = 0;
    (void)esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
    esp_http_client_cleanup(client);
    if (body) free(body);
    if (out_http_status) *out_http_status = status;

    if (err != ESP_OK) {
        char msg[ONLINE_ASR_RESULT_MAX];
        snprintf(msg, sizeof(msg), "http_failed err=%s tls=%d flags=0x%x",
                 esp_err_to_name(err), tls_error, tls_flags);
        set_status(err, status, msg);
        free(resp.buf);
        return err;
    }
    if (status < 200 || status >= 300) {
        char msg[ONLINE_ASR_RESULT_MAX];
        snprintf(msg, sizeof(msg), "http_status=%d", status);
        set_status(ESP_FAIL, status, msg);
        free(resp.buf);
        return ESP_FAIL;
    }

    err = parse_text_response(resp.buf, out_text, out_text_size);
    if (err != ESP_OK) {
        print_response_preview(resp.buf);
        set_status(err, status,
                   response_has_empty_message_content(resp.buf) ? "asr_empty" : "parse_failed");
    }
    free(resp.buf);
    return err;
}

static void worker_task(void *arg)
{
    (void)arg;
    asr_job_t job;
    while (true) {
        if (xQueueReceive(s_asr.queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        portENTER_CRITICAL(&s_asr.lock);
        s_asr.status.busy = true;
        portEXIT_CRITICAL(&s_asr.lock);

        asr_config_t cfg = {0};
        esp_err_t cfg_err = load_config(&cfg);
        char text[ONLINE_ASR_TEXT_MAX] = {0};
        int http_status = 0;
        esp_err_t err = cfg_err;
        if (cfg_err == ESP_OK) {
            printf("DG ASR: upload label=%s samples=%u seconds=%.2f model=%s base=%s\r\n",
                   job.label, (unsigned)job.sample_count,
                   (double)job.sample_count / (double)ASR_SAMPLE_RATE, cfg.model, cfg.base_url);
            print_heap_line("DG ASR before upload:");
            err = transcribe_pcm(&cfg, job.samples, job.sample_count, text, sizeof(text), &http_status);
        } else {
            set_status(cfg_err, 0, cfg.enabled ? "asr_not_configured" : "asr_disabled");
        }
        free(job.samples);

        portENTER_CRITICAL(&s_asr.lock);
        s_asr.status.busy = false;
        s_asr.status.last_error = err;
        s_asr.status.last_http_status = http_status;
        if (err == ESP_OK) {
            s_asr.status.ok_count++;
            snprintf(s_asr.status.last_result, sizeof(s_asr.status.last_result), "%s", "asr_ok");
            snprintf(s_asr.status.last_text, sizeof(s_asr.status.last_text), "%s", text);
            snprintf(s_asr.pending_text, sizeof(s_asr.pending_text), "%s", text);
            s_asr.transcript_ready = true;
        } else {
            s_asr.status.failed_count++;
            snprintf(s_asr.status.last_text, sizeof(s_asr.status.last_text), "%s", "");
        }
        portEXIT_CRITICAL(&s_asr.lock);

        if (err == ESP_OK) {
            printf("DG ASR: transcript=\"%s\" http=%d\r\n", text, http_status);
        } else {
            printf("DG ASR: failed err=%s http=%d result=%s\r\n",
                   esp_err_to_name(err), http_status, s_asr.status.last_result);
        }
    }
}

esp_err_t online_asr_init(void)
{
    ESP_RETURN_ON_ERROR(ensure_nvs(), TAG, "nvs");
    asr_config_t cfg = {0};
    (void)load_config(&cfg);
    if (!s_asr.queue) {
        s_asr.queue = xQueueCreate(ASR_QUEUE_DEPTH, sizeof(asr_job_t));
        if (!s_asr.queue) return ESP_ERR_NO_MEM;
    }
    if (!s_asr.worker) {
        if (xTaskCreate(worker_task, "online_asr", ASR_WORKER_STACK, NULL, 4, &s_asr.worker) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_asr.lock);
        s_asr.status.worker_running = true;
        portEXIT_CRITICAL(&s_asr.lock);
    }
    ESP_LOGI(TAG, "ready enabled=%d configured=%d", s_asr.status.enabled, s_asr.status.configured);
    return ESP_OK;
}

bool online_asr_is_ready(void)
{
    asr_config_t cfg = {0};
    return load_config(&cfg) == ESP_OK && s_asr.queue != NULL;
}

bool online_asr_is_busy(void)
{
    bool busy = false;
    portENTER_CRITICAL(&s_asr.lock);
    busy = s_asr.status.busy;
    portEXIT_CRITICAL(&s_asr.lock);
    if (s_asr.queue && uxQueueMessagesWaiting(s_asr.queue) > 0) {
        busy = true;
    }
    return busy;
}

esp_err_t online_asr_configure_openai(const char *api_key)
{
    if (!api_key || !api_key[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(ASR_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open asr nvs");
    esp_err_t err = nvs_set_u8(nvs, ASR_NVS_ENABLED, 1);
    if (err == ESP_OK) err = nvs_set_str(nvs, ASR_NVS_API_KEY, api_key);
    if (err == ESP_OK) err = nvs_set_str(nvs, ASR_NVS_LANGUAGE, "zh");
    if (err == ESP_OK) err = nvs_set_u32(nvs, ASR_NVS_TIMEOUT_MS, 30000);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    asr_config_t cfg = {0};
    (void)load_config(&cfg);
    set_status(err, 0, err == ESP_OK ? "asr_config_saved" : "asr_config_save_failed");
    return err;
}

esp_err_t online_asr_configure_openai_compatible(const char *api_key, const char *base_url, const char *model)
{
    if (!api_key || !api_key[0] || !base_url || !base_url[0] || !model || !model[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(base_url, "https://", 8) != 0 && strncmp(base_url, "http://", 7) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(ASR_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open asr nvs");
    esp_err_t err = nvs_set_u8(nvs, ASR_NVS_ENABLED, 1);
    if (err == ESP_OK) err = nvs_set_str(nvs, ASR_NVS_API_KEY, api_key);
    if (err == ESP_OK) err = nvs_set_str(nvs, ASR_NVS_BASE_URL, base_url);
    if (err == ESP_OK) err = nvs_set_str(nvs, ASR_NVS_MODEL, model);
    if (err == ESP_OK) err = nvs_set_str(nvs, ASR_NVS_LANGUAGE, "zh");
    if (err == ESP_OK) err = nvs_set_u32(nvs, ASR_NVS_TIMEOUT_MS, 30000);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    asr_config_t cfg = {0};
    (void)load_config(&cfg);
    set_status(err, 0, err == ESP_OK ? "asr_compat_saved" : "asr_compat_save_failed");
    return err;
}

esp_err_t online_asr_configure_base_url(const char *base_url)
{
    if (!base_url || !base_url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(base_url, "https://", 8) != 0 && strncmp(base_url, "http://", 7) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(ASR_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open asr nvs");
    esp_err_t err = nvs_set_str(nvs, ASR_NVS_BASE_URL, base_url);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    asr_config_t cfg = {0};
    (void)load_config(&cfg);
    set_status(err, 0, err == ESP_OK ? "asr_url_saved" : "asr_url_save_failed");
    return err;
}

esp_err_t online_asr_configure_model(const char *model)
{
    if (!model || !model[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(ASR_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open asr nvs");
    esp_err_t err = nvs_set_str(nvs, ASR_NVS_MODEL, model);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    asr_config_t cfg = {0};
    (void)load_config(&cfg);
    set_status(err, 0, err == ESP_OK ? "asr_model_saved" : "asr_model_save_failed");
    return err;
}

esp_err_t online_asr_submit_pcm16_take(int16_t *samples, uint32_t sample_count, const char *label)
{
    if (!samples || sample_count == 0) return ESP_ERR_INVALID_ARG;
    if (sample_count > ASR_MAX_SAMPLES) {
        free(samples);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!online_asr_is_ready()) {
        free(samples);
        return ESP_ERR_INVALID_STATE;
    }
    if (online_asr_is_busy()) {
        free(samples);
        portENTER_CRITICAL(&s_asr.lock);
        s_asr.status.dropped_count++;
        portEXIT_CRITICAL(&s_asr.lock);
        set_status(ESP_ERR_INVALID_STATE, 0, "asr_busy");
        return ESP_ERR_INVALID_STATE;
    }
    asr_job_t job = {
        .samples = samples,
        .sample_count = sample_count,
    };
    snprintf(job.label, sizeof(job.label), "%s", label && label[0] ? label : "command");
    if (xQueueSend(s_asr.queue, &job, 0) != pdTRUE) {
        free(samples);
        portENTER_CRITICAL(&s_asr.lock);
        s_asr.status.dropped_count++;
        portEXIT_CRITICAL(&s_asr.lock);
        set_status(ESP_ERR_INVALID_STATE, 0, "queue_full");
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_asr.lock);
    s_asr.status.submitted_count++;
    portEXIT_CRITICAL(&s_asr.lock);
    return ESP_OK;
}

bool online_asr_take_transcript(char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    bool ready = false;
    portENTER_CRITICAL(&s_asr.lock);
    if (s_asr.transcript_ready) {
        snprintf(out, out_size, "%s", s_asr.pending_text);
        s_asr.pending_text[0] = '\0';
        s_asr.transcript_ready = false;
        ready = true;
    }
    portEXIT_CRITICAL(&s_asr.lock);
    return ready;
}

void online_asr_get_status(online_asr_status_t *status)
{
    if (!status) return;
    asr_config_t cfg = {0};
    (void)load_config(&cfg);
    portENTER_CRITICAL(&s_asr.lock);
    *status = s_asr.status;
    portEXIT_CRITICAL(&s_asr.lock);
}

void online_asr_print_diag(void)
{
    asr_config_t cfg = {0};
    esp_err_t cfg_err = load_config(&cfg);
    printf("ASR DIAG: cfg=%s enabled=%d configured=%d model=%s base=%s busy=%d\r\n",
           esp_err_to_name(cfg_err), cfg.enabled ? 1 : 0,
           cfg_err == ESP_OK ? 1 : 0, cfg.model, cfg.base_url,
           online_asr_is_busy() ? 1 : 0);
    print_heap_line("ASR DIAG:");
    printf("ASR DIAG: network probe skipped in console; test with wake upload logs\r\n");
}
