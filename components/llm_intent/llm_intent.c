#include "llm_intent.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

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
#define LLM_MAX_RESPONSE 4096

#define ZH_SLEEP "\xE7\x9D\xA1\xE7\x9C\xA0"
#define ZH_SLEEP2 "\xE7\x9D\xA1\xE8\xA7\x89"
#define ZH_SLEEP_AID "\xE5\x8A\xA9\xE7\x9C\xA0"
#define ZH_REST "\xE4\xBC\x91\xE6\x81\xAF"
#define ZH_STOP "\xE5\x81\x9C\xE6\xAD\xA2"
#define ZH_CANCEL "\xE5\x8F\x96\xE6\xB6\x88"
#define ZH_CLOSE "\xE5\x85\xB3\xE9\x97\xAD"
#define ZH_EXIT "\xE9\x80\x80\xE5\x87\xBA"
#define ZH_END "\xE7\xBB\x93\xE6\x9D\x9F"
#define ZH_VOICE "\xE8\xAF\xAD\xE9\x9F\xB3"
#define ZH_MODE "\xE6\xA8\xA1\xE5\xBC\x8F"
#define ZH_LIGHT "\xE7\x81\xAF"
#define ZH_LED_STRIP "\xE7\x81\xAF\xE5\xB8\xA6"
#define ZH_SCREEN "\xE5\xB1\x8F\xE5\xB9\x95"
#define ZH_DISPLAY "\xE6\x98\xBE\xE7\xA4\xBA"
#define ZH_RED "\xE7\xBA\xA2"
#define ZH_GREEN "\xE7\xBB\xBF"
#define ZH_BLUE "\xE8\x93\x9D"
#define ZH_YELLOW "\xE9\xBB\x84"
#define ZH_ORANGE "\xE6\xA9\x99"
#define ZH_TANGERINE "\xE6\xA1\x94"
#define ZH_GOLD "\xE9\x87\x91"
#define ZH_WARM "\xE6\x9A\x96"
#define ZH_OFF "\xE5\x85\xB3\xE7\x81\xAF"
#define ZH_OPEN "\xE6\x89\x93\xE5\xBC\x80"
#define ZH_ENABLE "\xE5\xBC\x80\xE5\x90\xAF"
#define ZH_TURN_ON "\xE5\xBC\x80\xE7\x81\xAF"
#define ZH_BRIGHTER "\xE5\x8F\x98\xE4\xBA\xAE"
#define ZH_BRIGHTER_ALT "\xE8\xB0\x83\xE4\xBA\xAE"
#define ZH_DIMMER "\xE5\x8F\x98\xE6\x9A\x97"
#define ZH_DIMMER_ALT "\xE8\xB0\x83\xE6\x9A\x97"
#define ZH_MUSIC "\xE9\x9F\xB3\xE4\xB9\x90"
#define ZH_PLAY "\xE6\x92\xAD\xE6\x94\xBE"
#define ZH_NOISE "\xE5\x99\xAA\xE5\xA3\xB0"
#define ZH_WHITE_NOISE "\xE7\x99\xBD\xE5\x99\xAA\xE5\xA3\xB0"
#define ZH_PINK_NOISE "\xE7\xB2\x89\xE5\x99\xAA\xE5\xA3\xB0"
#define ZH_BREATH "\xE5\x91\xBC\xE5\x90\xB8"
#define ZH_STORY "\xE6\x95\x85\xE4\xBA\x8B"
#define ZH_OCEAN "\xE6\xB5\xB7"
#define ZH_WAVE "\xE6\xB5\xB7\xE6\xB5\xAA"
#define ZH_FOREST "\xE6\xA3\xAE\xE6\x9E\x97"
#define ZH_RAIN "\xE9\x9B\xA8"
#define ZH_ZEN "\xE7\xA6\x85"
#define ZH_MEDITATE "\xE5\x86\xA5\xE6\x83\xB3"
#define ZH_SELF_TEST "\xE8\x87\xAA\xE6\xA3\x80"
#define ZH_TEST "\xE6\xB5\x8B\xE8\xAF\x95"
#define ZH_VOLUME "\xE9\x9F\xB3\xE9\x87\x8F"
#define ZH_LOUD "\xE5\xA4\xA7\xE5\xA3\xB0"
#define ZH_SMALL "\xE5\xB0\x8F\xE5\xA3\xB0"
#define ZH_MUTE "\xE9\x9D\x99\xE9\x9F\xB3"

static const char *TAG = "llm_intent";

typedef struct {
    char backend_type[LLM_BACKEND_LEN];
    char model[LLM_MODEL_LEN];
    char base_url[LLM_URL_LEN];
    char api_key[LLM_API_KEY_LEN];
    char auth_type[LLM_AUTH_LEN];
    char max_tokens_field[LLM_FIELD_LEN];
} llm_config_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static llm_intent_status_t s_status;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool has(const char *text, const char *needle)
{
    return text && needle && needle[0] && strstr(text, needle) != NULL;
}

static bool has_i(const char *text, const char *needle)
{
    if (!text || !needle || !needle[0]) return false;
    size_t needle_len = strlen(needle);
    for (const char *p = text; *p; ++p) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == needle_len) return true;
    }
    return false;
}

static void set_status(esp_err_t err, int http_status, const char *result)
{
    portENTER_CRITICAL(&s_lock);
    s_status.last_error = err;
    s_status.last_http_status = http_status;
    snprintf(s_status.last_result, sizeof(s_status.last_result), "%s",
             result ? result : esp_err_to_name(err));
    portEXIT_CRITICAL(&s_lock);
}

static void inc_source(llm_intent_source_t source)
{
    portENTER_CRITICAL(&s_lock);
    if (source == LLM_INTENT_SOURCE_LOCAL) {
        s_status.local_count++;
    } else if (source == LLM_INTENT_SOURCE_LLM) {
        s_status.llm_count++;
    } else {
        s_status.fallback_count++;
    }
    portEXIT_CRITICAL(&s_lock);
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

static esp_err_t load_llm_config(llm_config_t *cfg)
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
    if (err != ESP_OK) {
        set_status(err, 0, "llm_config_missing");
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_INVALID_STATE : err;
    }
    nvs_get_str_default(nvs, CLAW_NVS_BACKEND_TYPE, cfg->backend_type,
                        sizeof(cfg->backend_type), cfg->backend_type);
    nvs_get_str_default(nvs, CLAW_NVS_MODEL, cfg->model, sizeof(cfg->model), cfg->model);
    nvs_get_str_default(nvs, CLAW_NVS_BASE_URL, cfg->base_url, sizeof(cfg->base_url), cfg->base_url);
    nvs_get_str_default(nvs, CLAW_NVS_API_KEY, cfg->api_key, sizeof(cfg->api_key), "");
    nvs_get_str_default(nvs, CLAW_NVS_AUTH_TYPE, cfg->auth_type, sizeof(cfg->auth_type), cfg->auth_type);
    nvs_get_str_default(nvs, CLAW_NVS_MAX_TOKENS_FIELD, cfg->max_tokens_field,
                        sizeof(cfg->max_tokens_field), cfg->max_tokens_field);
    nvs_close(nvs);

    bool configured = cfg->api_key[0] && cfg->model[0] && cfg->base_url[0];
    portENTER_CRITICAL(&s_lock);
    s_status.configured = configured;
    portEXIT_CRITICAL(&s_lock);
    if (!configured) {
        set_status(ESP_ERR_INVALID_STATE, 0, "llm_not_configured");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t llm_intent_configure_openai_compatible(const char *api_key, const char *base_url, const char *model)
{
    if (!api_key || !api_key[0] || !base_url || !base_url[0] || !model || !model[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(base_url, "https://", 8) != 0 && strncmp(base_url, "http://", 7) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open llm nvs");
    esp_err_t err = nvs_set_str(nvs, CLAW_NVS_BACKEND_TYPE, "openai_compatible");
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_MODEL, model);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_BASE_URL, base_url);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_API_KEY, api_key);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_AUTH_TYPE, "bearer");
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_MAX_TOKENS_FIELD, "max_tokens");
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    llm_config_t cfg = {0};
    (void)load_llm_config(&cfg);
    set_status(err, 0, err == ESP_OK ? "llm_config_saved" : "llm_config_save_failed");
    return err;
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
    if (resp->len + (size_t)evt->data_len + 1 > LLM_MAX_RESPONSE) {
        return ESP_FAIL;
    }
    if (resp->len + (size_t)evt->data_len + 1 > resp->cap) {
        size_t new_cap = resp->cap ? resp->cap * 2 : 1024;
        while (new_cap < resp->len + (size_t)evt->data_len + 1) {
            new_cap *= 2;
        }
        if (new_cap > LLM_MAX_RESPONSE) {
            new_cap = LLM_MAX_RESPONSE;
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

static esp_err_t post_json(const char *url, const llm_config_t *cfg,
                           const char *body, char **out_response, int *out_status)
{
    http_resp_t resp = {0};
    *out_response = NULL;
    *out_status = 0;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 25000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .addr_type = HTTP_ADDR_TYPE_INET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
    if (strstr(cfg->backend_type, "anthropic")) {
        esp_http_client_set_header(client, "x-api-key", cfg->api_key);
        esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
    } else if (strcasecmp(cfg->auth_type, "none") != 0) {
        char auth[LLM_API_KEY_LEN + 16];
        snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    int tls_error = 0;
    int tls_flags = 0;
    (void)esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        char msg[LLM_INTENT_RESULT_MAX];
        snprintf(msg, sizeof(msg), "llm_http_failed err=%s tls=%d flags=0x%x",
                 esp_err_to_name(err), tls_error, tls_flags);
        set_status(err, *out_status, msg);
        free(resp.buf);
        return err;
    }
    if (*out_status < 200 || *out_status >= 300) {
        char msg[LLM_INTENT_RESULT_MAX];
        snprintf(msg, sizeof(msg), "llm_http_status=%d", *out_status);
        set_status(ESP_FAIL, *out_status, msg);
        free(resp.buf);
        return ESP_FAIL;
    }
    *out_response = resp.buf ? resp.buf : strdup("");
    return *out_response ? ESP_OK : ESP_ERR_NO_MEM;
}

mobile_app_command_t llm_intent_command_from_name(const char *name)
{
    if (!name || !name[0] || strcmp(name, "none") == 0) return MOBILE_APP_COMMAND_NONE;
    if (strcmp(name, "sleep") == 0 || strcmp(name, "sleep.start") == 0) return MOBILE_APP_COMMAND_SLEEP;
    if (strcmp(name, "slept") == 0 || strcmp(name, "sleep.locked") == 0) return MOBILE_APP_COMMAND_SLEPT;
    if (strcmp(name, "stop") == 0 || strcmp(name, "sleep.stop") == 0) return MOBILE_APP_COMMAND_STOP;
    if (strcmp(name, "selftest") == 0 || strcmp(name, "self_test") == 0) return MOBILE_APP_COMMAND_SELF_TEST;
    if (strcmp(name, "wake.ack") == 0 || strcmp(name, "voice.wake_ack") == 0) return MOBILE_APP_COMMAND_WAKE_ACK;
    if (strcmp(name, "screen.red") == 0 || strcmp(name, "screen_red") == 0) return MOBILE_APP_COMMAND_SCREEN_RED;
    if (strcmp(name, "screen.green") == 0 || strcmp(name, "screen_green") == 0) return MOBILE_APP_COMMAND_SCREEN_GREEN;
    if (strcmp(name, "screen.blue") == 0 || strcmp(name, "screen_blue") == 0) return MOBILE_APP_COMMAND_SCREEN_BLUE;
    if (strcmp(name, "screen.yellow") == 0 || strcmp(name, "screen_yellow") == 0) return MOBILE_APP_COMMAND_SCREEN_YELLOW;
    if (strcmp(name, "screen.off") == 0 || strcmp(name, "screen_off") == 0) return MOBILE_APP_COMMAND_SCREEN_OFF;
    if (strcmp(name, "light.red") == 0 || strcmp(name, "light_red") == 0) return MOBILE_APP_COMMAND_LIGHT_RED;
    if (strcmp(name, "light.green") == 0 || strcmp(name, "light_green") == 0) return MOBILE_APP_COMMAND_LIGHT_GREEN;
    if (strcmp(name, "light.blue") == 0 || strcmp(name, "light_blue") == 0) return MOBILE_APP_COMMAND_LIGHT_BLUE;
    if (strcmp(name, "light.yellow") == 0 || strcmp(name, "light_yellow") == 0) return MOBILE_APP_COMMAND_LIGHT_YELLOW;
    if (strcmp(name, "light.off") == 0 || strcmp(name, "light_off") == 0) return MOBILE_APP_COMMAND_LIGHT_OFF;
    if (strcmp(name, "light.brighter") == 0 || strcmp(name, "light_bright") == 0 ||
        strcmp(name, "light_brighter") == 0) return MOBILE_APP_COMMAND_LIGHT_BRIGHTER;
    if (strcmp(name, "light.dimmer") == 0 || strcmp(name, "light_dim") == 0 ||
        strcmp(name, "light_dimmer") == 0) return MOBILE_APP_COMMAND_LIGHT_DIMMER;
    if (strcmp(name, "audio.music") == 0 || strcmp(name, "music.play") == 0) return MOBILE_APP_COMMAND_AUDIO_MUSIC;
    if (strcmp(name, "audio.noise") == 0 || strcmp(name, "audio.pink_noise") == 0) return MOBILE_APP_COMMAND_AUDIO_NOISE;
    if (strcmp(name, "audio.breathing") == 0) return MOBILE_APP_COMMAND_AUDIO_BREATHING;
    if (strcmp(name, "audio.volume.max") == 0 || strcmp(name, "volume.max") == 0) return MOBILE_APP_COMMAND_AUDIO_VOLUME_MAX;
    if (strcmp(name, "audio.volume.up") == 0 || strcmp(name, "volume.up") == 0) return MOBILE_APP_COMMAND_AUDIO_VOLUME_UP;
    if (strcmp(name, "audio.volume.down") == 0 || strcmp(name, "volume.down") == 0) return MOBILE_APP_COMMAND_AUDIO_VOLUME_DOWN;
    if (strcmp(name, "audio.mute") == 0 || strcmp(name, "mute") == 0) return MOBILE_APP_COMMAND_AUDIO_MUTE;
    if (strcmp(name, "story") == 0 || strcmp(name, "story.play") == 0) return MOBILE_APP_COMMAND_STORY;
    if (strcmp(name, "scene.ppm") == 0 || strcmp(name, "scene.default") == 0) return MOBILE_APP_COMMAND_SCENE_PPM;
    if (strcmp(name, "scene.ocean") == 0 || strcmp(name, "scene.tide") == 0) return MOBILE_APP_COMMAND_SCENE_OCEAN;
    if (strcmp(name, "scene.forest") == 0) return MOBILE_APP_COMMAND_SCENE_FOREST;
    if (strcmp(name, "scene.rain") == 0) return MOBILE_APP_COMMAND_SCENE_RAIN;
    if (strcmp(name, "scene.zen") == 0 || strcmp(name, "scene.meditation") == 0) return MOBILE_APP_COMMAND_SCENE_ZEN;
    if (strcmp(name, "scene.empty") == 0) return MOBILE_APP_COMMAND_SCENE_EMPTY;
    if (strcmp(name, "scene.ppm.play") == 0 || strcmp(name, "scene.default.play") == 0) return MOBILE_APP_COMMAND_SCENE_PPM_PLAY;
    if (strcmp(name, "scene.ocean.play") == 0 || strcmp(name, "scene.tide.play") == 0) return MOBILE_APP_COMMAND_SCENE_OCEAN_PLAY;
    if (strcmp(name, "scene.forest.play") == 0) return MOBILE_APP_COMMAND_SCENE_FOREST_PLAY;
    if (strcmp(name, "scene.rain.play") == 0) return MOBILE_APP_COMMAND_SCENE_RAIN_PLAY;
    if (strcmp(name, "scene.zen.play") == 0 || strcmp(name, "scene.meditation.play") == 0) return MOBILE_APP_COMMAND_SCENE_ZEN_PLAY;
    if (strcmp(name, "breath.relax") == 0 || strcmp(name, "breath.5-5") == 0) return MOBILE_APP_COMMAND_BREATH_RELAX;
    if (strcmp(name, "breath.box") == 0 || strcmp(name, "breath.4-4") == 0) return MOBILE_APP_COMMAND_BREATH_BOX;
    if (strcmp(name, "breath.deep") == 0 || strcmp(name, "breath.4-7-8") == 0) return MOBILE_APP_COMMAND_BREATH_DEEP;
    if (strcmp(name, "mid_sleep.test_minor") == 0) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_MINOR;
    if (strcmp(name, "mid_sleep.test_restless") == 0) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_RESTLESS;
    if (strcmp(name, "mid_sleep.test_arousal") == 0) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_AROUSAL;
    if (strcmp(name, "mid_sleep.test_out_of_bed") == 0) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_OUT_OF_BED;
    return MOBILE_APP_COMMAND_NONE;
}

const char *llm_intent_command_to_name(mobile_app_command_t command)
{
    switch (command) {
    case MOBILE_APP_COMMAND_SLEEP: return "sleep.start";
    case MOBILE_APP_COMMAND_SLEPT: return "sleep.locked";
    case MOBILE_APP_COMMAND_STOP: return "sleep.stop";
    case MOBILE_APP_COMMAND_SELF_TEST: return "self_test";
    case MOBILE_APP_COMMAND_LIGHT_RED: return "light.red";
    case MOBILE_APP_COMMAND_LIGHT_GREEN: return "light.green";
    case MOBILE_APP_COMMAND_LIGHT_BLUE: return "light.blue";
    case MOBILE_APP_COMMAND_LIGHT_YELLOW: return "light.yellow";
    case MOBILE_APP_COMMAND_LIGHT_OFF: return "light.off";
    case MOBILE_APP_COMMAND_LIGHT_BRIGHTER: return "light.brighter";
    case MOBILE_APP_COMMAND_LIGHT_DIMMER: return "light.dimmer";
    case MOBILE_APP_COMMAND_SCREEN_RED: return "screen.red";
    case MOBILE_APP_COMMAND_SCREEN_GREEN: return "screen.green";
    case MOBILE_APP_COMMAND_SCREEN_BLUE: return "screen.blue";
    case MOBILE_APP_COMMAND_SCREEN_YELLOW: return "screen.yellow";
    case MOBILE_APP_COMMAND_SCREEN_OFF: return "screen.off";
    case MOBILE_APP_COMMAND_AUDIO_MUSIC: return "audio.music";
    case MOBILE_APP_COMMAND_AUDIO_NOISE: return "audio.noise";
    case MOBILE_APP_COMMAND_AUDIO_BREATHING: return "audio.breathing";
    case MOBILE_APP_COMMAND_AUDIO_VOLUME_MAX: return "audio.volume.max";
    case MOBILE_APP_COMMAND_AUDIO_VOLUME_UP: return "audio.volume.up";
    case MOBILE_APP_COMMAND_AUDIO_VOLUME_DOWN: return "audio.volume.down";
    case MOBILE_APP_COMMAND_AUDIO_MUTE: return "audio.mute";
    case MOBILE_APP_COMMAND_STORY: return "story.play";
    case MOBILE_APP_COMMAND_WAKE_ACK: return "wake.ack";
    case MOBILE_APP_COMMAND_SCENE_PPM: return "scene.ppm";
    case MOBILE_APP_COMMAND_SCENE_OCEAN: return "scene.ocean";
    case MOBILE_APP_COMMAND_SCENE_FOREST: return "scene.forest";
    case MOBILE_APP_COMMAND_SCENE_RAIN: return "scene.rain";
    case MOBILE_APP_COMMAND_SCENE_ZEN: return "scene.zen";
    case MOBILE_APP_COMMAND_SCENE_EMPTY: return "scene.empty";
    case MOBILE_APP_COMMAND_SCENE_PPM_PLAY: return "scene.ppm.play";
    case MOBILE_APP_COMMAND_SCENE_OCEAN_PLAY: return "scene.ocean.play";
    case MOBILE_APP_COMMAND_SCENE_FOREST_PLAY: return "scene.forest.play";
    case MOBILE_APP_COMMAND_SCENE_RAIN_PLAY: return "scene.rain.play";
    case MOBILE_APP_COMMAND_SCENE_ZEN_PLAY: return "scene.zen.play";
    case MOBILE_APP_COMMAND_BREATH_RELAX: return "breath.relax";
    case MOBILE_APP_COMMAND_BREATH_BOX: return "breath.box";
    case MOBILE_APP_COMMAND_BREATH_DEEP: return "breath.deep";
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_MINOR: return "mid_sleep.test_minor";
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_RESTLESS: return "mid_sleep.test_restless";
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_AROUSAL: return "mid_sleep.test_arousal";
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_OUT_OF_BED: return "mid_sleep.test_out_of_bed";
    default: return "none";
    }
}

mobile_app_command_t llm_intent_local_match(const char *text)
{
    if (!text || !text[0]) return MOBILE_APP_COMMAND_NONE;

    mobile_app_command_t direct = llm_intent_command_from_name(text);
    if (direct != MOBILE_APP_COMMAND_NONE) return direct;

    if (has_i(text, "selftest") || has_i(text, "self test") || has(text, ZH_SELF_TEST)) {
        return MOBILE_APP_COMMAND_SELF_TEST;
    }
    if (has_i(text, "stop") || has_i(text, "cancel") || has_i(text, "exit") ||
        has(text, ZH_STOP) || has(text, ZH_CANCEL) || has(text, ZH_EXIT) ||
        has(text, ZH_END) || (has(text, ZH_CLOSE) && has(text, ZH_VOICE)) ||
        (has(text, ZH_VOICE) && has(text, ZH_MODE) && has(text, ZH_EXIT))) {
        return MOBILE_APP_COMMAND_STOP;
    }
    if (has_i(text, "sleep") || has_i(text, "bedtime") || has(text, ZH_SLEEP) ||
        has(text, ZH_SLEEP2) || has(text, ZH_SLEEP_AID) || has(text, ZH_REST)) {
        return MOBILE_APP_COMMAND_SLEEP;
    }

    bool screen = has_i(text, "screen") || has_i(text, "display") || has(text, ZH_SCREEN) || has(text, ZH_DISPLAY);
    bool light = has_i(text, "light") || has_i(text, "led") || has(text, ZH_LIGHT) || has(text, ZH_LED_STRIP);
    if (screen) {
        if (has_i(text, "red") || has(text, ZH_RED)) return MOBILE_APP_COMMAND_SCREEN_RED;
        if (has_i(text, "green") || has(text, ZH_GREEN)) return MOBILE_APP_COMMAND_SCREEN_GREEN;
        if (has_i(text, "blue") || has(text, ZH_BLUE)) return MOBILE_APP_COMMAND_SCREEN_BLUE;
        if (has_i(text, "yellow") || has_i(text, "orange") || has_i(text, "amber") ||
            has(text, ZH_YELLOW) || has(text, ZH_ORANGE) || has(text, ZH_TANGERINE) ||
            has(text, ZH_GOLD) || has(text, ZH_WARM)) return MOBILE_APP_COMMAND_SCREEN_YELLOW;
        if (has_i(text, "off") || has_i(text, "normal") || has(text, ZH_CLOSE)) return MOBILE_APP_COMMAND_SCREEN_OFF;
    }
    if (light) {
        if (has_i(text, "brighter") || has_i(text, "brighten") ||
            has_i(text, "light up") || has(text, ZH_BRIGHTER) || has(text, ZH_BRIGHTER_ALT)) {
            return MOBILE_APP_COMMAND_LIGHT_BRIGHTER;
        }
        if (has_i(text, "dimmer") || has_i(text, "dim the light") ||
            has(text, ZH_DIMMER) || has(text, ZH_DIMMER_ALT)) {
            return MOBILE_APP_COMMAND_LIGHT_DIMMER;
        }
        if (has_i(text, "red") || has(text, ZH_RED)) return MOBILE_APP_COMMAND_LIGHT_RED;
        if (has_i(text, "green") || has(text, ZH_GREEN)) return MOBILE_APP_COMMAND_LIGHT_GREEN;
        if (has_i(text, "blue") || has(text, ZH_BLUE)) return MOBILE_APP_COMMAND_LIGHT_BLUE;
        if (has_i(text, "yellow") || has_i(text, "orange") || has_i(text, "amber") ||
            has(text, ZH_YELLOW) || has(text, ZH_ORANGE) || has(text, ZH_TANGERINE) ||
            has(text, ZH_GOLD) || has(text, ZH_WARM)) return MOBILE_APP_COMMAND_LIGHT_YELLOW;
        if (has_i(text, "off") || has(text, ZH_OFF) || has(text, ZH_CLOSE)) return MOBILE_APP_COMMAND_LIGHT_OFF;
        /* A bare "turn on the light strip" request has no colour.  Keep it
         * deterministic and local so both Feishu and voice use the safe
         * device-command queue instead of starting an LLM/HTTP transaction. */
        if (has_i(text, "on") || has_i(text, "turn on") ||
            has(text, ZH_OPEN) || has(text, ZH_ENABLE) || has(text, ZH_TURN_ON)) {
            return MOBILE_APP_COMMAND_LIGHT_YELLOW;
        }
    }

    if (has_i(text, "mute") || has(text, ZH_MUTE)) return MOBILE_APP_COMMAND_AUDIO_MUTE;
    if ((has_i(text, "volume") || has(text, ZH_VOLUME)) &&
        (has_i(text, "up") || has_i(text, "louder") || has(text, ZH_LOUD))) {
        return MOBILE_APP_COMMAND_AUDIO_VOLUME_UP;
    }
    if ((has_i(text, "volume") || has(text, ZH_VOLUME)) &&
        (has_i(text, "down") || has_i(text, "lower") || has(text, ZH_SMALL))) {
        return MOBILE_APP_COMMAND_AUDIO_VOLUME_DOWN;
    }
    if (has_i(text, "max volume")) return MOBILE_APP_COMMAND_AUDIO_VOLUME_MAX;

    if (has_i(text, "story") || has(text, ZH_STORY)) return MOBILE_APP_COMMAND_STORY;
    if (has_i(text, "breath") || has(text, ZH_BREATH)) {
        if (has_i(text, "box") || has_i(text, "4-4")) return MOBILE_APP_COMMAND_BREATH_BOX;
        if (has_i(text, "4-7-8") || has_i(text, "deep")) return MOBILE_APP_COMMAND_BREATH_DEEP;
        return MOBILE_APP_COMMAND_AUDIO_BREATHING;
    }
    if (has_i(text, "rain") || has(text, ZH_RAIN)) {
        return has_i(text, "play") || has(text, ZH_PLAY) ? MOBILE_APP_COMMAND_SCENE_RAIN_PLAY : MOBILE_APP_COMMAND_SCENE_RAIN;
    }
    if (has_i(text, "ocean") || has_i(text, "wave") || has(text, ZH_OCEAN) || has(text, ZH_WAVE)) {
        return has_i(text, "play") || has(text, ZH_PLAY) ? MOBILE_APP_COMMAND_SCENE_OCEAN_PLAY : MOBILE_APP_COMMAND_SCENE_OCEAN;
    }
    if (has_i(text, "forest") || has(text, ZH_FOREST)) {
        return has_i(text, "play") || has(text, ZH_PLAY) ? MOBILE_APP_COMMAND_SCENE_FOREST_PLAY : MOBILE_APP_COMMAND_SCENE_FOREST;
    }
    if (has_i(text, "zen") || has_i(text, "meditation") || has(text, ZH_ZEN) || has(text, ZH_MEDITATE)) {
        return has_i(text, "play") || has(text, ZH_PLAY) ? MOBILE_APP_COMMAND_SCENE_ZEN_PLAY : MOBILE_APP_COMMAND_SCENE_ZEN;
    }
    if (has_i(text, "noise") || has(text, ZH_NOISE) || has(text, ZH_WHITE_NOISE) || has(text, ZH_PINK_NOISE)) {
        return MOBILE_APP_COMMAND_AUDIO_NOISE;
    }
    if (has_i(text, "music") || has_i(text, "song") || has(text, ZH_MUSIC)) {
        return MOBILE_APP_COMMAND_AUDIO_MUSIC;
    }
    if (has_i(text, "test minor")) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_MINOR;
    if (has_i(text, "test restless")) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_RESTLESS;
    if (has_i(text, "test arousal") || (has(text, ZH_TEST) && has_i(text, "arousal"))) {
        return MOBILE_APP_COMMAND_MID_SLEEP_TEST_AROUSAL;
    }
    if (has_i(text, "out of bed")) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_OUT_OF_BED;
    return MOBILE_APP_COMMAND_NONE;
}

static bool parse_route_json(const char *text, llm_intent_result_t *result)
{
    const char *start = text ? strchr(text, '{') : NULL;
    const char *end = text ? strrchr(text, '}') : NULL;
    if (!start || !end || end <= start || !result) return false;
    size_t len = (size_t)(end - start + 1);
    char *json = strndup(start, len);
    if (!json) return false;
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return false;

    cJSON *cmd = cJSON_GetObjectItem(root, "command");
    cJSON *reply = cJSON_GetObjectItem(root, "reply");
    if (cJSON_IsString(cmd)) {
        result->command = llm_intent_command_from_name(cmd->valuestring);
        snprintf(result->command_name, sizeof(result->command_name), "%s", cmd->valuestring);
    }
    if (cJSON_IsString(reply) && reply->valuestring) {
        snprintf(result->reply, sizeof(result->reply), "%s", reply->valuestring);
    }
    cJSON_Delete(root);
    return true;
}

static esp_err_t parse_openai_response(const char *response, llm_intent_result_t *result)
{
    cJSON *root = cJSON_Parse(response);
    if (!root) return ESP_FAIL;
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = cJSON_IsObject(first) ? cJSON_GetObjectItem(first, "message") : NULL;
    cJSON *content = cJSON_IsObject(message) ? cJSON_GetObjectItem(message, "content") : NULL;
    bool ok = cJSON_IsString(content) && parse_route_json(content->valuestring, result);
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t parse_anthropic_response(const char *response, llm_intent_result_t *result)
{
    cJSON *root = cJSON_Parse(response);
    if (!root) return ESP_FAIL;
    cJSON *content = cJSON_GetObjectItem(root, "content");
    cJSON *first = cJSON_IsArray(content) ? cJSON_GetArrayItem(content, 0) : NULL;
    cJSON *text = cJSON_IsObject(first) ? cJSON_GetObjectItem(first, "text") : NULL;
    bool ok = cJSON_IsString(text) && parse_route_json(text->valuestring, result);
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_FAIL;
}

static const char *ROUTER_PROMPT =
    "You are the DreamGuardian sleep device intent router. Classify Chinese or English voice transcripts. "
    "Return ONLY compact JSON: {\"command\":\"...\",\"reply\":\"...\"}. "
    "Allowed commands: none, sleep.start, sleep.stop, self_test, wake.ack, "
    "light.red, light.green, light.blue, light.yellow, light.off, screen.red, screen.green, screen.blue, screen.yellow, screen.off, "
    "audio.music, audio.noise, audio.breathing, audio.volume.max, audio.volume.up, audio.volume.down, audio.mute, "
    "story.play, scene.ppm, scene.ocean, scene.forest, scene.rain, scene.zen, scene.empty, "
    "scene.ppm.play, scene.ocean.play, scene.forest.play, scene.rain.play, scene.zen.play, "
    "breath.relax, breath.box, breath.deep, mid_sleep.test_minor, mid_sleep.test_restless, "
    "mid_sleep.test_arousal, mid_sleep.test_out_of_bed. "
    "Map sleep aid, bedtime, start sleep mode to sleep.start. Deep-sleep lock is touch-only, so map spoken deep-sleep requests to none. Map wake up, stop, cancel, quiet, exit to sleep.stop unless the user clearly asks only to turn off light/audio. "
    "Map LED/light strip colors to light.*, display/screen colors to screen.*. "
    "Map white noise, pink noise, rain masking, ambient noise to audio.noise; music/song to audio.music; breathing guidance to audio.breathing or breath.* when a method is named. "
    "Map ocean/waves, forest, rain, zen/meditation scenes to scene.* and use scene.*.play when the user asks to play it now. "
    "Use none for advice or conversation without a direct device action. Never invent commands.";

static esp_err_t route_with_llm(const char *text, llm_intent_result_t *result)
{
    llm_config_t cfg = {0};
    char url[LLM_URL_LEN + 32];
    char *body_str = NULL;
    char *response = NULL;
    int status = 0;
    esp_err_t err;

    ESP_RETURN_ON_ERROR(load_llm_config(&cfg), TAG, "load llm config");
    cJSON *body = cJSON_CreateObject();
    if (!body) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(body, "model", cfg.model);

    if (strstr(cfg.backend_type, "anthropic")) {
        join_api_path(url, sizeof(url), cfg.base_url, "/messages");
        cJSON_AddStringToObject(body, "system", ROUTER_PROMPT);
        cJSON_AddNumberToObject(body, "max_tokens", 160);
        cJSON *messages = cJSON_AddArrayToObject(body, "messages");
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", text);
        cJSON_AddItemToArray(messages, msg);
    } else {
        join_api_path(url, sizeof(url), cfg.base_url, "/chat/completions");
        const char *tokens_field = cfg.max_tokens_field[0] ? cfg.max_tokens_field : "max_tokens";
        cJSON_AddNumberToObject(body, tokens_field, 160);
        cJSON *messages = cJSON_AddArrayToObject(body, "messages");
        cJSON *sys = cJSON_CreateObject();
        cJSON *usr = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", ROUTER_PROMPT);
        cJSON_AddStringToObject(usr, "role", "user");
        cJSON_AddStringToObject(usr, "content", text);
        cJSON_AddItemToArray(messages, sys);
        cJSON_AddItemToArray(messages, usr);
    }

    body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return ESP_ERR_NO_MEM;

    err = post_json(url, &cfg, body_str, &response, &status);
    if (err == ESP_OK) {
        err = strstr(cfg.backend_type, "anthropic") ?
              parse_anthropic_response(response, result) :
              parse_openai_response(response, result);
        set_status(err, status, err == ESP_OK ? "llm_intent_ok" : "llm_parse_failed");
        if (err == ESP_OK) {
            result->source = LLM_INTENT_SOURCE_LLM;
            result->http_status = status;
        }
    }

    free(response);
    free(body_str);
    return err;
}

const char *llm_intent_command_reply(mobile_app_command_t command, esp_err_t err)
{
    if (err != ESP_OK) return "Command recognized, but execution failed.";
    switch (command) {
    case MOBILE_APP_COMMAND_SLEEP: return "Sleep mode started.";
    case MOBILE_APP_COMMAND_SLEPT: return "Stable sleep lock mode started.";
    case MOBILE_APP_COMMAND_STOP: return "Current sleep intervention stopped.";
    case MOBILE_APP_COMMAND_SELF_TEST: return "Self test started.";
    case MOBILE_APP_COMMAND_LIGHT_RED: return "LED strip set to red.";
    case MOBILE_APP_COMMAND_LIGHT_GREEN: return "LED strip set to green.";
    case MOBILE_APP_COMMAND_LIGHT_BLUE: return "LED strip set to blue.";
    case MOBILE_APP_COMMAND_LIGHT_YELLOW: return "LED strip set to yellow.";
    case MOBILE_APP_COMMAND_LIGHT_OFF: return "LED strip turned off.";
    case MOBILE_APP_COMMAND_LIGHT_BRIGHTER: return "LED strip brightness increased.";
    case MOBILE_APP_COMMAND_LIGHT_DIMMER: return "LED strip brightness decreased.";
    case MOBILE_APP_COMMAND_SCREEN_RED: return "Screen set to red.";
    case MOBILE_APP_COMMAND_SCREEN_GREEN: return "Screen set to green.";
    case MOBILE_APP_COMMAND_SCREEN_BLUE: return "Screen set to blue.";
    case MOBILE_APP_COMMAND_SCREEN_YELLOW: return "Screen set to yellow.";
    case MOBILE_APP_COMMAND_SCREEN_OFF: return "Screen returned to normal.";
    case MOBILE_APP_COMMAND_AUDIO_MUSIC: return "Music playback started.";
    case MOBILE_APP_COMMAND_AUDIO_NOISE: return "Noise masking playback started.";
    case MOBILE_APP_COMMAND_AUDIO_BREATHING: return "Breathing guide playback started.";
    case MOBILE_APP_COMMAND_AUDIO_VOLUME_MAX: return "Volume set to maximum.";
    case MOBILE_APP_COMMAND_AUDIO_VOLUME_UP: return "Volume increased.";
    case MOBILE_APP_COMMAND_AUDIO_VOLUME_DOWN: return "Volume decreased.";
    case MOBILE_APP_COMMAND_AUDIO_MUTE: return "Audio muted.";
    case MOBILE_APP_COMMAND_STORY: return "Bedtime story mode started.";
    case MOBILE_APP_COMMAND_WAKE_ACK: return "Wake acknowledgement played.";
    case MOBILE_APP_COMMAND_SCENE_OCEAN:
    case MOBILE_APP_COMMAND_SCENE_OCEAN_PLAY: return "Ocean sleep scene selected.";
    case MOBILE_APP_COMMAND_SCENE_FOREST:
    case MOBILE_APP_COMMAND_SCENE_FOREST_PLAY: return "Forest sleep scene selected.";
    case MOBILE_APP_COMMAND_SCENE_RAIN:
    case MOBILE_APP_COMMAND_SCENE_RAIN_PLAY: return "Rain sleep scene selected.";
    case MOBILE_APP_COMMAND_SCENE_ZEN:
    case MOBILE_APP_COMMAND_SCENE_ZEN_PLAY: return "Zen sleep scene selected.";
    case MOBILE_APP_COMMAND_SCENE_PPM:
    case MOBILE_APP_COMMAND_SCENE_PPM_PLAY: return "Default rhythm sleep scene selected.";
    case MOBILE_APP_COMMAND_SCENE_EMPTY: return "Custom scene cleared.";
    case MOBILE_APP_COMMAND_BREATH_RELAX: return "Relaxed 5-5 breathing selected.";
    case MOBILE_APP_COMMAND_BREATH_BOX: return "Box breathing selected.";
    case MOBILE_APP_COMMAND_BREATH_DEEP: return "Deep 4-7-8 breathing selected.";
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_MINOR: return "Minor disturbance test started.";
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_RESTLESS: return "Restless sleep test started.";
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_AROUSAL: return "Arousal risk test started.";
    case MOBILE_APP_COMMAND_MID_SLEEP_TEST_OUT_OF_BED: return "Out-of-bed night light test started.";
    default: return "No direct device action recognized.";
    }
}

esp_err_t llm_intent_resolve_text(const char *text, bool allow_cloud, llm_intent_result_t *result)
{
    if (!text || !result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    result->error = ESP_ERR_NOT_FOUND;

    mobile_app_command_t local = llm_intent_local_match(text);
    if (local != MOBILE_APP_COMMAND_NONE) {
        result->command = local;
        result->source = LLM_INTENT_SOURCE_LOCAL;
        snprintf(result->command_name, sizeof(result->command_name), "%s",
                 llm_intent_command_to_name(local));
        snprintf(result->reply, sizeof(result->reply), "%s", llm_intent_command_reply(local, ESP_OK));
        result->error = ESP_OK;
        inc_source(result->source);
        set_status(ESP_OK, 0, "local_intent_ok");
        printf("DG Intent: text=\"%s\" source=local command=%s\n",
               text, result->command_name);
        return ESP_OK;
    }

    if (allow_cloud) {
        printf("DG Intent: text=\"%s\" source=llm_request\n", text);
        esp_err_t llm_err = route_with_llm(text, result);
        if (llm_err == ESP_OK) {
            result->error = ESP_OK;
            if (!result->command_name[0]) {
                snprintf(result->command_name, sizeof(result->command_name), "%s",
                         llm_intent_command_to_name(result->command));
            }
            if (!result->reply[0]) {
                snprintf(result->reply, sizeof(result->reply), "%s",
                         llm_intent_command_reply(result->command, ESP_OK));
            }
            inc_source(LLM_INTENT_SOURCE_LLM);
            printf("DG Intent: text=\"%s\" source=llm command=%s http=%d\n",
                   text, result->command_name, result->http_status);
            return ESP_OK;
        }
        printf("DG Intent: text=\"%s\" source=llm error=%s\n",
               text, esp_err_to_name(llm_err));
        result->error = llm_err;
    }

    snprintf(result->command_name, sizeof(result->command_name), "%s", "none");
    snprintf(result->reply, sizeof(result->reply), "%s", llm_intent_command_reply(MOBILE_APP_COMMAND_NONE, ESP_OK));
    inc_source(LLM_INTENT_SOURCE_NONE);
    printf("DG Intent: text=\"%s\" source=none error=%s\n",
           text, esp_err_to_name(result->error));
    return result->error == ESP_OK ? ESP_ERR_NOT_FOUND : result->error;
}

void llm_intent_get_status(llm_intent_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}
