#include "online_chat.h"

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
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CHAT_NVS_NAMESPACE "dg_claw"
#define CHAT_NVS_MODEL "model"
#define CHAT_NVS_BASE_URL "base_url"
#define CHAT_NVS_API_KEY "api_key"
#define CHAT_NVS_ASR_API_KEY "asr_api_key"

#define CHAT_API_KEY_MAX 256
#define CHAT_QUEUE_DEPTH 1
#define CHAT_WORKER_STACK 12288
#define CHAT_SSE_LINE_MAX 8192
#define CHAT_B64_BUF 1024
#define CHAT_TIMEOUT_MS 45000
#define CHAT_AUDIO_PATH "/spiffs/chat_response.wav"

static const char *TAG = "online_chat";

typedef struct {
    char model[ONLINE_CHAT_MODEL_MAX];
    char base_url[ONLINE_CHAT_BASE_URL_MAX];
    char api_key[CHAT_API_KEY_MAX];
} chat_config_t;

typedef struct {
    char text[ONLINE_CHAT_TEXT_MAX];
} chat_job_t;

typedef struct {
    FILE *file;
    uint32_t bytes;
    char b64[CHAT_B64_BUF];
    size_t b64_len;
} audio_b64_sink_t;

typedef struct {
    QueueHandle_t queue;
    TaskHandle_t worker;
    online_chat_status_t status;
    online_chat_result_t pending;
    bool result_ready;
    portMUX_TYPE lock;
} chat_ctx_t;

static chat_ctx_t s_chat = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static void worker_task(void *arg);

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

static esp_err_t load_config(chat_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->model, sizeof(cfg->model), "%s", "qwen3.5-omni-flash");
    snprintf(cfg->base_url, sizeof(cfg->base_url), "%s",
             "https://dashscope.aliyuncs.com/compatible-mode/v1");

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CHAT_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        char llm_key[CHAT_API_KEY_MAX] = {0};
        char asr_key[CHAT_API_KEY_MAX] = {0};
        nvs_get_str_default(nvs, CHAT_NVS_MODEL, cfg->model, sizeof(cfg->model), cfg->model);
        nvs_get_str_default(nvs, CHAT_NVS_BASE_URL, cfg->base_url, sizeof(cfg->base_url), cfg->base_url);
        nvs_get_str_default(nvs, CHAT_NVS_API_KEY, llm_key, sizeof(llm_key), "");
        nvs_get_str_default(nvs, CHAT_NVS_ASR_API_KEY, asr_key, sizeof(asr_key), "");
        if (asr_key[0] && strstr(cfg->base_url, "dashscope.aliyuncs.com")) {
            snprintf(cfg->api_key, sizeof(cfg->api_key), "%s", asr_key);
        } else if (llm_key[0]) {
            snprintf(cfg->api_key, sizeof(cfg->api_key), "%s", llm_key);
        } else if (asr_key[0]) {
            snprintf(cfg->api_key, sizeof(cfg->api_key), "%s", asr_key);
        }
        nvs_close(nvs);
    }

    bool configured = cfg->api_key[0] && cfg->model[0] && cfg->base_url[0];
    portENTER_CRITICAL(&s_chat.lock);
    s_chat.status.configured = configured;
    snprintf(s_chat.status.model, sizeof(s_chat.status.model), "%s", cfg->model);
    snprintf(s_chat.status.base_url, sizeof(s_chat.status.base_url), "%s", cfg->base_url);
    portEXIT_CRITICAL(&s_chat.lock);
    return configured ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t ensure_worker_started(void)
{
    if (s_chat.worker) {
        return ESP_OK;
    }
    if (!s_chat.queue) {
        s_chat.queue = xQueueCreate(CHAT_QUEUE_DEPTH, sizeof(chat_job_t));
        if (!s_chat.queue) return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreateWithCaps(worker_task, "online_chat",
                                        CHAT_WORKER_STACK, NULL, 4,
                                        &s_chat.worker,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreate(worker_task, "online_chat", CHAT_WORKER_STACK, NULL, 4, &s_chat.worker);
    }
    if (ok != pdPASS) {
        s_chat.worker = NULL;
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_chat.lock);
    s_chat.status.worker_running = true;
    portEXIT_CRITICAL(&s_chat.lock);
    printf("DG Chat: worker started stack=psram_preferred\r\n");
    return ESP_OK;
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

static void append_text(char *out, size_t out_size, const char *text)
{
    if (!out || out_size == 0 || !text || !text[0]) return;
    size_t have = strlen(out);
    if (have >= out_size - 1) return;
    snprintf(out + have, out_size - have, "%s", text);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t wav_duration_ms(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    uint8_t header[44];
    size_t n = fread(header, 1, sizeof(header), file);
    fclose(file);
    if (n < sizeof(header) || memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0) {
        return 0;
    }
    uint16_t channels = read_le16(header + 22);
    uint32_t sample_rate = read_le32(header + 24);
    uint16_t bits = read_le16(header + 34);
    uint32_t data_bytes = read_le32(header + 40);
    if (channels == 0 || sample_rate == 0 || bits != 16 || data_bytes == 0) {
        return 0;
    }
    uint32_t bytes_per_sec = sample_rate * channels * (bits / 8U);
    if (bytes_per_sec == 0) return 0;
    return (uint32_t)(((uint64_t)data_bytes * 1000ULL) / bytes_per_sec);
}

static esp_err_t b64_sink_flush(audio_b64_sink_t *sink)
{
    if (!sink || !sink->file || sink->b64_len == 0) return ESP_OK;
    if ((sink->b64_len % 4U) != 0) return ESP_ERR_INVALID_SIZE;

    size_t out_cap = (sink->b64_len / 4U) * 3U + 4U;
    uint8_t *decoded = heap_caps_malloc(out_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!decoded) decoded = heap_caps_malloc(out_cap, MALLOC_CAP_8BIT);
    if (!decoded) return ESP_ERR_NO_MEM;

    size_t out_len = 0;
    int ret = mbedtls_base64_decode(decoded, out_cap, &out_len,
                                    (const uint8_t *)sink->b64, sink->b64_len);
    if (ret != 0) {
        free(decoded);
        return ESP_FAIL;
    }
    if (out_len > 0 && fwrite(decoded, 1, out_len, sink->file) != out_len) {
        free(decoded);
        return ESP_FAIL;
    }
    sink->bytes += (uint32_t)out_len;
    sink->b64_len = 0;
    free(decoded);
    return ESP_OK;
}

static esp_err_t b64_sink_append(audio_b64_sink_t *sink, const char *data)
{
    if (!sink || !data) return ESP_ERR_INVALID_ARG;
    for (const char *p = data; *p; ++p) {
        if (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') {
            continue;
        }
        sink->b64[sink->b64_len++] = *p;
        if (sink->b64_len == sizeof(sink->b64)) {
            ESP_RETURN_ON_ERROR(b64_sink_flush(sink), TAG, "b64 flush");
        }
    }
    return ESP_OK;
}

static esp_err_t parse_stream_json(const char *json, online_chat_result_t *result,
                                   audio_b64_sink_t *audio)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_FAIL;

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *delta = choice ? cJSON_GetObjectItem(choice, "delta") : NULL;
    cJSON *message = choice ? cJSON_GetObjectItem(choice, "message") : NULL;
    cJSON *obj = cJSON_IsObject(delta) ? delta : message;
    if (cJSON_IsObject(obj)) {
        cJSON *content = cJSON_GetObjectItem(obj, "content");
        if (cJSON_IsString(content) && content->valuestring) {
            append_text(result->text, sizeof(result->text), content->valuestring);
        }
        cJSON *audio_obj = cJSON_GetObjectItem(obj, "audio");
        cJSON *data = cJSON_IsObject(audio_obj) ? cJSON_GetObjectItem(audio_obj, "data") : NULL;
        if (cJSON_IsString(data) && data->valuestring && data->valuestring[0]) {
            esp_err_t err = b64_sink_append(audio, data->valuestring);
            cJSON_Delete(root);
            return err;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_sse_line(char *line, online_chat_result_t *result,
                                 audio_b64_sink_t *audio, bool *done)
{
    if (!line || !result || !audio || !done) return ESP_ERR_INVALID_ARG;
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "data:", 5) != 0) return ESP_OK;
    line += 5;
    while (*line == ' ' || *line == '\t') line++;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }
    if (strcmp(line, "[DONE]") == 0) {
        *done = true;
        return ESP_OK;
    }
    return parse_stream_json(line, result, audio);
}

static esp_err_t build_chat_body(const chat_config_t *cfg, const char *text, char **out_body)
{
    if (!cfg || !text || !out_body) return ESP_ERR_INVALID_ARG;
    *out_body = NULL;

    cJSON *body = cJSON_CreateObject();
    if (!body) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(body, "model", cfg->model);
    cJSON_AddBoolToObject(body, "stream", true);
    cJSON_AddNumberToObject(body, "max_tokens", 180);
    cJSON_AddNumberToObject(body, "temperature", 0.6);

    cJSON *modalities = cJSON_AddArrayToObject(body, "modalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));

    cJSON *audio = cJSON_AddObjectToObject(body, "audio");
    cJSON_AddStringToObject(audio, "voice", "Cherry");
    cJSON_AddStringToObject(audio, "format", "wav");

    cJSON *messages = cJSON_AddArrayToObject(body, "messages");
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content",
        "你是小梦，智能睡眠守护设备里的语音助手。"
        "用户通过唤醒词与你连续语音交流。回答要自然、简短、适合直接朗读，"
        "不要使用Markdown，不要列太多点。设备能力包括灯带控制、助眠音乐、睡眠模式、传感器状态辅助。"
        "如果用户问你是谁或让你自我介绍，请用第一人称说明你是小梦以及你能做什么。");
    cJSON_AddItemToArray(messages, sys);
    cJSON *usr = cJSON_CreateObject();
    cJSON_AddStringToObject(usr, "role", "user");
    cJSON_AddStringToObject(usr, "content", text);
    cJSON_AddItemToArray(messages, usr);

    *out_body = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return *out_body ? ESP_OK : ESP_ERR_NO_MEM;
}

static int http_write_all(esp_http_client_handle_t client, const char *data, size_t len)
{
    size_t written_total = 0;
    while (written_total < len) {
        int written = esp_http_client_write(client, data + written_total, len - written_total);
        if (written <= 0) return written;
        written_total += (size_t)written;
    }
    return (int)written_total;
}

static esp_err_t chat_request_audio(const chat_config_t *cfg, const char *text,
                                    online_chat_result_t *result)
{
    if (!cfg || !text || !result) return ESP_ERR_INVALID_ARG;
    char url[ONLINE_CHAT_BASE_URL_MAX + 32];
    join_api_path(url, sizeof(url), cfg->base_url, "/chat/completions");

    char *body = NULL;
    ESP_RETURN_ON_ERROR(build_chat_body(cfg, text, &body), TAG, "build body");
    size_t body_len = strlen(body);

    (void)remove(CHAT_AUDIO_PATH);
    audio_b64_sink_t audio = {0};
    audio.file = fopen(CHAT_AUDIO_PATH, "wb");
    if (!audio.file) {
        free(body);
        return ESP_FAIL;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = CHAT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .addr_type = HTTP_ADDR_TYPE_INET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        fclose(audio.file);
        free(body);
        return ESP_ERR_NO_MEM;
    }
    char auth[CHAT_API_KEY_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_header(client, "Accept", "text/event-stream");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err == ESP_OK) {
        int written = http_write_all(client, body, body_len);
        if (written != (int)body_len) {
            err = ESP_FAIL;
        }
    }
    int status = 0;
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
    }

    char *line = heap_caps_malloc(CHAT_SSE_LINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line) line = heap_caps_malloc(CHAT_SSE_LINE_MAX, MALLOC_CAP_8BIT);
    if (!line && err == ESP_OK) {
        err = ESP_ERR_NO_MEM;
    }

    size_t line_len = 0;
    bool done = false;
    char rx[384];
    while (err == ESP_OK && !done) {
        int r = esp_http_client_read(client, rx, sizeof(rx));
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        for (int i = 0; i < r; ++i) {
            if (line_len + 1 >= CHAT_SSE_LINE_MAX) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            line[line_len++] = rx[i];
            if (rx[i] == '\n') {
                line[line_len] = '\0';
                err = handle_sse_line(line, result, &audio, &done);
                line_len = 0;
                if (err != ESP_OK || done) break;
            }
        }
    }
    if (err == ESP_OK && line_len > 0 && line) {
        line[line_len] = '\0';
        err = handle_sse_line(line, result, &audio, &done);
    }
    if (err == ESP_OK) {
        err = b64_sink_flush(&audio);
    }

    int tls_error = 0;
    int tls_flags = 0;
    (void)esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
    esp_http_client_cleanup(client);
    if (line) free(line);
    free(body);
    fclose(audio.file);

    result->http_status = status;
    result->audio_bytes = audio.bytes;
    if (err != ESP_OK) {
        char msg[ONLINE_CHAT_RESULT_MAX];
        snprintf(msg, sizeof(msg), "chat_http_failed err=%s tls=%d flags=0x%x",
                 esp_err_to_name(err), tls_error, tls_flags);
        snprintf(result->result, sizeof(result->result), "%s", msg);
        return err;
    }
    if (status < 200 || status >= 300) {
        snprintf(result->result, sizeof(result->result), "chat_http_status=%d", status);
        return ESP_FAIL;
    }
    if (audio.bytes < 64U) {
        snprintf(result->result, sizeof(result->result), "%s", "chat_audio_empty");
        return ESP_FAIL;
    }
    snprintf(result->audio_path, sizeof(result->audio_path), "%s", CHAT_AUDIO_PATH);
    result->duration_ms = wav_duration_ms(CHAT_AUDIO_PATH);
    if (result->duration_ms == 0) {
        size_t text_len = strlen(result->text);
        result->duration_ms = 2500U + (uint32_t)text_len * 180U;
    }
    snprintf(result->result, sizeof(result->result), "%s", "chat_ok");
    return ESP_OK;
}

static void worker_task(void *arg)
{
    (void)arg;
    chat_job_t job;
    while (true) {
        if (xQueueReceive(s_chat.queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        portENTER_CRITICAL(&s_chat.lock);
        s_chat.status.busy = true;
        portEXIT_CRITICAL(&s_chat.lock);

        online_chat_result_t result = {0};
        chat_config_t cfg = {0};
        esp_err_t cfg_err = load_config(&cfg);
        esp_err_t err = cfg_err;
        if (cfg_err == ESP_OK) {
            printf("DG Chat: request text=\"%s\" model=%s base=%s\r\n",
                   job.text, cfg.model, cfg.base_url);
            err = chat_request_audio(&cfg, job.text, &result);
        } else {
            snprintf(result.result, sizeof(result.result), "%s", "chat_not_configured");
        }
        result.error = err;

        portENTER_CRITICAL(&s_chat.lock);
        s_chat.status.busy = false;
        s_chat.status.last_error = err;
        s_chat.status.last_http_status = result.http_status;
        snprintf(s_chat.status.last_result, sizeof(s_chat.status.last_result), "%s",
                 result.result[0] ? result.result : esp_err_to_name(err));
        snprintf(s_chat.status.last_text, sizeof(s_chat.status.last_text), "%s", result.text);
        if (err == ESP_OK) {
            s_chat.status.ok_count++;
        } else {
            s_chat.status.failed_count++;
        }
        s_chat.pending = result;
        s_chat.result_ready = true;
        portEXIT_CRITICAL(&s_chat.lock);

        printf("DG Chat: done err=%s http=%d result=%s text=\"%s\" audio=%u duration_ms=%u\r\n",
               esp_err_to_name(err), result.http_status, result.result,
               result.text, (unsigned)result.audio_bytes, (unsigned)result.duration_ms);
    }
}

esp_err_t online_chat_init(void)
{
    ESP_RETURN_ON_ERROR(ensure_nvs(), TAG, "nvs");
    chat_config_t cfg = {0};
    (void)load_config(&cfg);
    if (!s_chat.queue) {
        s_chat.queue = xQueueCreate(CHAT_QUEUE_DEPTH, sizeof(chat_job_t));
        if (!s_chat.queue) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool online_chat_is_ready(void)
{
    chat_config_t cfg = {0};
    return load_config(&cfg) == ESP_OK && s_chat.queue != NULL;
}

bool online_chat_is_busy(void)
{
    portENTER_CRITICAL(&s_chat.lock);
    bool busy = s_chat.status.busy;
    portEXIT_CRITICAL(&s_chat.lock);
    return busy;
}

esp_err_t online_chat_submit_text(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(ensure_worker_started(), TAG, "chat worker");
    if (online_chat_is_busy()) return ESP_ERR_INVALID_STATE;
    chat_job_t job = {0};
    snprintf(job.text, sizeof(job.text), "%s", text);
    if (xQueueSend(s_chat.queue, &job, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_chat.lock);
        s_chat.status.dropped_count++;
        portEXIT_CRITICAL(&s_chat.lock);
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_chat.lock);
    s_chat.status.submitted_count++;
    portEXIT_CRITICAL(&s_chat.lock);
    return ESP_OK;
}

bool online_chat_take_result(online_chat_result_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_chat.lock);
    bool ready = s_chat.result_ready;
    if (ready) {
        *out = s_chat.pending;
        s_chat.result_ready = false;
    }
    portEXIT_CRITICAL(&s_chat.lock);
    return ready;
}

void online_chat_get_status(online_chat_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_chat.lock);
    *status = s_chat.status;
    portEXIT_CRITICAL(&s_chat.lock);
}
