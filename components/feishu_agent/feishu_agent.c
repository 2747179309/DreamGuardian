#include "feishu_agent.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "llm_intent.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sleep_history.h"

#define FEISHU_API_BASE "https://open.feishu.cn/open-apis"
#define FEISHU_AUTH_URL FEISHU_API_BASE "/auth/v3/tenant_access_token/internal"
#define FEISHU_SEND_MSG_URL FEISHU_API_BASE "/im/v1/messages"
#define FEISHU_UPLOAD_IMAGE_URL FEISHU_API_BASE "/im/v1/images"
#define FEISHU_WS_CONFIG_URL "https://open.feishu.cn/callback/ws/endpoint"

#define FEISHU_APP_ID_LEN 64
#define FEISHU_APP_SECRET_LEN 256
#define FEISHU_TOKEN_LEN 512
#define FEISHU_URL_LEN 512
#define FEISHU_MAX_HEADERS 16
#define FEISHU_MAX_RESPONSE 8192
#define FEISHU_DEDUP_CACHE_SIZE 32
#define FEISHU_RECONNECT_DELAY_MS 60000
#define FEISHU_INITIAL_CONNECT_TIMEOUT_MS 15000
#define FEISHU_AGENT_TASK_STACK 8192
#define FEISHU_WS_TASK_STACK 3584
#define FEISHU_REPLY_TASK_STACK 8192
#define FEISHU_REPLY_QUEUE_LEN 8
#define FEISHU_EVENT_QUEUE_LEN 6
#define FEISHU_EVENT_MAX_BYTES (16 * 1024)
#define FEISHU_LOW_INTERNAL_HEAP_BYTES (24 * 1024)
#define FEISHU_LOCAL_COMMAND_ONLY 1
#define FEISHU_SEND_REPLIES 1
#define FEISHU_REPORT_WIDTH 600
#define FEISHU_REPORT_HEIGHT 360

#define CLAW_NVS_NAMESPACE "dg_claw"
#define CLAW_NVS_BACKEND_TYPE "backend_type"
#define CLAW_NVS_MODEL "model"
#define CLAW_NVS_BASE_URL "base_url"
#define CLAW_NVS_API_KEY "api_key"
#define CLAW_NVS_AUTH_TYPE "auth_type"
#define CLAW_NVS_MAX_TOKENS_FIELD "tokens_field"

#define LLM_BACKEND_LEN 32
#define LLM_MODEL_LEN 64
#define LLM_URL_LEN 256
#define LLM_API_KEY_LEN 256
#define LLM_AUTH_LEN 32
#define LLM_FIELD_LEN 32
#define LLM_REPLY_LEN 256

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} feishu_resp_t;

typedef struct {
    char key[32];
    char value[128];
} feishu_ws_header_t;

typedef struct {
    uint64_t seq_id;
    uint64_t log_id;
    int32_t service;
    int32_t method;
    feishu_ws_header_t headers[FEISHU_MAX_HEADERS];
    size_t header_count;
    const uint8_t *payload;
    size_t payload_len;
} feishu_ws_frame_t;

typedef struct {
    char backend_type[LLM_BACKEND_LEN];
    char model[LLM_MODEL_LEN];
    char base_url[LLM_URL_LEN];
    char api_key[LLM_API_KEY_LEN];
    char auth_type[LLM_AUTH_LEN];
    char max_tokens_field[LLM_FIELD_LEN];
} llm_config_t;

typedef struct {
    mobile_app_command_t command;
    char reply[LLM_REPLY_LEN];
} llm_route_t;

typedef struct {
    char route_id[128];
    char text[1024];
    uint8_t type;
    uint8_t days;
} feishu_reply_msg_t;

typedef struct {
    size_t len;
    char data[];
} feishu_event_msg_t;

enum {
    FEISHU_REPLY_TEXT = 0,
    FEISHU_REPLY_SLEEP_REPORT = 1,
};

typedef struct {
    char app_id[FEISHU_APP_ID_LEN];
    char app_secret[FEISHU_APP_SECRET_LEN];
    char tenant_token[FEISHU_TOKEN_LEN];
    int64_t token_expire_time_ms;
    char ws_url[FEISHU_URL_LEN];
    int ws_ping_interval_ms;
    int ws_reconnect_interval_ms;
    int ws_reconnect_nonce_ms;
    int ws_service_id;
    bool ws_connected;
    bool ws_ever_connected;
    int64_t ws_disconnect_since_ms;
    bool stop_requested;
    bool running;
    bool restart_requested;
    esp_websocket_client_handle_t ws_client;
    TaskHandle_t ws_task;
    QueueHandle_t reply_queue;
    TaskHandle_t reply_task;
    QueueHandle_t event_queue;
    uint64_t seen_message_keys[FEISHU_DEDUP_CACHE_SIZE];
    size_t seen_message_idx;
    mobile_app_command_cb_t command_cb;
    void *command_ctx;
    uint32_t ws_data_count;
    uint32_t ws_binary_count;
    uint32_t ws_text_count;
    uint32_t last_ws_opcode;
    uint32_t last_ws_data_len;
    uint32_t last_ws_payload_len;
    uint32_t frame_count;
    uint32_t event_count;
    uint32_t message_event_count;
    uint32_t parse_fail_count;
    uint32_t received_count;
    uint32_t command_count;
    uint32_t reply_count;
    int last_http_status;
    char last_event_type[64];
    char last_result[FEISHU_AGENT_LAST_RESULT_LEN];
} feishu_state_t;

static const char *TAG = "feishu_agent";
static feishu_state_t s_feishu = {
    .ws_ping_interval_ms = 120000,
    .ws_reconnect_interval_ms = 30000,
    .ws_reconnect_nonce_ms = 30000,
};

static esp_err_t send_text(const char *chat_id, const char *text);
static void process_ws_event_json(const char *json, size_t len);

static void *feishu_psram_calloc(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ptr ? ptr : calloc(count, size);
}

static void *feishu_psram_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ptr ? ptr : malloc(size);
}

static void set_status_result(const char *text, int http_status)
{
    snprintf(s_feishu.last_result, sizeof(s_feishu.last_result), "%s", text ? text : "");
    s_feishu.last_http_status = http_status;
}

static bool last_result_has_prefix(const char *prefix)
{
    size_t len = strlen(prefix);
    return strncmp(s_feishu.last_result, prefix, len) == 0;
}

static bool internal_heap_low(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < FEISHU_LOW_INTERNAL_HEAP_BYTES;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static uint64_t fnv1a64(const char *text)
{
    uint64_t hash = 1469598103934665603ULL;
    if (!text) return hash;
    while (*text) {
        hash ^= (uint8_t)(*text++);
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool dedup_check_and_record(const char *message_id)
{
    uint64_t key = fnv1a64(message_id);
    for (size_t i = 0; i < FEISHU_DEDUP_CACHE_SIZE; i++) {
        if (s_feishu.seen_message_keys[i] == key) return true;
    }
    s_feishu.seen_message_keys[s_feishu.seen_message_idx] = key;
    s_feishu.seen_message_idx = (s_feishu.seen_message_idx + 1) % FEISHU_DEDUP_CACHE_SIZE;
    return false;
}

static esp_err_t resp_init(feishu_resp_t *resp)
{
    resp->buf = heap_caps_calloc(1, FEISHU_MAX_RESPONSE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp->buf) {
        resp->buf = calloc(1, FEISHU_MAX_RESPONSE);
    }
    if (!resp->buf) return ESP_ERR_NO_MEM;
    resp->cap = FEISHU_MAX_RESPONSE;
    resp->len = 0;
    return ESP_OK;
}

static esp_err_t resp_append(feishu_resp_t *resp, const char *data, size_t len)
{
    size_t cap = resp->cap;
    while (resp->len + len + 1 > cap) cap *= 2;
    if (cap != resp->cap) {
        char *grown = realloc(resp->buf, cap);
        if (!grown) return ESP_ERR_NO_MEM;
        resp->buf = grown;
        resp->cap = cap;
    }
    memcpy(resp->buf + resp->len, data, len);
    resp->len += len;
    resp->buf[resp->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    feishu_resp_t *resp = (feishu_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && resp && evt->data && evt->data_len > 0) {
        return resp_append(resp, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

static esp_err_t http_json(const char *url,
                           const char *method,
                           const char *authorization,
                           const char *body,
                           char **out_response,
                           int *out_status)
{
    feishu_resp_t resp = {0};
    esp_err_t err = ESP_FAIL;
    int tls_error = 0;
    int tls_flags = 0;

    *out_response = NULL;
    *out_status = 0;
    ESP_RETURN_ON_ERROR(resp_init(&resp), TAG, "response alloc");

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .addr_type = HTTP_ADDR_TYPE_INET,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };

    for (int attempt = 0; attempt < 3; ++attempt) {
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            err = ESP_FAIL;
        } else {
            resp.len = 0;
            if (resp.buf && resp.cap > 0) {
                resp.buf[0] = '\0';
            }

            esp_http_client_set_method(client, strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
            esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
            esp_http_client_set_header(client, "Accept", "application/json");
            esp_http_client_set_header(client, "Connection", "close");
            if (authorization && authorization[0]) {
                esp_http_client_set_header(client, "Authorization", authorization);
            }
            if (body) {
                esp_http_client_set_post_field(client, body, (int)strlen(body));
            }

            err = esp_http_client_perform(client);
            *out_status = esp_http_client_get_status_code(client);
            tls_error = 0;
            tls_flags = 0;
            (void)esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
            esp_http_client_cleanup(client);
            if (err == ESP_OK) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 500 : 1500));
    }
    if (err != ESP_OK) {
        char msg[FEISHU_AGENT_LAST_RESULT_LEN];
        snprintf(msg, sizeof(msg), "http_failed err=%s tls=%d flags=0x%x heap=%u/%u",
                 esp_err_to_name(err), tls_error, tls_flags,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        set_status_result(msg, *out_status);
        free(resp.buf);
        return err;
    }

    set_status_result("http_ok", *out_status);
    *out_response = resp.buf;
    return ESP_OK;
}

static void nvs_get_str_default(nvs_handle_t nvs, const char *key, char *out, size_t out_size, const char *fallback)
{
    if (!out || out_size == 0) return;
    size_t len = out_size;
    if (nvs_get_str(nvs, key, out, &len) != ESP_OK) {
        snprintf(out, out_size, "%s", fallback ? fallback : "");
    }
}

static esp_err_t llm_load_config(llm_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->backend_type, sizeof(cfg->backend_type), "%s", "openai_compatible");
    snprintf(cfg->model, sizeof(cfg->model), "%s", "qwen3.5-omni-flash");
    snprintf(cfg->base_url, sizeof(cfg->base_url), "%s", "https://dashscope.aliyuncs.com/compatible-mode/v1");
    snprintf(cfg->auth_type, sizeof(cfg->auth_type), "%s", "bearer");
    snprintf(cfg->max_tokens_field, sizeof(cfg->max_tokens_field), "%s", "max_tokens");

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CLAW_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_INVALID_STATE : err;
    nvs_get_str_default(nvs, CLAW_NVS_BACKEND_TYPE, cfg->backend_type, sizeof(cfg->backend_type), cfg->backend_type);
    nvs_get_str_default(nvs, CLAW_NVS_MODEL, cfg->model, sizeof(cfg->model), cfg->model);
    nvs_get_str_default(nvs, CLAW_NVS_BASE_URL, cfg->base_url, sizeof(cfg->base_url), cfg->base_url);
    nvs_get_str_default(nvs, CLAW_NVS_API_KEY, cfg->api_key, sizeof(cfg->api_key), "");
    nvs_get_str_default(nvs, CLAW_NVS_AUTH_TYPE, cfg->auth_type, sizeof(cfg->auth_type), cfg->auth_type);
    nvs_get_str_default(nvs, CLAW_NVS_MAX_TOKENS_FIELD, cfg->max_tokens_field, sizeof(cfg->max_tokens_field), cfg->max_tokens_field);
    nvs_close(nvs);
    return cfg->api_key[0] && cfg->model[0] && cfg->base_url[0] ? ESP_OK : ESP_ERR_INVALID_STATE;
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

static esp_err_t llm_http_json(const char *url, const llm_config_t *cfg, const char *body, char **out_response, int *out_status)
{
    feishu_resp_t resp = {0};
    esp_http_client_handle_t client = NULL;
    esp_err_t err;

    *out_response = NULL;
    *out_status = 0;
    ESP_RETURN_ON_ERROR(resp_init(&resp), TAG, "llm response alloc");

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 25000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .addr_type = HTTP_ADDR_TYPE_INET,
    };
    client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    if (strstr(cfg->backend_type, "anthropic")) {
        esp_http_client_set_header(client, "x-api-key", cfg->api_key);
        esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
    } else if (strcasecmp(cfg->auth_type, "none") != 0) {
        char auth[LLM_API_KEY_LEN + 16];
        snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    int tls_error = 0;
    int tls_flags = 0;
    (void)esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        char msg[FEISHU_AGENT_LAST_RESULT_LEN];
        snprintf(msg, sizeof(msg), "llm_http_failed err=%s tls=%d flags=0x%x",
                 esp_err_to_name(err), tls_error, tls_flags);
        set_status_result(msg, *out_status);
        free(resp.buf);
        return err;
    }
    if (*out_status < 200 || *out_status >= 300) {
        char msg[FEISHU_AGENT_LAST_RESULT_LEN];
        snprintf(msg, sizeof(msg), "llm_http_status=%d", *out_status);
        set_status_result(msg, *out_status);
        free(resp.buf);
        return ESP_FAIL;
    }
    *out_response = resp.buf;
    return ESP_OK;
}

static esp_err_t load_credentials(void)
{
    nvs_handle_t nvs;
    size_t len;
    esp_err_t err = nvs_open("dg_claw", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    memset(s_feishu.app_id, 0, sizeof(s_feishu.app_id));
    memset(s_feishu.app_secret, 0, sizeof(s_feishu.app_secret));

    len = sizeof(s_feishu.app_id);
    (void)nvs_get_str(nvs, "fs_app_id", s_feishu.app_id, &len);
    len = sizeof(s_feishu.app_secret);
    (void)nvs_get_str(nvs, "fs_secret", s_feishu.app_secret, &len);
    nvs_close(nvs);

    if (!s_feishu.app_id[0] || !s_feishu.app_secret[0]) {
        ESP_LOGW(TAG, "Feishu App ID/Secret not configured");
        set_status_result("app_id_or_secret_not_configured", 0);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t get_tenant_token(void)
{
    cJSON *body = NULL;
    char *body_str = NULL;
    char *response = NULL;
    cJSON *root = NULL;
    int status = 0;
    int64_t t = now_ms();
    esp_err_t err;

    if (s_feishu.tenant_token[0] && t + 300000 < s_feishu.token_expire_time_ms) {
        return ESP_OK;
    }

    body = cJSON_CreateObject();
    if (!body) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(body, "app_id", s_feishu.app_id);
    cJSON_AddStringToObject(body, "app_secret", s_feishu.app_secret);
    body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return ESP_ERR_NO_MEM;

    err = http_json(FEISHU_AUTH_URL, "POST", NULL, body_str, &response, &status);
    free(body_str);
    if (err != ESP_OK) {
        char prev[sizeof(s_feishu.last_result)];
        snprintf(prev, sizeof(prev), "%s", s_feishu.last_result);
        char msg[96];
        snprintf(msg, sizeof(msg), "tenant_token_request_failed %.60s", prev);
        set_status_result(msg, status);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "tenant token HTTP %d body=%s", status, response ? response : "");
        set_status_result("tenant_token_http_failed", status);
        free(response);
        return ESP_FAIL;
    }

    root = cJSON_Parse(response);
    free(response);
    if (!root) return ESP_FAIL;

    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *token = cJSON_GetObjectItem(root, "tenant_access_token");
    cJSON *expire = cJSON_GetObjectItem(root, "expire");
    if (!cJSON_IsNumber(code) || code->valueint != 0 || !cJSON_IsString(token)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "tenant token response invalid");
        set_status_result("tenant_token_invalid_response", status);
        return ESP_FAIL;
    }

    snprintf(s_feishu.tenant_token, sizeof(s_feishu.tenant_token), "%s", token->valuestring);
    s_feishu.token_expire_time_ms = t + (int64_t)(cJSON_IsNumber(expire) ? expire->valueint : 7200) * 1000LL;
    cJSON_Delete(root);
    ESP_LOGI(TAG, "tenant token refreshed");
    set_status_result("tenant_token_ok", status);
    return ESP_OK;
}

static esp_err_t api_call(const char *url, const char *method, const char *body, char **out_response)
{
    char auth[FEISHU_TOKEN_LEN + 16];
    int status = 0;
    esp_err_t err = get_tenant_token();
    if (err != ESP_OK) return err;

    snprintf(auth, sizeof(auth), "Bearer %s", s_feishu.tenant_token);
    err = http_json(url, method, auth, body, out_response, &status);
    if (err != ESP_OK) return err;
    if (status != 200) {
        ESP_LOGE(TAG, "Feishu API HTTP %d url=%s body=%s", status, url, *out_response ? *out_response : "");
        set_status_result("api_http_failed", status);
        free(*out_response);
        *out_response = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool parse_query_param(const char *url, const char *key, char *out, size_t out_size)
{
    const char *q = strchr(url, '?');
    size_t key_len = strlen(key);
    if (!q) return false;
    q++;
    while (*q) {
        const char *eq = strchr(q, '=');
        if (!eq) break;
        const char *amp = strchr(eq + 1, '&');
        size_t name_len = (size_t)(eq - q);
        if (name_len == key_len && strncmp(q, key, key_len) == 0) {
            size_t value_len = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);
            size_t copy_len = value_len < out_size - 1 ? value_len : out_size - 1;
            memcpy(out, eq + 1, copy_len);
            out[copy_len] = '\0';
            return true;
        }
        if (!amp) break;
        q = amp + 1;
    }
    return false;
}

static esp_err_t pull_ws_config(void)
{
    cJSON *body = cJSON_CreateObject();
    char *body_str = NULL;
    char *response = NULL;
    cJSON *root = NULL;
    int status = 0;
    char service_id[24] = {0};
    esp_err_t err;

    if (!body) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(body, "AppID", s_feishu.app_id);
    cJSON_AddStringToObject(body, "AppSecret", s_feishu.app_secret);
    body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return ESP_ERR_NO_MEM;

    err = http_json(FEISHU_WS_CONFIG_URL, "POST", NULL, body_str, &response, &status);
    free(body_str);
    if (err != ESP_OK) {
        char prev[sizeof(s_feishu.last_result)];
        snprintf(prev, sizeof(prev), "%s", s_feishu.last_result);
        char msg[FEISHU_AGENT_LAST_RESULT_LEN];
        snprintf(msg, sizeof(msg), "ws_config_request_failed %.220s", prev);
        set_status_result(msg, status);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "WS config HTTP %d body=%s", status, response ? response : "");
        set_status_result("ws_config_http_failed", status);
        free(response);
        return ESP_FAIL;
    }

    root = cJSON_Parse(response);
    free(response);
    if (!root) return ESP_FAIL;

    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *url = cJSON_IsObject(data) ? cJSON_GetObjectItem(data, "URL") : NULL;
    cJSON *cfg = cJSON_IsObject(data) ? cJSON_GetObjectItem(data, "ClientConfig") : NULL;
    if (!cJSON_IsNumber(code) || code->valueint != 0 || !cJSON_IsString(url)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "WS config response invalid");
        set_status_result("ws_config_invalid_response", status);
        return ESP_FAIL;
    }

    snprintf(s_feishu.ws_url, sizeof(s_feishu.ws_url), "%s", url->valuestring);
    if (parse_query_param(s_feishu.ws_url, "service_id", service_id, sizeof(service_id))) {
        s_feishu.ws_service_id = atoi(service_id);
    }
    if (cJSON_IsObject(cfg)) {
        cJSON *ping = cJSON_GetObjectItem(cfg, "PingInterval");
        cJSON *reconnect = cJSON_GetObjectItem(cfg, "ReconnectInterval");
        cJSON *nonce = cJSON_GetObjectItem(cfg, "ReconnectNonce");
        if (cJSON_IsNumber(ping)) s_feishu.ws_ping_interval_ms = ping->valueint * 1000;
        if (cJSON_IsNumber(reconnect)) s_feishu.ws_reconnect_interval_ms = reconnect->valueint * 1000;
        if (cJSON_IsNumber(nonce)) s_feishu.ws_reconnect_nonce_ms = nonce->valueint * 1000;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "WS config loaded service=%d", s_feishu.ws_service_id);
    set_status_result("ws_config_ok", status);
    return ESP_OK;
}

static bool pb_read_varint(const uint8_t *buf, size_t len, size_t *pos, uint64_t *out)
{
    uint64_t value = 0;
    int shift = 0;
    while (*pos < len && shift <= 63) {
        uint8_t byte = buf[(*pos)++];
        value |= ((uint64_t)(byte & 0x7F)) << shift;
        if ((byte & 0x80) == 0) {
            *out = value;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool pb_skip_field(const uint8_t *buf, size_t len, size_t *pos, uint8_t wire_type)
{
    uint64_t size = 0;
    switch (wire_type) {
    case 0:
        return pb_read_varint(buf, len, pos, &size);
    case 1:
        if (*pos + 8 > len) return false;
        *pos += 8;
        return true;
    case 2:
        if (!pb_read_varint(buf, len, pos, &size) || *pos + (size_t)size > len) return false;
        *pos += (size_t)size;
        return true;
    case 5:
        if (*pos + 4 > len) return false;
        *pos += 4;
        return true;
    default:
        return false;
    }
}

static bool pb_parse_header(const uint8_t *buf, size_t len, feishu_ws_header_t *header)
{
    size_t pos = 0;
    memset(header, 0, sizeof(*header));
    while (pos < len) {
        uint64_t tag = 0;
        uint64_t size = 0;
        if (!pb_read_varint(buf, len, &pos, &tag)) return false;
        uint32_t field = (uint32_t)(tag >> 3);
        uint8_t wire_type = (uint8_t)(tag & 0x07);
        if (wire_type != 2) {
            if (!pb_skip_field(buf, len, &pos, wire_type)) return false;
            continue;
        }
        if (!pb_read_varint(buf, len, &pos, &size) || pos + (size_t)size > len) return false;
        if (field == 1) {
            size_t copy_len = (size_t)size < sizeof(header->key) - 1 ? (size_t)size : sizeof(header->key) - 1;
            memcpy(header->key, buf + pos, copy_len);
        } else if (field == 2) {
            size_t copy_len = (size_t)size < sizeof(header->value) - 1 ? (size_t)size : sizeof(header->value) - 1;
            memcpy(header->value, buf + pos, copy_len);
        }
        pos += (size_t)size;
    }
    return true;
}

static bool pb_parse_frame(const uint8_t *buf, size_t len, feishu_ws_frame_t *frame)
{
    size_t pos = 0;
    memset(frame, 0, sizeof(*frame));
    while (pos < len) {
        uint64_t tag = 0;
        uint64_t value = 0;
        uint64_t size = 0;
        if (!pb_read_varint(buf, len, &pos, &tag)) return false;
        uint32_t field = (uint32_t)(tag >> 3);
        uint8_t wire_type = (uint8_t)(tag & 0x07);

        if (field == 1 && wire_type == 0) {
            if (!pb_read_varint(buf, len, &pos, &frame->seq_id)) return false;
        } else if (field == 2 && wire_type == 0) {
            if (!pb_read_varint(buf, len, &pos, &frame->log_id)) return false;
        } else if (field == 3 && wire_type == 0) {
            if (!pb_read_varint(buf, len, &pos, &value)) return false;
            frame->service = (int32_t)value;
        } else if (field == 4 && wire_type == 0) {
            if (!pb_read_varint(buf, len, &pos, &value)) return false;
            frame->method = (int32_t)value;
        } else if (field == 5 && wire_type == 2) {
            if (!pb_read_varint(buf, len, &pos, &size) || pos + (size_t)size > len) return false;
            if (frame->header_count < FEISHU_MAX_HEADERS) {
                (void)pb_parse_header(buf + pos, (size_t)size, &frame->headers[frame->header_count++]);
            }
            pos += (size_t)size;
        } else if (field == 8 && wire_type == 2) {
            if (!pb_read_varint(buf, len, &pos, &size) || pos + (size_t)size > len) return false;
            frame->payload = buf + pos;
            frame->payload_len = (size_t)size;
            pos += (size_t)size;
        } else {
            if (!pb_skip_field(buf, len, &pos, wire_type)) return false;
        }
    }
    return true;
}

static const char *ws_header_value(const feishu_ws_frame_t *frame, const char *key)
{
    for (size_t i = 0; i < frame->header_count; i++) {
        if (strcmp(frame->headers[i].key, key) == 0) return frame->headers[i].value;
    }
    return NULL;
}

static bool pb_write_varint(uint8_t *buf, size_t cap, size_t *pos, uint64_t value)
{
    do {
        if (*pos >= cap) return false;
        uint8_t byte = (uint8_t)(value & 0x7F);
        value >>= 7;
        if (value) byte |= 0x80;
        buf[(*pos)++] = byte;
    } while (value);
    return true;
}

static bool pb_write_bytes(uint8_t *buf, size_t cap, size_t *pos, uint32_t field, const uint8_t *data, size_t len)
{
    if (!pb_write_varint(buf, cap, pos, ((uint64_t)field << 3) | 2) ||
            !pb_write_varint(buf, cap, pos, len) ||
            *pos + len > cap) {
        return false;
    }
    memcpy(buf + *pos, data, len);
    *pos += len;
    return true;
}

static bool pb_write_string(uint8_t *buf, size_t cap, size_t *pos, uint32_t field, const char *text)
{
    const char *safe = text ? text : "";
    return pb_write_bytes(buf, cap, pos, field, (const uint8_t *)safe, strlen(safe));
}

static bool pb_write_header(uint8_t *buf, size_t cap, size_t *pos, const feishu_ws_header_t *header)
{
    uint8_t tmp[256];
    size_t tmp_pos = 0;
    if (!pb_write_string(tmp, sizeof(tmp), &tmp_pos, 1, header->key) ||
            !pb_write_string(tmp, sizeof(tmp), &tmp_pos, 2, header->value)) {
        return false;
    }
    return pb_write_bytes(buf, cap, pos, 5, tmp, tmp_pos);
}

static int ws_send_frame(const feishu_ws_frame_t *frame, const uint8_t *payload, size_t payload_len, int timeout_ms)
{
    uint8_t *buf = feishu_psram_malloc(1024);
    size_t pos = 0;
    int sent = -1;

    if (!frame || !s_feishu.ws_client || !buf) {
        free(buf);
        return -1;
    }
    if (!pb_write_varint(buf, 1024, &pos, ((uint64_t)1 << 3)) ||
            !pb_write_varint(buf, 1024, &pos, frame->seq_id) ||
            !pb_write_varint(buf, 1024, &pos, ((uint64_t)2 << 3)) ||
            !pb_write_varint(buf, 1024, &pos, frame->log_id) ||
            !pb_write_varint(buf, 1024, &pos, ((uint64_t)3 << 3)) ||
            !pb_write_varint(buf, 1024, &pos, frame->service) ||
            !pb_write_varint(buf, 1024, &pos, ((uint64_t)4 << 3)) ||
            !pb_write_varint(buf, 1024, &pos, frame->method)) {
        free(buf);
        return -1;
    }
    for (size_t i = 0; i < frame->header_count; i++) {
        if (!pb_write_header(buf, 1024, &pos, &frame->headers[i])) {
            free(buf);
            return -1;
        }
    }
    if (payload && payload_len > 0 && !pb_write_bytes(buf, 1024, &pos, 8, payload, payload_len)) {
        free(buf);
        return -1;
    }
    sent = esp_websocket_client_send_bin(s_feishu.ws_client, (const char *)buf, pos, timeout_ms);
    free(buf);
    return sent;
}

static char *extract_text(const char *content_json)
{
    cJSON *root = cJSON_Parse(content_json);
    if (!root) return NULL;
    cJSON *text = cJSON_GetObjectItem(root, "text");
    char *out = NULL;
    if (cJSON_IsString(text) && text->valuestring) {
        out = strdup(text->valuestring);
    }
    cJSON_Delete(root);
    return out;
}

static bool text_has(const char *text, const char *needle)
{
    return text && needle && strstr(text, needle) != NULL;
}

static bool text_wants_play(const char *text)
{
    return text_has(text, "\xE6\x92\xAD\xE6\x94\xBE") ||
           text_has(text, "\xE6\x92\xAD") ||
           text_has(text, "\xE6\x94\xBE") ||
           text_has(text, "play") ||
           text_has(text, "Play");
}

static mobile_app_command_t command_from_text(const char *text)
{
    if (!text || !text[0]) return MOBILE_APP_COMMAND_NONE;

    bool wants_stop = text_has(text, "stop") || text_has(text, "Stop") ||
                      text_has(text, "cancel") || text_has(text, "Cancel") ||
                      text_has(text, "\xE5\x81\x9C\xE6\xAD\xA2") ||
                      text_has(text, "\xE5\x81\x9C\xE4\xB8\x8B") ||
                      text_has(text, "\xE7\xBB\x93\xE6\x9D\x9F") ||
                      text_has(text, "\xE9\x80\x80\xE5\x87\xBA") ||
                      text_has(text, "\xE5\x8F\x96\xE6\xB6\x88") ||
                      text_has(text, "\xE8\xB5\xB7\xE5\xBA\x8A") ||
                      text_has(text, "\xE4\xB8\x8D\xE7\x9D\xA1");
    if (wants_stop) {
        return MOBILE_APP_COMMAND_STOP;
    }

    bool wants_sleep = text_has(text, "sleep") || text_has(text, "Sleep") ||
                       text_has(text, "bedtime") || text_has(text, "Bedtime") ||
                       text_has(text, "\xE7\x9D\xA1\xE7\x9C\xA0") ||
                       text_has(text, "\xE5\x8A\xA9\xE7\x9C\xA0") ||
                       text_has(text, "\xE7\x9D\xA1\xE8\xA7\x89") ||
                       text_has(text, "\xE6\x99\x9A\xE5\xAE\x89") ||
                       text_has(text, "\xE5\x85\xA5\xE7\x9D\xA1") ||
                       text_has(text, "\xE5\x82\xAC\xE7\x9C\xA0") ||
                       text_has(text, "\xE5\xB9\xB2\xE9\xA2\x84");
    if (wants_sleep) {
        return MOBILE_APP_COMMAND_SLEEP;
    }

    if (text_has(text, "selftest") || text_has(text, "self test") ||
        text_has(text, "\xE8\x87\xAA\xE6\xA3\x80")) {
        return MOBILE_APP_COMMAND_SELF_TEST;
    }

    bool about_screen = text_has(text, "\xE5\xB1\x8F\xE5\xB9\x95") ||
                        text_has(text, "\xE6\x98\xBE\xE7\xA4\xBA") ||
                        text_has(text, "LCD") || text_has(text, "lcd") ||
                        text_has(text, "screen") || text_has(text, "display");
    if (about_screen) {
        if (text_has(text, "\xE8\x93\x9D") || text_has(text, "blue")) {
            return MOBILE_APP_COMMAND_SCREEN_BLUE;
        }
        if (text_has(text, "\xE7\xBA\xA2") || text_has(text, "red")) {
            return MOBILE_APP_COMMAND_SCREEN_RED;
        }
        if (text_has(text, "\xE5\x85\xB3") ||
            text_has(text, "\xE6\x81\xA2\xE5\xA4\x8D") ||
            text_has(text, "\xE6\x97\xB6\xE9\x92\x9F") ||
            text_has(text, "off")) {
            return MOBILE_APP_COMMAND_SCREEN_OFF;
        }
    }

    bool about_light = text_has(text, "\xE7\x81\xAF\xE5\xB8\xA6") ||
                       text_has(text, "\xE7\x81\xAF") ||
                       text_has(text, "LED") || text_has(text, "led");
    if (about_light) {
        if (text_has(text, "\xE5\x8F\x98\xE4\xBA\xAE") ||
            text_has(text, "\xE8\xB0\x83\xE4\xBA\xAE") ||
            text_has(text, "brighter") || text_has(text, "brighten")) {
            return MOBILE_APP_COMMAND_LIGHT_BRIGHTER;
        }
        if (text_has(text, "\xE5\x8F\x98\xE6\x9A\x97") ||
            text_has(text, "\xE8\xB0\x83\xE6\x9A\x97") ||
            text_has(text, "dimmer") || text_has(text, "dim the light")) {
            return MOBILE_APP_COMMAND_LIGHT_DIMMER;
        }
        if (text_has(text, "\xE8\x93\x9D") || text_has(text, "blue")) {
            return MOBILE_APP_COMMAND_LIGHT_BLUE;
        }
        if (text_has(text, "\xE7\xBA\xA2") || text_has(text, "red")) {
            return MOBILE_APP_COMMAND_LIGHT_RED;
        }
        if (text_has(text, "\xE5\x85\xB3") || text_has(text, "off")) {
            return MOBILE_APP_COMMAND_LIGHT_OFF;
        }
        if (text_has(text, "\xE6\x89\x93\xE5\xBC\x80") ||
            text_has(text, "\xE5\xBC\x80\xE5\x90\xAF") ||
            text_has(text, "\xE5\xBC\x80\xE7\x81\xAF") ||
            text_has(text, "turn on") || text_has(text, "Turn on")) {
            return MOBILE_APP_COMMAND_LIGHT_YELLOW;
        }
    }

    if (text_has(text, "mid_sleep") || text_has(text, "intervention") ||
        text_has(text, "\xE4\xB8\xAD\xE9\x80\x94\xE5\xB9\xB2\xE9\xA2\x84")) {
        if (text_has(text, "\xE7\xA6\xBB\xE5\xBA\x8A") ||
            text_has(text, "\xE8\xB5\xB7\xE5\xA4\x9C") ||
            text_has(text, "out_of_bed")) {
            return MOBILE_APP_COMMAND_MID_SLEEP_TEST_OUT_OF_BED;
        }
        if (text_has(text, "\xE7\xBF\xBB\xE8\xBA\xAB") ||
            text_has(text, "\xE4\xB8\x8D\xE5\xAE\x89") ||
            text_has(text, "restless")) {
            return MOBILE_APP_COMMAND_MID_SLEEP_TEST_RESTLESS;
        }
        if (text_has(text, "\xE5\x81\x9A\xE6\xA2\xA6") ||
            text_has(text, "\xE6\xA2\xA6") ||
            text_has(text, "\xE9\x86\x92") ||
            text_has(text, "arousal")) {
            return MOBILE_APP_COMMAND_MID_SLEEP_TEST_AROUSAL;
        }
        return MOBILE_APP_COMMAND_MID_SLEEP_TEST_MINOR;
    }

    if (text_wants_play(text)) {
        if (text_has(text, "ocean") || text_has(text, "tide") ||
            text_has(text, "\xE6\xB5\xB7\xE6\xB5\xAA") ||
            text_has(text, "\xE6\xBD\xAE\xE6\xB1\x90")) {
            return MOBILE_APP_COMMAND_SCENE_OCEAN_PLAY;
        }
        if (text_has(text, "forest") ||
            text_has(text, "\xE6\xA3\xAE\xE6\x9E\x97") ||
            text_has(text, "\xE6\x9E\x97\xE6\xB5\xB7")) {
            return MOBILE_APP_COMMAND_SCENE_FOREST_PLAY;
        }
        if (text_has(text, "rain") ||
            text_has(text, "\xE9\x9B\xA8\xE5\xA4\x9C") ||
            text_has(text, "\xE4\xB8\x8B\xE9\x9B\xA8")) {
            return MOBILE_APP_COMMAND_SCENE_RAIN_PLAY;
        }
        if (text_has(text, "zen") ||
            text_has(text, "\xE7\xA6\x85") ||
            text_has(text, "\xE5\x86\xA5\xE6\x83\xB3")) {
            return MOBILE_APP_COMMAND_SCENE_ZEN_PLAY;
        }
        if (text_has(text, "PPM") || text_has(text, "ppm") ||
            text_has(text, "3104") ||
            text_has(text, "\xE5\xBE\x8B\xE5\x8A\xA8")) {
            return MOBILE_APP_COMMAND_SCENE_PPM_PLAY;
        }
    }

    if (text_has(text, "music") || text_has(text, "Music") || text_has(text, "MP3") ||
        text_has(text, "mp3") || text_has(text, "\xE9\x9F\xB3\xE4\xB9\x90") ||
        text_has(text, "\xE6\xAD\x8C\xE6\x9B\xB2")) {
        return MOBILE_APP_COMMAND_AUDIO_MUSIC;
    }
    if (text_has(text, "story") || text_has(text, "Story") ||
        text_has(text, "\xE6\x95\x85\xE4\xBA\x8B")) {
        return MOBILE_APP_COMMAND_STORY;
    }
    if (text_has(text, "noise") || text_has(text, "Noise") ||
        text_has(text, "\xE7\x99\xBD\xE5\x99\xAA\xE5\xA3\xB0") ||
        text_has(text, "\xE5\x99\xAA\xE5\xA3\xB0")) {
        return MOBILE_APP_COMMAND_AUDIO_NOISE;
    }
    if (text_has(text, "breathing") || text_has(text, "Breathing") ||
        text_has(text, "\xE5\x91\xBC\xE5\x90\xB8")) {
        return MOBILE_APP_COMMAND_AUDIO_BREATHING;
    }
    if (text_has(text, "ocean") || text_has(text, "tide") ||
        text_has(text, "\xE6\xB5\xB7\xE6\xB5\xAA") ||
        text_has(text, "\xE6\xBD\xAE\xE6\xB1\x90")) {
        if (text_wants_play(text)) return MOBILE_APP_COMMAND_SCENE_OCEAN_PLAY;
        return MOBILE_APP_COMMAND_SCENE_OCEAN;
    }
    if (text_has(text, "forest") ||
        text_has(text, "\xE6\xA3\xAE\xE6\x9E\x97") ||
        text_has(text, "\xE6\x9E\x97\xE6\xB5\xB7")) {
        if (text_wants_play(text)) return MOBILE_APP_COMMAND_SCENE_FOREST_PLAY;
        return MOBILE_APP_COMMAND_SCENE_FOREST;
    }
    if (text_has(text, "rain") ||
        text_has(text, "\xE9\x9B\xA8\xE5\xA4\x9C") ||
        text_has(text, "\xE4\xB8\x8B\xE9\x9B\xA8")) {
        if (text_wants_play(text)) return MOBILE_APP_COMMAND_SCENE_RAIN_PLAY;
        return MOBILE_APP_COMMAND_SCENE_RAIN;
    }
    if (text_has(text, "zen") ||
        text_has(text, "\xE7\xA6\x85") ||
        text_has(text, "\xE5\x86\xA5\xE6\x83\xB3")) {
        if (text_wants_play(text)) return MOBILE_APP_COMMAND_SCENE_ZEN_PLAY;
        return MOBILE_APP_COMMAND_SCENE_ZEN;
    }
    if (text_has(text, "PPM") || text_has(text, "ppm") ||
        text_has(text, "3104") ||
        text_has(text, "\xE5\xBE\x8B\xE5\x8A\xA8")) {
        if (text_wants_play(text)) return MOBILE_APP_COMMAND_SCENE_PPM_PLAY;
        return MOBILE_APP_COMMAND_SCENE_PPM;
    }
    if (text_has(text, "4-7-8") || text_has(text, "478")) {
        return MOBILE_APP_COMMAND_BREATH_DEEP;
    }
    if (text_has(text, "box") || text_has(text, "4-4") ||
        text_has(text, "\xE7\xAE\xB1\xE5\xBC\x8F")) {
        return MOBILE_APP_COMMAND_BREATH_BOX;
    }
    if (text_has(text, "5-5") || text_has(text, "\xE8\x88\x92\xE7\xBC\x93")) {
        return MOBILE_APP_COMMAND_BREATH_RELAX;
    }

    if (text_has(text, "屏幕") || text_has(text, "显示")) {
        if (text_has(text, "蓝")) return MOBILE_APP_COMMAND_SCREEN_BLUE;
        if (text_has(text, "红")) return MOBILE_APP_COMMAND_SCREEN_RED;
        if (text_has(text, "关闭") || text_has(text, "关掉") ||
                text_has(text, "恢复") || text_has(text, "时钟")) {
            return MOBILE_APP_COMMAND_SCREEN_OFF;
        }
    }
    if (text_has(text, "灯带") || text_has(text, "灯") || text_has(text, "LED") || text_has(text, "led")) {
        if (text_has(text, "蓝")) return MOBILE_APP_COMMAND_LIGHT_BLUE;
        if (text_has(text, "红")) return MOBILE_APP_COMMAND_LIGHT_RED;
        if (text_has(text, "关闭") || text_has(text, "关掉") || text_has(text, "关灯")) {
            return MOBILE_APP_COMMAND_LIGHT_OFF;
        }
    }
    if (text_has(text, "开始") && (text_has(text, "睡眠") || text_has(text, "助眠") || text_has(text, "干预"))) {
        return MOBILE_APP_COMMAND_SLEEP;
    }
    if (text_has(text, "停止") || text_has(text, "停下") || text_has(text, "取消")) {
        return MOBILE_APP_COMMAND_STOP;
    }
    if (text_has(text, "自检")) {
        return MOBILE_APP_COMMAND_SELF_TEST;
    }
    return MOBILE_APP_COMMAND_NONE;
}

static mobile_app_command_t command_from_llm_name(const char *name)
{
    if (!name || !name[0] || strcmp(name, "none") == 0) return MOBILE_APP_COMMAND_NONE;
    if (strcmp(name, "screen.red") == 0) return MOBILE_APP_COMMAND_SCREEN_RED;
    if (strcmp(name, "screen.blue") == 0) return MOBILE_APP_COMMAND_SCREEN_BLUE;
    if (strcmp(name, "screen.off") == 0) return MOBILE_APP_COMMAND_SCREEN_OFF;
    if (strcmp(name, "light.red") == 0) return MOBILE_APP_COMMAND_LIGHT_RED;
    if (strcmp(name, "light.blue") == 0) return MOBILE_APP_COMMAND_LIGHT_BLUE;
    if (strcmp(name, "light.off") == 0) return MOBILE_APP_COMMAND_LIGHT_OFF;
    if (strcmp(name, "light.brighter") == 0) return MOBILE_APP_COMMAND_LIGHT_BRIGHTER;
    if (strcmp(name, "light.dimmer") == 0) return MOBILE_APP_COMMAND_LIGHT_DIMMER;
    if (strcmp(name, "sleep.start") == 0) return MOBILE_APP_COMMAND_SLEEP;
    if (strcmp(name, "sleep.stop") == 0) return MOBILE_APP_COMMAND_STOP;
    if (strcmp(name, "audio.music") == 0 || strcmp(name, "music.play") == 0) return MOBILE_APP_COMMAND_AUDIO_MUSIC;
    if (strcmp(name, "audio.noise") == 0 || strcmp(name, "audio.pink_noise") == 0) return MOBILE_APP_COMMAND_AUDIO_NOISE;
    if (strcmp(name, "audio.breathing") == 0 || strcmp(name, "audio.stereo_breathing") == 0) return MOBILE_APP_COMMAND_AUDIO_BREATHING;
    if (strcmp(name, "story.play") == 0 || strcmp(name, "story") == 0) return MOBILE_APP_COMMAND_STORY;
    if (strcmp(name, "scene.ppm.play") == 0 || strcmp(name, "scene.default.play") == 0) return MOBILE_APP_COMMAND_SCENE_PPM_PLAY;
    if (strcmp(name, "scene.ocean.play") == 0 || strcmp(name, "scene.tide.play") == 0) return MOBILE_APP_COMMAND_SCENE_OCEAN_PLAY;
    if (strcmp(name, "scene.forest.play") == 0) return MOBILE_APP_COMMAND_SCENE_FOREST_PLAY;
    if (strcmp(name, "scene.rain.play") == 0 || strcmp(name, "scene.rainy.play") == 0) return MOBILE_APP_COMMAND_SCENE_RAIN_PLAY;
    if (strcmp(name, "scene.zen.play") == 0 || strcmp(name, "scene.meditation.play") == 0) return MOBILE_APP_COMMAND_SCENE_ZEN_PLAY;
    if (strcmp(name, "scene.ppm") == 0 || strcmp(name, "scene.ppm3104e") == 0) return MOBILE_APP_COMMAND_SCENE_PPM;
    if (strcmp(name, "scene.ocean") == 0 || strcmp(name, "scene.tide") == 0) return MOBILE_APP_COMMAND_SCENE_OCEAN;
    if (strcmp(name, "scene.forest") == 0) return MOBILE_APP_COMMAND_SCENE_FOREST;
    if (strcmp(name, "scene.rain") == 0 || strcmp(name, "scene.rainy") == 0) return MOBILE_APP_COMMAND_SCENE_RAIN;
    if (strcmp(name, "scene.zen") == 0 || strcmp(name, "scene.meditation") == 0) return MOBILE_APP_COMMAND_SCENE_ZEN;
    if (strcmp(name, "scene.empty") == 0 || strcmp(name, "scene.custom") == 0) return MOBILE_APP_COMMAND_SCENE_EMPTY;
    if (strcmp(name, "breath.relax") == 0 || strcmp(name, "breath.5-5") == 0) return MOBILE_APP_COMMAND_BREATH_RELAX;
    if (strcmp(name, "breath.box") == 0 || strcmp(name, "breath.4-4") == 0) return MOBILE_APP_COMMAND_BREATH_BOX;
    if (strcmp(name, "breath.deep") == 0 || strcmp(name, "breath.4-7-8") == 0) return MOBILE_APP_COMMAND_BREATH_DEEP;
    if (strcmp(name, "mid_sleep.test_minor") == 0 || strcmp(name, "intervention.test_minor") == 0) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_MINOR;
    if (strcmp(name, "mid_sleep.test_restless") == 0 || strcmp(name, "intervention.test_restless") == 0) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_RESTLESS;
    if (strcmp(name, "mid_sleep.test_arousal") == 0 || strcmp(name, "intervention.test_arousal") == 0) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_AROUSAL;
    if (strcmp(name, "mid_sleep.test_out_of_bed") == 0 || strcmp(name, "intervention.test_out_of_bed") == 0) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_OUT_OF_BED;
    if (strcmp(name, "self_test") == 0) return MOBILE_APP_COMMAND_SELF_TEST;
    return MOBILE_APP_COMMAND_NONE;
}

static bool parse_llm_route_json(const char *text, llm_route_t *route)
{
    const char *start = text ? strchr(text, '{') : NULL;
    const char *end = text ? strrchr(text, '}') : NULL;
    if (!start || !end || end <= start || !route) return false;

    size_t len = (size_t)(end - start + 1);
    char *json = strndup(start, len);
    if (!json) return false;
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return false;

    cJSON *cmd = cJSON_GetObjectItem(root, "command");
    cJSON *reply = cJSON_GetObjectItem(root, "reply");
    route->command = cJSON_IsString(cmd) ? command_from_llm_name(cmd->valuestring) : MOBILE_APP_COMMAND_NONE;
    if (cJSON_IsString(reply) && reply->valuestring) {
        snprintf(route->reply, sizeof(route->reply), "%s", reply->valuestring);
    }
    cJSON_Delete(root);
    return true;
}

static esp_err_t parse_openai_route_response(const char *response, llm_route_t *route)
{
    cJSON *root = cJSON_Parse(response);
    if (!root) return ESP_FAIL;
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = cJSON_IsObject(first) ? cJSON_GetObjectItem(first, "message") : NULL;
    cJSON *content = cJSON_IsObject(message) ? cJSON_GetObjectItem(message, "content") : NULL;
    bool ok = cJSON_IsString(content) && parse_llm_route_json(content->valuestring, route);
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t parse_anthropic_route_response(const char *response, llm_route_t *route)
{
    cJSON *root = cJSON_Parse(response);
    if (!root) return ESP_FAIL;
    cJSON *content = cJSON_GetObjectItem(root, "content");
    cJSON *first = cJSON_IsArray(content) ? cJSON_GetArrayItem(content, 0) : NULL;
    cJSON *text = cJSON_IsObject(first) ? cJSON_GetObjectItem(first, "text") : NULL;
    bool ok = cJSON_IsString(text) && parse_llm_route_json(text->valuestring, route);
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_FAIL;
}

static const char *LLM_ROUTER_PROMPT =
    "You are the DreamGuardian sleep device brain. Read the user's Chinese or English message and return ONLY compact JSON. "
    "Schema: {\"command\":\"...\",\"reply\":\"...\"}. "
    "Allowed command values: screen.red, screen.blue, screen.off, light.red, light.blue, light.off, sleep.start, sleep.stop, audio.music, audio.noise, audio.breathing, story.play, scene.ppm, scene.ocean, scene.forest, scene.rain, scene.zen, scene.empty, scene.ppm.play, scene.ocean.play, scene.forest.play, scene.rain.play, scene.zen.play, breath.relax, breath.box, breath.deep, self_test, none. "
    "Map screen/display color requests to screen.*, LED/light strip requests to light.*, sleep aid/intervention/start bedtime mode to sleep.start, stop/cancel/quiet to sleep.stop or light.off/screen.off as appropriate. "
    "Map requests to play music, melody, gentle song, or speaker music to audio.music. Map white noise, pink noise, rain-like masking, or ambient noise to audio.noise. Map breathing guidance sound to audio.breathing. Map tell a story or bedtime story to story.play. "
    "Map requests for default rhythm/soft stream/birds/chimes to scene.ppm, ocean/tide/wave to scene.ocean, forest/birds/stream to scene.forest, rain/night rain to scene.rain, zen/meditation/chimes to scene.zen. If the user asks to play a named scene from the speaker, use scene.<name>.play. Map 5-5 breathing to breath.relax, box breathing/4-4 to breath.box, 4-7-8/deep breathing to breath.deep. "
    "For story.play, include a short calming bedtime story text in reply while the device plays soft background audio. "
    "If the user asks for sleep advice, planning, statistics, or conversation without an immediate device action, use command none and reply as a concise helpful sleep assistant in Chinese. "
    "Never invent commands. The reply should be short and natural Chinese.";

static esp_err_t llm_route_text(const char *text, llm_route_t *route)
{
    llm_config_t cfg = {0};
    char url[LLM_URL_LEN + 32];
    char *body_str = NULL;
    char *response = NULL;
    int status = 0;
    esp_err_t err = ESP_FAIL;

    if (!text || !route) return ESP_ERR_INVALID_ARG;
    memset(route, 0, sizeof(*route));
    ESP_RETURN_ON_ERROR(llm_load_config(&cfg), TAG, "llm config");

    cJSON *body = cJSON_CreateObject();
    if (!body) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(body, "model", cfg.model);

    if (strstr(cfg.backend_type, "anthropic")) {
        join_api_path(url, sizeof(url), cfg.base_url, "/messages");
        cJSON_AddStringToObject(body, "system", LLM_ROUTER_PROMPT);
        cJSON_AddNumberToObject(body, "max_tokens", 192);
        cJSON *messages = cJSON_AddArrayToObject(body, "messages");
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", text);
        cJSON_AddItemToArray(messages, msg);
    } else {
        join_api_path(url, sizeof(url), cfg.base_url, "/chat/completions");
        const char *tokens_field = cfg.max_tokens_field[0] ? cfg.max_tokens_field : "max_tokens";
        cJSON_AddNumberToObject(body, tokens_field, 192);
        cJSON *messages = cJSON_AddArrayToObject(body, "messages");
        cJSON *sys = cJSON_CreateObject();
        cJSON *usr = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", LLM_ROUTER_PROMPT);
        cJSON_AddStringToObject(usr, "role", "user");
        cJSON_AddStringToObject(usr, "content", text);
        cJSON_AddItemToArray(messages, sys);
        cJSON_AddItemToArray(messages, usr);
    }

    body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return ESP_ERR_NO_MEM;

    err = llm_http_json(url, &cfg, body_str, &response, &status);
    if (err == ESP_OK) {
        err = strstr(cfg.backend_type, "anthropic") ?
              parse_anthropic_route_response(response, route) :
              parse_openai_route_response(response, route);
        set_status_result(err == ESP_OK ? "llm_route_ok" : "llm_parse_failed", status);
    }

    free(response);
    free(body_str);
    return err;
}

static const char *command_reply(mobile_app_command_t command, esp_err_t err)
{
    if (err != ESP_OK) return "OK: command recognized, but device execution failed.";
    switch (command) {
    case MOBILE_APP_COMMAND_NONE:
        return "无对应设备调用：该指令不在设备能力白名单中，系统未执行任何硬件操作。";
    case MOBILE_APP_COMMAND_SLEEP:
        return "OK: sleep mode started.";
    case MOBILE_APP_COMMAND_STOP:
        return "OK: sleep mode stopped.";
    case MOBILE_APP_COMMAND_SELF_TEST:
        return "OK: self test started.";
    case MOBILE_APP_COMMAND_LIGHT_RED:
        return "OK: light set to red.";
    case MOBILE_APP_COMMAND_LIGHT_BLUE:
        return "OK: light set to blue.";
    case MOBILE_APP_COMMAND_LIGHT_GREEN:
        return "OK: light set to green.";
    case MOBILE_APP_COMMAND_LIGHT_YELLOW:
        return "OK: light strip turned on with warm light.";
    case MOBILE_APP_COMMAND_LIGHT_OFF:
        return "OK: light turned off.";
    case MOBILE_APP_COMMAND_LIGHT_BRIGHTER:
        return "OK: light brightness increased.";
    case MOBILE_APP_COMMAND_LIGHT_DIMMER:
        return "OK: light brightness decreased.";
    case MOBILE_APP_COMMAND_SCREEN_RED:
        return "OK: screen set to red.";
    case MOBILE_APP_COMMAND_SCREEN_BLUE:
        return "OK: screen set to blue.";
    case MOBILE_APP_COMMAND_SCREEN_OFF:
        return "OK: screen returned to normal.";
    default:
        break;
    }
    if (err != ESP_OK) return "指令已识别，但设备执行失败。";
    switch (command) {
    case MOBILE_APP_COMMAND_SCREEN_RED:
        return "已把屏幕切换为红色。";
    case MOBILE_APP_COMMAND_SCREEN_BLUE:
        return "已把屏幕切换为蓝色。";
    case MOBILE_APP_COMMAND_SCREEN_OFF:
        return "已恢复屏幕时钟显示。";
    case MOBILE_APP_COMMAND_LIGHT_RED:
        return "已把灯带切换为红色。";
    case MOBILE_APP_COMMAND_LIGHT_BLUE:
        return "已把灯带切换为蓝色。";
    case MOBILE_APP_COMMAND_LIGHT_OFF:
        return "已关闭灯带手动颜色。";
    case MOBILE_APP_COMMAND_SLEEP:
        return "已开始睡眠干预。";
    case MOBILE_APP_COMMAND_STOP:
        return "已停止当前干预。";
    case MOBILE_APP_COMMAND_AUDIO_MUSIC:
        return "已开始通过扬声器播放音乐。";
    case MOBILE_APP_COMMAND_AUDIO_NOISE:
        return "已开始播放低音量白噪声。";
    case MOBILE_APP_COMMAND_AUDIO_BREATHING:
        return "已开始播放呼吸引导音。";
    case MOBILE_APP_COMMAND_STORY:
        return "已开始睡前故事模式。";
    case MOBILE_APP_COMMAND_SCENE_PPM_PLAY:
        return "正在为您播放默认律动助眠音乐。";
    case MOBILE_APP_COMMAND_SCENE_OCEAN_PLAY:
        return "正在为您播放深海潮汐助眠音乐。";
    case MOBILE_APP_COMMAND_SCENE_FOREST_PLAY:
        return "正在为您播放林海晨曦助眠音乐。";
    case MOBILE_APP_COMMAND_SCENE_RAIN_PLAY:
        return "正在为您播放雨夜入眠助眠音乐。";
    case MOBILE_APP_COMMAND_SCENE_ZEN_PLAY:
        return "正在为您播放太虚禅音助眠音乐。";
    case MOBILE_APP_COMMAND_SCENE_PPM:
        return "已切换为默认律动助眠场景。";
    case MOBILE_APP_COMMAND_SCENE_OCEAN:
        return "已切换为深海潮汐助眠场景。";
    case MOBILE_APP_COMMAND_SCENE_FOREST:
        return "已切换为林海晨曦助眠场景。";
    case MOBILE_APP_COMMAND_SCENE_RAIN:
        return "已切换为雨夜入眠助眠场景。";
    case MOBILE_APP_COMMAND_SCENE_ZEN:
        return "已切换为太虚禅音助眠场景。";
    case MOBILE_APP_COMMAND_SCENE_EMPTY:
        return "已切换为放空归零自定义场景。";
    case MOBILE_APP_COMMAND_BREATH_RELAX:
        return "已切换为舒缓5-5呼吸引导。";
    case MOBILE_APP_COMMAND_BREATH_BOX:
        return "已切换为箱式4-4呼吸引导。";
    case MOBILE_APP_COMMAND_BREATH_DEEP:
        return "已切换为深眠4-7-8呼吸引导。";
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_MINOR:
        return "\xE5\xB7\xB2\xE8\xA7\xA6\xE5\x8F\x91\xE8\xBD\xBB\xE5\xBE\xAE\xE6\x89\xB0\xE5\x8A\xA8\xE4\xB8\xAD\xE9\x80\x94\xE5\xB9\xB2\xE9\xA2\x84\xE3\x80\x82";
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_RESTLESS:
        return "\xE5\xB7\xB2\xE8\xA7\xA6\xE5\x8F\x91\xE7\xBF\xBB\xE8\xBA\xAB\x2F\xE7\x9D\xA1\xE7\x9C\xA0\xE4\xB8\x8D\xE5\xAE\x89\xE4\xB8\xAD\xE9\x80\x94\xE5\xB9\xB2\xE9\xA2\x84\xE3\x80\x82";
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_AROUSAL:
        return "\xE5\xB7\xB2\xE8\xA7\xA6\xE5\x8F\x91\xE9\xAB\x98\xE5\x94\xA4\xE9\x86\x92\xE9\xA3\x8E\xE9\x99\xA9\xE4\xB8\xAD\xE9\x80\x94\xE5\xB9\xB2\xE9\xA2\x84\xE3\x80\x82";
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_OUT_OF_BED:
        return "\xE5\xB7\xB2\xE8\xA7\xA6\xE5\x8F\x91\xE7\xA6\xBB\xE5\xBA\x8A\xE5\xA4\x9C\xE8\xB7\xAF\xE7\x81\xAF\xE5\xB9\xB2\xE9\xA2\x84\xE3\x80\x82";
    case MOBILE_APP_COMMAND_SELF_TEST:
        return "已开始硬件自检。";
    default:
        return "我已收到消息。当前支持：屏幕/灯带颜色、播放音乐、白噪声、呼吸引导音、讲故事、开始或停止睡眠干预。";
    }
}

static const char s_font_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-/.+";
static const uint8_t s_font_5x7[][5] = {
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},
    {0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e},{0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},
    {0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
    {0x08,0x08,0x08,0x08,0x08},{0x20,0x10,0x08,0x04,0x02},
    {0x00,0x60,0x60,0x00,0x00},{0x08,0x08,0x3e,0x08,0x08},
};

static void bmp_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void bmp_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void bmp_pixel(uint8_t *bmp, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (!bmp || x < 0 || y < 0 || x >= FEISHU_REPORT_WIDTH || y >= FEISHU_REPORT_HEIGHT) return;
    const int stride = (FEISHU_REPORT_WIDTH * 3 + 3) & ~3;
    size_t pos = 54U + (size_t)(FEISHU_REPORT_HEIGHT - 1 - y) * stride + (size_t)x * 3U;
    bmp[pos] = b;
    bmp[pos + 1] = g;
    bmp[pos + 2] = r;
}

static void bmp_line(uint8_t *bmp, int x0, int y0, int x1, int y1,
                     uint8_t r, uint8_t g, uint8_t b)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        bmp_pixel(bmp, x0, y0, r, g, b);
        bmp_pixel(bmp, x0 + 1, y0, r, g, b);
        if (x0 == x1 && y0 == y1) break;
        int e2 = error * 2;
        if (e2 >= dy) { error += dy; x0 += sx; }
        if (e2 <= dx) { error += dx; y0 += sy; }
    }
}

static void bmp_rect(uint8_t *bmp, int x, int y, int w, int h,
                     uint8_t r, uint8_t g, uint8_t b)
{
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) bmp_pixel(bmp, xx, yy, r, g, b);
    }
}

static void bmp_text(uint8_t *bmp, int x, int y, const char *text,
                     int scale, uint8_t r, uint8_t g, uint8_t b)
{
    if (!text || scale < 1) return;
    for (; *text; ++text, x += 6 * scale) {
        if (*text == ' ') continue;
        const char *found = strchr(s_font_chars, *text);
        if (!found) continue;
        const uint8_t *glyph = s_font_5x7[found - s_font_chars];
        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row < 7; ++row) {
                if (glyph[col] & (1U << row)) {
                    bmp_rect(bmp, x + col * scale, y + row * scale,
                             scale, scale, r, g, b);
                }
            }
        }
    }
}

static uint8_t *make_sleep_report_bmp(uint8_t requested_days,
                                      const sleep_daily_summary_t *days,
                                      size_t count, size_t *out_size)
{
    const int stride = (FEISHU_REPORT_WIDTH * 3 + 3) & ~3;
    size_t size = 54U + (size_t)stride * FEISHU_REPORT_HEIGHT;
    uint8_t *bmp = feishu_psram_calloc(1, size);
    if (!bmp) return NULL;
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp_u32(bmp + 2, (uint32_t)size);
    bmp_u32(bmp + 10, 54);
    bmp_u32(bmp + 14, 40);
    bmp_u32(bmp + 18, FEISHU_REPORT_WIDTH);
    bmp_u32(bmp + 22, FEISHU_REPORT_HEIGHT);
    bmp_u16(bmp + 26, 1);
    bmp_u16(bmp + 28, 24);
    bmp_u32(bmp + 34, (uint32_t)(stride * FEISHU_REPORT_HEIGHT));
    bmp_rect(bmp, 0, 0, FEISHU_REPORT_WIDTH, FEISHU_REPORT_HEIGHT, 12, 18, 30);
    bmp_text(bmp, 24, 12, "DREAMGUARDIAN SLEEP REPORT", 2, 238, 242, 248);
    char subtitle[32];
    snprintf(subtitle, sizeof(subtitle), "LAST %u DAYS", (unsigned)requested_days);
    bmp_text(bmp, 24, 34, subtitle, 1, 156, 170, 191);

    if (count == 0) {
        bmp_text(bmp, 210, 165, "NO DATA", 3, 255, 166, 69);
        *out_size = size;
        return bmp;
    }

    const int left = 58, right = 574;
    const int score_top = 72, score_bottom = 182;
    const int sleep_top = 222, sleep_bottom = 322;
    for (int i = 0; i <= 4; ++i) {
        int y1 = score_top + (score_bottom - score_top) * i / 4;
        int y2 = sleep_top + (sleep_bottom - sleep_top) * i / 4;
        bmp_line(bmp, left, y1, right, y1, 48, 60, 78);
        bmp_line(bmp, left, y2, right, y2, 48, 60, 78);
    }
    bmp_text(bmp, left, 57, "QUALITY SCORE", 1, 255, 166, 69);
    bmp_text(bmp, 28, score_top - 3, "100", 1, 156, 170, 191);
    bmp_text(bmp, 40, score_bottom - 6, "0", 1, 156, 170, 191);
    bmp_text(bmp, left, 207, "SLEEP MIN", 1, 75, 180, 255);

    uint16_t sleep_max = 480;
    for (size_t i = 0; i < count; ++i) if (days[i].sleep_min > sleep_max) sleep_max = days[i].sleep_min;
    sleep_max = (uint16_t)(((sleep_max + 59U) / 60U) * 60U);
    char max_text[8];
    snprintf(max_text, sizeof(max_text), "%u", (unsigned)sleep_max);
    bmp_text(bmp, 22, sleep_top - 3, max_text, 1, 156, 170, 191);
    bmp_text(bmp, 40, sleep_bottom - 6, "0", 1, 156, 170, 191);

    int prev_x = 0, prev_score_y = 0, prev_sleep_y = 0;
    for (size_t i = 0; i < count; ++i) {
        int x = count == 1 ? (left + right) / 2 : left + (int)i * (right - left) / (int)(count - 1);
        int score_y = score_bottom - (int)days[i].quality * (score_bottom - score_top) / 100;
        int sleep_y = sleep_bottom - (int)days[i].sleep_min * (sleep_bottom - sleep_top) / sleep_max;
        if (i > 0) {
            bmp_line(bmp, prev_x, prev_score_y, x, score_y, 255, 166, 69);
            bmp_line(bmp, prev_x, prev_sleep_y, x, sleep_y, 75, 180, 255);
        }
        bmp_rect(bmp, x - 3, score_y - 3, 7, 7, 255, 166, 69);
        bmp_rect(bmp, x - 3, sleep_y - 3, 7, 7, 75, 180, 255);
        if (count <= 7 || i == 0 || i + 1 == count || (i % 2) == 0) {
            char date[8];
            snprintf(date, sizeof(date), "%02u/%02u",
                     (unsigned)((days[i].date_key / 100U) % 100U),
                     (unsigned)(days[i].date_key % 100U));
            int tx = x - 15;
            if (tx < 1) tx = 1;
            if (tx > FEISHU_REPORT_WIDTH - 38) tx = FEISHU_REPORT_WIDTH - 38;
            bmp_text(bmp, tx, 340, date, 1, 180, 192, 210);
        }
        prev_x = x; prev_score_y = score_y; prev_sleep_y = sleep_y;
    }
    *out_size = size;
    return bmp;
}

static esp_err_t upload_report_image(const uint8_t *image, size_t image_size,
                                     char *image_key, size_t image_key_size)
{
    static const char boundary[] = "----DreamGuardianReportBoundary";
    char prefix[256];
    char suffix[64];
    int prefix_len = snprintf(prefix, sizeof(prefix),
                              "--%s\r\nContent-Disposition: form-data; name=\"image_type\"\r\n\r\nmessage\r\n"
                              "--%s\r\nContent-Disposition: form-data; name=\"image\"; filename=\"sleep-report.bmp\"\r\n"
                              "Content-Type: image/bmp\r\n\r\n", boundary, boundary);
    int suffix_len = snprintf(suffix, sizeof(suffix), "\r\n--%s--\r\n", boundary);
    if (prefix_len <= 0 || suffix_len <= 0) return ESP_FAIL;
    size_t body_size = (size_t)prefix_len + image_size + (size_t)suffix_len;
    uint8_t *body = feishu_psram_malloc(body_size);
    if (!body) return ESP_ERR_NO_MEM;
    memcpy(body, prefix, (size_t)prefix_len);
    memcpy(body + prefix_len, image, image_size);
    memcpy(body + prefix_len + image_size, suffix, (size_t)suffix_len);

    esp_err_t err = get_tenant_token();
    if (err != ESP_OK) { free(body); return err; }
    feishu_resp_t resp = {0};
    err = resp_init(&resp);
    if (err != ESP_OK) { free(body); return err; }
    esp_http_client_config_t cfg = {
        .url = FEISHU_UPLOAD_IMAGE_URL,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .addr_type = HTTP_ADDR_TYPE_INET,
        .buffer_size = 2048,
        .buffer_size_tx = 4096,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(resp.buf); free(body); return ESP_ERR_NO_MEM; }
    char auth[FEISHU_TOKEN_LEN + 16];
    char content_type[96];
    snprintf(auth, sizeof(auth), "Bearer %s", s_feishu.tenant_token);
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, (const char *)body, (int)body_size);
    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err == ESP_OK && status == 200) {
        cJSON *root = cJSON_Parse(resp.buf);
        cJSON *code = root ? cJSON_GetObjectItem(root, "code") : NULL;
        cJSON *data = root ? cJSON_GetObjectItem(root, "data") : NULL;
        cJSON *key = cJSON_IsObject(data) ? cJSON_GetObjectItem(data, "image_key") : NULL;
        if (cJSON_IsNumber(code) && code->valueint == 0 && cJSON_IsString(key)) {
            snprintf(image_key, image_key_size, "%s", key->valuestring);
            cJSON_Delete(root);
            free(resp.buf);
            return ESP_OK;
        }
        cJSON_Delete(root);
    }
    printf("DG Feishu: report image upload failed err=%s http=%d body=%.160s\r\n",
           esp_err_to_name(err), status, resp.buf ? resp.buf : "");
    free(resp.buf);
    return err == ESP_OK ? ESP_FAIL : err;
}

static esp_err_t send_image(const char *chat_id, const char *image_key)
{
    const char *id_type = strncmp(chat_id, "ou_", 3) == 0 ? "open_id" : "chat_id";
    char url[FEISHU_URL_LEN];
    snprintf(url, sizeof(url), "%s?receive_id_type=%s", FEISHU_SEND_MSG_URL, id_type);
    cJSON *body = cJSON_CreateObject();
    cJSON *content = cJSON_CreateObject();
    if (!body || !content) { cJSON_Delete(body); cJSON_Delete(content); return ESP_ERR_NO_MEM; }
    cJSON_AddStringToObject(content, "image_key", image_key);
    char *content_str = cJSON_PrintUnformatted(content);
    cJSON_AddStringToObject(body, "receive_id", chat_id);
    cJSON_AddStringToObject(body, "msg_type", "image");
    cJSON_AddStringToObject(body, "content", content_str ? content_str : "{}");
    char *body_str = cJSON_PrintUnformatted(body);
    char *response = NULL;
    esp_err_t err = body_str ? api_call(url, "POST", body_str, &response) : ESP_ERR_NO_MEM;
    free(response); free(body_str); free(content_str);
    cJSON_Delete(content); cJSON_Delete(body);
    return err;
}

static esp_err_t send_text(const char *chat_id, const char *text)
{
    const char *id_type = strncmp(chat_id, "ou_", 3) == 0 ? "open_id" : "chat_id";
    char url[FEISHU_URL_LEN];
    cJSON *body = cJSON_CreateObject();
    cJSON *content = cJSON_CreateObject();
    char *content_str = NULL;
    char *body_str = NULL;
    char *response = NULL;
    esp_err_t err = ESP_ERR_NO_MEM;

    if (!body || !content) goto cleanup;
    snprintf(url, sizeof(url), "%s?receive_id_type=%s", FEISHU_SEND_MSG_URL, id_type);
    cJSON_AddStringToObject(content, "text", text ? text : "");
    content_str = cJSON_PrintUnformatted(content);
    if (!content_str) goto cleanup;

    cJSON_AddStringToObject(body, "receive_id", chat_id);
    cJSON_AddStringToObject(body, "msg_type", "text");
    cJSON_AddStringToObject(body, "content", content_str);
    body_str = cJSON_PrintUnformatted(body);
    if (!body_str) goto cleanup;

    err = api_call(url, "POST", body_str, &response);
    free(response);

cleanup:
    free(content_str);
    free(body_str);
    cJSON_Delete(content);
    cJSON_Delete(body);
    return err;
}

static void reply_task(void *arg)
{
    (void)arg;
    feishu_reply_msg_t *msg = feishu_psram_calloc(1, sizeof(*msg));
    if (!msg) {
        set_status_result("reply_worker_alloc_failed", 0);
        s_feishu.reply_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    while (!s_feishu.stop_requested) {
        if (!s_feishu.reply_queue ||
                xQueueReceive(s_feishu.reply_queue, msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        if (msg->route_id[0]) {
            esp_err_t err = ESP_ERR_INVALID_ARG;
            if (msg->type == FEISHU_REPLY_SLEEP_REPORT) {
                sleep_daily_summary_t *days = feishu_psram_calloc(SLEEP_LOG_HISTORY_MAX_DAYS,
                                                                  sizeof(*days));
                size_t count = days ? sleep_log_get_recent_days(msg->days, days,
                                                                 SLEEP_LOG_HISTORY_MAX_DAYS) : 0;
                size_t image_size = 0;
                uint8_t *image = days ? make_sleep_report_bmp(msg->days, days, count, &image_size) : NULL;
                char image_key[128] = {0};
                esp_err_t image_err = image ? upload_report_image(image, image_size,
                                                                   image_key, sizeof(image_key)) : ESP_ERR_NO_MEM;
                free(image);
                if (image_err == ESP_OK) image_err = send_image(msg->route_id, image_key);
                char *report = feishu_psram_calloc(1, 1024);
                if (report) sleep_log_format_recent_report(msg->days, report, 1024);
                if (image_err != ESP_OK) {
                    size_t used = report ? strlen(report) : 0;
                    if (report) snprintf(report + used, 1024 - used,
                             "\n报告图发送失败（%s），文字统计仍可使用。", esp_err_to_name(image_err));
                }
                err = report ? send_text(msg->route_id, report) : ESP_ERR_NO_MEM;
                printf("DG Feishu: recent report days=%u records=%u image=%s text=%s\r\n",
                       (unsigned)msg->days, (unsigned)count,
                       esp_err_to_name(image_err), esp_err_to_name(err));
                free(report);
                free(days);
            } else if (msg->text[0]) {
                err = send_text(msg->route_id, msg->text);
            }
            if (err == ESP_OK) {
                s_feishu.reply_count++;
                set_status_result("reply_sent", 200);
                printf("DG Feishu: reply sent count=%u\r\n", (unsigned)s_feishu.reply_count);
            } else {
                char status[FEISHU_AGENT_LAST_RESULT_LEN];
                snprintf(status, sizeof(status), "reply_failed err=%s %.190s",
                         esp_err_to_name(err), s_feishu.last_result);
                set_status_result(status, s_feishu.last_http_status);
                printf("DG Feishu: reply failed err=%s last=%s\r\n",
                       esp_err_to_name(err), s_feishu.last_result);
            }
        }
        memset(msg, 0, sizeof(*msg));
    }
    free(msg);
    s_feishu.reply_task = NULL;
    vTaskDelete(NULL);
}

/* WebSocket callbacks run on the websocket client's small internal stack.
 * They must only assemble/ack frames and enqueue an owned payload.  The
 * already-existing Feishu agent worker drains this queue so we do not spend
 * another task control block from the scarce internal heap. */
static void process_queued_events(void)
{
    feishu_event_msg_t *msg = NULL;
    while (s_feishu.event_queue &&
           xQueueReceive(s_feishu.event_queue, &msg, 0) == pdTRUE) {
        if (msg) {
            printf("DG Feishu: event worker len=%u stack_free=%u\r\n",
                   (unsigned)msg->len,
                   (unsigned)uxTaskGetStackHighWaterMark(NULL));
            process_ws_event_json(msg->data, msg->len);
            free(msg);
            msg = NULL;
        }
    }
}

static void enqueue_ws_event(const uint8_t *payload, size_t len)
{
    if (!payload || len == 0 || len > FEISHU_EVENT_MAX_BYTES || !s_feishu.event_queue) {
        s_feishu.parse_fail_count++;
        set_status_result(len > FEISHU_EVENT_MAX_BYTES ? "event_too_large" : "event_queue_unavailable", 0);
        return;
    }
    feishu_event_msg_t *msg = feishu_psram_malloc(sizeof(*msg) + len + 1);
    if (!msg) {
        set_status_result("event_alloc_failed", 0);
        return;
    }
    msg->len = len;
    memcpy(msg->data, payload, len);
    msg->data[len] = '\0';
    if (xQueueSend(s_feishu.event_queue, &msg, 0) != pdTRUE) {
        free(msg);
        set_status_result("event_queue_full", 0);
        return;
    }
}

static esp_err_t ensure_reply_task(void)
{
    if (s_feishu.reply_task) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreateWithCaps(reply_task,
                                       "feishu_reply",
                                       FEISHU_REPLY_TASK_STACK,
                                       NULL,
                                       3,
                                       &s_feishu.reply_task,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        s_feishu.reply_task = NULL;
        printf("DG Feishu: reply task alloc failed stack=%u internal=%u largest=%u spiram=%u\r\n",
               (unsigned)FEISHU_REPLY_TASK_STACK,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void enqueue_reply(const char *route_id, const char *text)
{
    if (!route_id || !route_id[0] || !text || !text[0] || !s_feishu.reply_queue) {
        return;
    }
    feishu_reply_msg_t *msg = feishu_psram_calloc(1, sizeof(*msg));
    if (!msg) {
        set_status_result("reply_alloc_failed", 0);
        return;
    }
    snprintf(msg->route_id, sizeof(msg->route_id), "%s", route_id);
    snprintf(msg->text, sizeof(msg->text), "%s", text);
    if (xQueueSend(s_feishu.reply_queue, msg, 0) == pdTRUE) {
        set_status_result("reply_queued", 0);
        printf("DG Feishu: reply queued route=%s text=%s\r\n",
               msg->route_id, msg->text);
        (void)ensure_reply_task();
    } else {
        set_status_result("reply_queue_full", 0);
        printf("DG Feishu: reply queue full route=%s\r\n", msg->route_id);
    }
    free(msg);
}

static void enqueue_sleep_report(const char *route_id, uint8_t days)
{
    if (!route_id || !route_id[0] || !s_feishu.reply_queue) return;
    if (days == 0) days = 7;
    if (days > SLEEP_LOG_HISTORY_MAX_DAYS) days = SLEEP_LOG_HISTORY_MAX_DAYS;
    feishu_reply_msg_t msg = {
        .type = FEISHU_REPLY_SLEEP_REPORT,
        .days = days,
    };
    snprintf(msg.route_id, sizeof(msg.route_id), "%s", route_id);
    if (xQueueSend(s_feishu.reply_queue, &msg, 0) == pdTRUE) {
        set_status_result("sleep_report_queued", 0);
        printf("DG Feishu: sleep report queued route=%s days=%u\r\n",
               msg.route_id, (unsigned)days);
        (void)ensure_reply_task();
    } else {
        set_status_result("reply_queue_full", 0);
    }
}

static bool recent_sleep_report_request(const char *text, uint8_t *days)
{
    if (!text || !text_has(text, "睡眠报告")) return false;
    if (!text_has(text, "近") && !text_has(text, "最近") &&
        !text_has(text, "过去") && !text_has(text, "几日") && !text_has(text, "几天")) return false;
    unsigned value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p >= '0' && *p <= '9') {
            value = 0;
            while (*p >= '0' && *p <= '9') {
                value = value * 10U + (unsigned)(*p - '0');
                ++p;
            }
            break;
        }
    }
    if (value == 0) {
        if (text_has(text, "十四")) value = 14;
        else if (text_has(text, "十三")) value = 13;
        else if (text_has(text, "十二")) value = 12;
        else if (text_has(text, "十一")) value = 11;
        else if (text_has(text, "十")) value = 10;
        else if (text_has(text, "九")) value = 9;
        else if (text_has(text, "八")) value = 8;
        else if (text_has(text, "七")) value = 7;
        else if (text_has(text, "六")) value = 6;
        else if (text_has(text, "五")) value = 5;
        else if (text_has(text, "四")) value = 4;
        else if (text_has(text, "三")) value = 3;
        else if (text_has(text, "二") || text_has(text, "两")) value = 2;
        else if (text_has(text, "一")) value = 1;
        else value = 7;
    }
    if (value > SLEEP_LOG_HISTORY_MAX_DAYS) value = SLEEP_LOG_HISTORY_MAX_DAYS;
    if (days) *days = (uint8_t)value;
    return true;
}

static void handle_inbound_text(const char *chat_id, const char *sender_id, const char *message_id, const char *text)
{
    llm_route_t route = {0};
    mobile_app_command_t command = command_from_text(text);
    bool llm_ok = false;
    esp_err_t err = ESP_OK;
    const char *route_id = chat_id && chat_id[0] ? chat_id : sender_id;
    const char *reply = NULL;

    ESP_LOGI(TAG, "inbound route=%s message=%s text=%s", route_id ? route_id : "", message_id ? message_id : "", text);
    s_feishu.received_count++;
    printf("DG Feishu: inbound rx=%u msg=%s text=%s\r\n",
           (unsigned)s_feishu.received_count,
           message_id ? message_id : "",
           text ? text : "");

    uint8_t report_days = 0;
    if (recent_sleep_report_request(text, &report_days)) {
        enqueue_sleep_report(route_id, report_days);
        return;
    }

    if (command == MOBILE_APP_COMMAND_NONE && !FEISHU_LOCAL_COMMAND_ONLY) {
        llm_intent_result_t intent = {0};
        esp_err_t llm_err = llm_intent_resolve_text(text, true, &intent);
        llm_ok = llm_err == ESP_OK;
        if (!llm_ok) {
            printf("DG Feishu: llm route failed err=%s last=%s\r\n",
                   esp_err_to_name(llm_err), s_feishu.last_result);
            llm_err = llm_route_text(text, &route);
            llm_ok = llm_err == ESP_OK;
            if (!llm_ok) {
                printf("DG Feishu: legacy llm route failed err=%s last=%s\r\n",
                       esp_err_to_name(llm_err), s_feishu.last_result);
            }
        }
        if (llm_ok) {
            command = intent.command != MOBILE_APP_COMMAND_NONE ? intent.command : route.command;
            if (intent.reply[0]) {
                snprintf(route.reply, sizeof(route.reply), "%s", intent.reply);
            }
        }
    }

    if (command != MOBILE_APP_COMMAND_NONE && s_feishu.command_cb) {
        err = s_feishu.command_cb(command, s_feishu.command_ctx);
        s_feishu.command_count++;
        printf("DG Feishu: command=%d err=%s count=%u\r\n",
               (int)command, esp_err_to_name(err), (unsigned)s_feishu.command_count);
    } else {
        printf("DG Feishu: no device command llm=%d local_only=%d\r\n",
               llm_ok ? 1 : 0, FEISHU_LOCAL_COMMAND_ONLY);
    }
    if (!FEISHU_SEND_REPLIES) {
        printf("DG Feishu: reply disabled to preserve local http/webhook stability\r\n");
        return;
    }
    if (command == MOBILE_APP_COMMAND_NONE) {
        /* The execution side owns the final safety decision.  Even if an LLM
         * produced conversational text, an unmapped command gets the same
         * explicit no-op response and can never become an arbitrary action. */
        reply = command_reply(MOBILE_APP_COMMAND_NONE, ESP_OK);
    } else if (llm_ok && route.reply[0]) {
        reply = route.reply;
    } else {
        reply = command_reply(command, err);
    }
    enqueue_reply(route_id, reply);
}

static void handle_message_event(cJSON *event)
{
    s_feishu.message_event_count++;
    cJSON *message = cJSON_GetObjectItem(event, "message");
    cJSON *sender = cJSON_GetObjectItem(event, "sender");
    cJSON *sender_id_json = cJSON_IsObject(sender) ? cJSON_GetObjectItem(sender, "sender_id") : NULL;
    cJSON *open_id = cJSON_IsObject(sender_id_json) ? cJSON_GetObjectItem(sender_id_json, "open_id") : NULL;
    cJSON *message_id_json = cJSON_IsObject(message) ? cJSON_GetObjectItem(message, "message_id") : NULL;
    cJSON *chat_id_json = cJSON_IsObject(message) ? cJSON_GetObjectItem(message, "chat_id") : NULL;
    cJSON *chat_type_json = cJSON_IsObject(message) ? cJSON_GetObjectItem(message, "chat_type") : NULL;
    cJSON *message_type_json = cJSON_IsObject(message) ? cJSON_GetObjectItem(message, "message_type") : NULL;
    cJSON *content_json = cJSON_IsObject(message) ? cJSON_GetObjectItem(message, "content") : NULL;

    if (!cJSON_IsString(chat_id_json) || !cJSON_IsString(content_json)) {
        printf("DG Feishu: message event missing chat/content msg_events=%u\r\n",
               (unsigned)s_feishu.message_event_count);
        return;
    }

    const char *message_id = cJSON_IsString(message_id_json) ? message_id_json->valuestring : "";
    const char *chat_id = chat_id_json->valuestring;
    const char *sender_id = cJSON_IsString(open_id) ? open_id->valuestring : "";
    const char *chat_type = cJSON_IsString(chat_type_json) ? chat_type_json->valuestring : "p2p";
    const char *message_type = cJSON_IsString(message_type_json) ? message_type_json->valuestring : "text";
    const char *route_id = (strcmp(chat_type, "p2p") == 0 && sender_id[0]) ? sender_id : chat_id;

    if (message_id[0] && dedup_check_and_record(message_id)) return;

    if (strcmp(message_type, "text") == 0) {
        char *text = extract_text(content_json->valuestring);
        if (!text) {
            printf("DG Feishu: text extract failed content=%s\r\n", content_json->valuestring);
            return;
        }
        char *clean = text;
        if (strncmp(clean, "@_user_1 ", 9) == 0) clean += 9;
        char *last_at = strrchr(clean, '>');
        if (last_at && strstr(clean, "<at ") == clean) {
            clean = last_at + 1;
        }
        while (*clean == ' ' || *clean == '\n' || *clean == '\r' || *clean == '\t') clean++;
        if (clean[0]) handle_inbound_text(route_id, sender_id, message_id, clean);
        free(text);
    } else if (route_id && route_id[0]) {
        enqueue_reply(route_id, "我目前只支持文本指令。");
    }
}

static void process_ws_event_json(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        s_feishu.parse_fail_count++;
        set_status_result("event_json_parse_failed", 0);
        return;
    }
    cJSON *event = cJSON_GetObjectItem(root, "event");
    cJSON *header = cJSON_GetObjectItem(root, "header");
    cJSON *event_type = cJSON_IsObject(header) ? cJSON_GetObjectItem(header, "event_type") : NULL;
    s_feishu.event_count++;
    if (cJSON_IsString(event_type) && event_type->valuestring) {
        snprintf(s_feishu.last_event_type, sizeof(s_feishu.last_event_type), "%s", event_type->valuestring);
    } else {
        snprintf(s_feishu.last_event_type, sizeof(s_feishu.last_event_type), "%s", "unknown");
    }
    printf("DG Feishu: event type=%s events=%u len=%u\r\n",
           s_feishu.last_event_type,
           (unsigned)s_feishu.event_count,
           (unsigned)len);
    if (cJSON_IsObject(event) &&
            (!event_type || (cJSON_IsString(event_type) &&
                             strcmp(event_type->valuestring, "im.message.receive_v1") == 0))) {
        handle_message_event(event);
    }
    cJSON_Delete(root);
}

static void handle_ws_frame(const uint8_t *buf, size_t len)
{
    feishu_ws_frame_t *frame = feishu_psram_calloc(1, sizeof(*frame));
    const char *type = NULL;
    if (!frame) {
        s_feishu.parse_fail_count++;
        set_status_result("ws_frame_alloc_failed", 0);
        return;
    }
    if (!pb_parse_frame(buf, len, frame)) {
        ESP_LOGW(TAG, "WS frame parse failed");
        s_feishu.parse_fail_count++;
        set_status_result("ws_frame_parse_failed", 0);
        free(frame);
        return;
    }
    s_feishu.frame_count++;

    type = ws_header_value(frame, "type");
    if (frame->method == 0) {
        if (type && strcmp(type, "pong") == 0 && frame->payload && frame->payload_len > 0) {
            cJSON *cfg = cJSON_ParseWithLength((const char *)frame->payload, frame->payload_len);
            if (cfg) {
                cJSON *ping = cJSON_GetObjectItem(cfg, "PingInterval");
                if (cJSON_IsNumber(ping)) s_feishu.ws_ping_interval_ms = ping->valueint * 1000;
                cJSON_Delete(cfg);
            }
        }
        free(frame);
        return;
    }
    if (!type || strcmp(type, "event") != 0 || !frame->payload || frame->payload_len == 0) {
        free(frame);
        return;
    }

    const char ack_payload[] = "{\"code\":200}";
    int ack = ws_send_frame(frame, (const uint8_t *)ack_payload, strlen(ack_payload), 1000);
    printf("DG Feishu: frame method=%d type=%s payload=%u ack=%d frames=%u\r\n",
           (int)frame->method,
           type,
           (unsigned)frame->payload_len,
           ack,
           (unsigned)s_feishu.frame_count);

    enqueue_ws_event(frame->payload, frame->payload_len);
    free(frame);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    static uint8_t *rx_buf = NULL;
    static size_t rx_cap = 0;
    esp_websocket_event_data_t *event = (esp_websocket_event_data_t *)event_data;
    (void)arg;
    (void)base;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        s_feishu.ws_connected = true;
        s_feishu.ws_ever_connected = true;
        s_feishu.ws_disconnect_since_ms = 0;
        ESP_LOGI(TAG, "WS connected");
        set_status_result("ws_connected", 0);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_ERROR) {
        char msg[FEISHU_AGENT_LAST_RESULT_LEN];
        if (event && event->data_ptr && event->data_len > 0) {
            int copy_len = event->data_len;
            if (copy_len > (int)sizeof(msg) - 10) copy_len = (int)sizeof(msg) - 10;
            snprintf(msg, sizeof(msg), "ws_error %.*s", copy_len, event->data_ptr);
        } else if (event) {
            snprintf(msg, sizeof(msg), "ws_error type=%d http=%d tls=%d stack=%d flags=0x%x errno=%d",
                     event->error_handle.error_type,
                     event->error_handle.esp_ws_handshake_status_code,
                     event->error_handle.esp_tls_last_esp_err,
                     event->error_handle.esp_tls_stack_err,
                     event->error_handle.esp_tls_cert_verify_flags,
                     event->error_handle.esp_transport_sock_errno);
        } else {
            snprintf(msg, sizeof(msg), "ws_error");
        }
        ESP_LOGW(TAG, "%s", msg);
        set_status_result(msg, event ? event->error_handle.esp_ws_handshake_status_code : 0);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_feishu.ws_connected = false;
        if (s_feishu.ws_ever_connected && s_feishu.ws_disconnect_since_ms == 0) {
            s_feishu.ws_disconnect_since_ms = now_ms();
        }
        ESP_LOGW(TAG, "WS disconnected");
        if (!last_result_has_prefix("ws_error")) {
            if (event) {
                char msg[FEISHU_AGENT_LAST_RESULT_LEN];
                snprintf(msg, sizeof(msg), "ws_disconnected type=%d http=%d tls=%d stack=%d flags=0x%x errno=%d",
                         event->error_handle.error_type,
                         event->error_handle.esp_ws_handshake_status_code,
                         event->error_handle.esp_tls_last_esp_err,
                         event->error_handle.esp_tls_stack_err,
                         event->error_handle.esp_tls_cert_verify_flags,
                         event->error_handle.esp_transport_sock_errno);
                set_status_result(msg, event->error_handle.esp_ws_handshake_status_code);
            } else {
                set_status_result("ws_disconnected", 0);
            }
        }
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || !event) {
        return;
    }

    s_feishu.ws_data_count++;
    s_feishu.last_ws_opcode = event->op_code;
    s_feishu.last_ws_data_len = event->data_len > 0 ? (uint32_t)event->data_len : 0;
    s_feishu.last_ws_payload_len = event->payload_len > 0 ? (uint32_t)event->payload_len : 0;
    if (event->op_code == WS_TRANSPORT_OPCODES_BINARY) {
        s_feishu.ws_binary_count++;
    } else if (event->op_code == WS_TRANSPORT_OPCODES_TEXT) {
        s_feishu.ws_text_count++;
    } else {
        return;
    }

    size_t need = event->payload_offset + event->data_len;
    if (event->payload_offset == 0) {
        free(rx_buf);
        rx_cap = event->payload_len > need ? event->payload_len : need;
        if (event->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            rx_cap += 1;
        }
        rx_buf = feishu_psram_malloc(rx_cap);
        if (!rx_buf) {
            rx_cap = 0;
            return;
        }
    } else if (!rx_buf || need > rx_cap) {
        return;
    }

    memcpy(rx_buf + event->payload_offset, event->data_ptr, event->data_len);
    if (need >= event->payload_len) {
        if (event->op_code == WS_TRANSPORT_OPCODES_BINARY) {
            handle_ws_frame(rx_buf, event->payload_len);
        } else {
            rx_buf[event->payload_len] = '\0';
            enqueue_ws_event(rx_buf, event->payload_len);
        }
        free(rx_buf);
        rx_buf = NULL;
        rx_cap = 0;
    }
}

static void ws_task(void *arg)
{
    (void)arg;
    while (!s_feishu.stop_requested) {
        int64_t connect_started_ms = 0;
        int64_t last_ping_ms = 0;
        int64_t last_status_ms = 0;

        /* Credentials are loaded by feishu_agent_start/restart on an internal
         * stack.  This worker uses a PSRAM stack and must never call NVS/flash
         * APIs while the cache is temporarily disabled. */
        if (!s_feishu.app_id[0] || !s_feishu.app_secret[0]) {
            s_feishu.restart_requested = false;
            vTaskDelay(pdMS_TO_TICKS(FEISHU_RECONNECT_DELAY_MS));
            continue;
        }

        ESP_LOGI(TAG, "Feishu agent starting app_id=%.8s...", s_feishu.app_id);
        if (pull_ws_config() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(FEISHU_RECONNECT_DELAY_MS));
            continue;
        }

        ESP_LOGI(TAG, "heap before ws internal=%u largest=%u spiram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        esp_websocket_client_config_t ws_config = {
            .uri = s_feishu.ws_url,
            .buffer_size = 768,
            .task_stack = FEISHU_WS_TASK_STACK,
            .reconnect_timeout_ms = s_feishu.ws_reconnect_interval_ms,
            .network_timeout_ms = 10000,
            .ping_interval_sec = 300,
            .disable_pingpong_discon = true,
            .disable_auto_reconnect = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        s_feishu.ws_client = esp_websocket_client_init(&ws_config);
        if (!s_feishu.ws_client) {
            set_status_result("ws_client_alloc_failed", 0);
            vTaskDelay(pdMS_TO_TICKS(FEISHU_RECONNECT_DELAY_MS));
            continue;
        }

        s_feishu.ws_connected = false;
        s_feishu.ws_ever_connected = false;
        s_feishu.ws_disconnect_since_ms = 0;
        esp_websocket_register_events(s_feishu.ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
        if (esp_websocket_client_start(s_feishu.ws_client) != ESP_OK) {
            ESP_LOGE(TAG, "WS start failed");
            char msg[FEISHU_AGENT_LAST_RESULT_LEN];
            snprintf(msg, sizeof(msg), "ws_start_failed heap=%u/%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            set_status_result(msg, 0);
            esp_websocket_client_destroy(s_feishu.ws_client);
            s_feishu.ws_client = NULL;
            if (internal_heap_low()) {
                printf("DG Feishu: ws start failed with low heap; retrying instead of disabling agent\r\n");
            }
            vTaskDelay(pdMS_TO_TICKS(FEISHU_RECONNECT_DELAY_MS));
            continue;
        }

        connect_started_ms = now_ms();
        while (s_feishu.ws_client && !s_feishu.stop_requested && !s_feishu.restart_requested) {
            int64_t t = now_ms();
            process_queued_events();
            if (t - last_status_ms >= 15000) {
                printf("DG Feishu: live ws=%d data=%u bin=%u text=%u frames=%u events=%u msg=%u rx=%u cmd=%u reply=%u last=%s heap=%u/%u\r\n",
                       s_feishu.ws_connected ? 1 : 0,
                       (unsigned)s_feishu.ws_data_count,
                       (unsigned)s_feishu.ws_binary_count,
                       (unsigned)s_feishu.ws_text_count,
                       (unsigned)s_feishu.frame_count,
                       (unsigned)s_feishu.event_count,
                       (unsigned)s_feishu.message_event_count,
                       (unsigned)s_feishu.received_count,
                       (unsigned)s_feishu.command_count,
                       (unsigned)s_feishu.reply_count,
                       s_feishu.last_result,
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                last_status_ms = t;
            }
            if (s_feishu.ws_connected && t - last_ping_ms >= s_feishu.ws_ping_interval_ms) {
                feishu_ws_frame_t *ping = feishu_psram_calloc(1, sizeof(*ping));
                if (ping) {
                    ping->service = s_feishu.ws_service_id;
                    ping->header_count = 1;
                    snprintf(ping->headers[0].key, sizeof(ping->headers[0].key), "%s", "type");
                    snprintf(ping->headers[0].value, sizeof(ping->headers[0].value), "%s", "ping");
                    (void)ws_send_frame(ping, NULL, 0, 1000);
                    free(ping);
                }
                last_ping_ms = t;
            }
            if (!s_feishu.ws_ever_connected) {
                if (!esp_websocket_client_is_connected(s_feishu.ws_client) &&
                        last_result_has_prefix("ws_error") &&
                        t - connect_started_ms >= 1000) {
                    ESP_LOGW(TAG, "WS connect failed; recreating client");
                    break;
                }
                if (!esp_websocket_client_is_connected(s_feishu.ws_client) &&
                        t - connect_started_ms >= FEISHU_INITIAL_CONNECT_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "WS initial connect timeout");
                    set_status_result("ws_initial_connect_timeout", 0);
                    break;
                }
            } else if (!esp_websocket_client_is_connected(s_feishu.ws_client) && !s_feishu.ws_connected) {
                if (s_feishu.ws_disconnect_since_ms == 0) s_feishu.ws_disconnect_since_ms = t;
                if (t - s_feishu.ws_disconnect_since_ms >= 3000) {
                    ESP_LOGW(TAG, "WS disconnected; recreating client");
                    if (!last_result_has_prefix("ws_error")) {
                        set_status_result("ws_reconnect_fast", 0);
                    }
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (s_feishu.ws_client) {
            esp_websocket_client_stop(s_feishu.ws_client);
            esp_websocket_client_destroy(s_feishu.ws_client);
            s_feishu.ws_client = NULL;
        }
        s_feishu.ws_connected = false;

        if (s_feishu.restart_requested) {
            s_feishu.restart_requested = false;
            memset(s_feishu.tenant_token, 0, sizeof(s_feishu.tenant_token));
        }
        if (!s_feishu.ws_ever_connected && internal_heap_low()) {
            printf("DG Feishu: initial ws failed with low heap; keep retrying internal=%u largest=%u spiram=%u\r\n",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
        if (!s_feishu.stop_requested) {
            vTaskDelay(pdMS_TO_TICKS(FEISHU_RECONNECT_DELAY_MS));
        }
    }

    s_feishu.running = false;
    s_feishu.ws_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t feishu_agent_start(const feishu_agent_config_t *config)
{
    if (config) {
        s_feishu.command_cb = config->command_cb;
        s_feishu.command_ctx = config->command_ctx;
    }
    if (s_feishu.ws_task) {
        return ESP_OK;
    }
    /* This function is called from the normal internal-stack application
     * task, making NVS reads safe before the PSRAM-stack worker starts. */
    ESP_RETURN_ON_ERROR(load_credentials(), TAG, "load credentials");
    s_feishu.stop_requested = false;
    s_feishu.restart_requested = false;
    printf("DG Feishu: start heap internal=%u largest=%u spiram=%u\r\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (FEISHU_SEND_REPLIES && !s_feishu.reply_queue) {
        s_feishu.reply_queue = xQueueCreateWithCaps(FEISHU_REPLY_QUEUE_LEN,
                                                    sizeof(feishu_reply_msg_t),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_feishu.reply_queue) {
            printf("DG Feishu: reply queue alloc failed internal=%u largest=%u spiram=%u\r\n",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_feishu.event_queue) {
        s_feishu.event_queue = xQueueCreateWithCaps(FEISHU_EVENT_QUEUE_LEN,
                                                    sizeof(feishu_event_msg_t *),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_feishu.event_queue) {
            set_status_result("event_queue_alloc_failed", 0);
            return ESP_ERR_NO_MEM;
        }
    }
    BaseType_t ok = xTaskCreateWithCaps(ws_task, "feishu_agent", FEISHU_AGENT_TASK_STACK,
                                       NULL, 4, &s_feishu.ws_task,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        s_feishu.ws_task = NULL;
        s_feishu.stop_requested = true;
        printf("DG Feishu: agent task alloc failed stack=%u internal=%u largest=%u spiram=%u\r\n",
               (unsigned)FEISHU_AGENT_TASK_STACK,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }
    s_feishu.running = true;
    return ESP_OK;
}

esp_err_t feishu_agent_restart(void)
{
    ESP_RETURN_ON_ERROR(load_credentials(), TAG, "reload credentials");
    s_feishu.restart_requested = true;
    return ESP_OK;
}

void feishu_agent_get_status(feishu_agent_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
    status->running = s_feishu.running;
    status->configured = s_feishu.app_id[0] != '\0' && s_feishu.app_secret[0] != '\0';
    status->ws_connected = s_feishu.ws_connected;
    status->ws_ever_connected = s_feishu.ws_ever_connected;
    status->ws_data_count = s_feishu.ws_data_count;
    status->ws_binary_count = s_feishu.ws_binary_count;
    status->ws_text_count = s_feishu.ws_text_count;
    status->last_ws_opcode = s_feishu.last_ws_opcode;
    status->last_ws_data_len = s_feishu.last_ws_data_len;
    status->last_ws_payload_len = s_feishu.last_ws_payload_len;
    status->frame_count = s_feishu.frame_count;
    status->event_count = s_feishu.event_count;
    status->message_event_count = s_feishu.message_event_count;
    status->parse_fail_count = s_feishu.parse_fail_count;
    status->received_count = s_feishu.received_count;
    status->command_count = s_feishu.command_count;
    status->reply_count = s_feishu.reply_count;
    status->last_http_status = s_feishu.last_http_status;
    snprintf(status->last_event_type, sizeof(status->last_event_type), "%s", s_feishu.last_event_type);
    snprintf(status->last_result, sizeof(status->last_result), "%s", s_feishu.last_result);
}
