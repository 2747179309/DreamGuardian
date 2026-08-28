#include "mobile_app.h"

#include "ai_bridge.h"
#include "audio_monitor.h"
#include "sleep_ui.h"
#include "../storage/include/sleep_history.h"
#include "stereo_audio.h"
#include "voice_sr.h"
#include "online_asr.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "feishu_agent.h"
#include "llm_intent.h"
#include "freertos/task.h"
#include "status_light.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "time_sync.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include <sys/stat.h>

#define MOBILE_NVS_NAMESPACE "dg_wifi"
#define MOBILE_NVS_KEY_SSID "ssid"
#define MOBILE_NVS_KEY_PASS "pass"
#define MOBILE_NVS_KEY_PROFILE_COUNT "count"
/* Keep exactly one STA network. A newly provisioned hotspot replaces the
 * previous one, so the device cannot silently fail over to an old venue. */
#define WIFI_PROFILE_MAX 1
#define WIFI_PROFILE_STORAGE_SLOTS 5
#define CLAW_NVS_NAMESPACE "dg_claw"
#define CLAW_NVS_PROVIDER "provider"
#define CLAW_NVS_BACKEND_TYPE "backend_type"
#define CLAW_NVS_MODEL "model"
#define CLAW_NVS_BASE_URL "base_url"
#define CLAW_NVS_API_KEY "api_key"
#define CLAW_NVS_AUTH_TYPE "auth_type"
#define CLAW_NVS_MAX_TOKENS "max_tokens"
#define CLAW_NVS_TIMEOUT_MS "timeout_ms"
#define CLAW_NVS_IMAGE_MAX_BYTES "image_max"
#define CLAW_NVS_MAX_TOKENS_FIELD "tokens_field"
#define CLAW_NVS_SUPPORTS_TOOLS "tools"
#define CLAW_NVS_SUPPORTS_VISION "vision"
#define CLAW_NVS_IMAGE_REMOTE_ONLY "remote_img"
#define CLAW_NVS_QQ_APP_ID "qq_app_id"
#define CLAW_NVS_QQ_APP_SECRET "qq_secret"
#define CLAW_NVS_QQ_MSG_TYPE "qq_msg"
#define CLAW_NVS_FEISHU_APP_ID "fs_app_id"
#define CLAW_NVS_FEISHU_APP_SECRET "fs_secret"
#define CLAW_NVS_TG_BOT_TOKEN "tg_token"
#define CLAW_NVS_WECHAT_TOKEN "wx_token"
#define CLAW_NVS_WECHAT_BASE_URL "wx_base"
#define CLAW_NVS_WECHAT_CDN_URL "wx_cdn"
#define CLAW_NVS_WECHAT_ACCOUNT_ID "wx_acct"
#define CLAW_NVS_ASR_ENABLED "asr_enabled"
#define CLAW_NVS_ASR_MODEL "asr_model"
#define CLAW_NVS_ASR_BASE_URL "asr_base_url"
#define CLAW_NVS_ASR_API_KEY "asr_api_key"
#define CLAW_NVS_ASR_LANGUAGE "asr_language"
#define CLAW_NVS_ASR_TIMEOUT_MS "asr_timeout_ms"
#define PREF_NVS_NAMESPACE "dg_prefs"
#define PREF_NVS_SCENE "scene"
#define PREF_NVS_BREATH "breath"
#define PREF_COUNT_PREFIX "cnt_"
#define WAKE_NVS_NAMESPACE "dg_wake"
#define WAKE_NVS_PHRASE "phrase"
#define WAKE_NVS_UPLOAD_COUNT "upload_count"
#define WAKE_NVS_LAST_NAME "last_name"
#define WAKE_NVS_LAST_BYTES "last_bytes"
#define SETUP_AP_DEFAULT_SSID "DreamGuardian-Control"
#define SETUP_AP_DEFAULT_PASSWORD "CHANGE_ME"
#define SETUP_AP_DEFAULT_CHANNEL 6
#define WIFI_STA_RETRY_INTERVAL_US 1000000LL

#define CLAW_FIELD_PROVIDER_MAX 32
#define CLAW_FIELD_BACKEND_MAX 32
#define CLAW_FIELD_MODEL_MAX 64
#define CLAW_FIELD_URL_MAX 256
#define CLAW_FIELD_API_KEY_MAX 256
#define CLAW_FIELD_AUTH_MAX 32
#define CLAW_FIELD_NUM_MAX 16
#define CLAW_FIELD_BOOL_MAX 8
#define CLAW_FIELD_APP_ID_MAX 64
#define CLAW_FIELD_SECRET_MAX 256
#define CLAW_FIELD_TOKEN_MAX 256
#define CLAW_FIELD_MSG_MAX 8
#define AUDIO_UPLOAD_MAX_BYTES (6 * 1024 * 1024)
#define WAKE_SAMPLE_MAX_BYTES (512 * 1024)
#define WAKE_SAMPLE_MAX_COUNT 20

static const char *TAG = "mobile_app";

typedef struct {
    int64_t start_us;
    int64_t last_us;
    uint32_t samples;
    uint32_t present_samples;
    uint32_t valid_samples;
    uint32_t assist_active_samples;
    uint32_t sleep_locked_samples;
    uint32_t high_risk_samples;
    double breath_sum;
    double heart_sum;
    double motion_sum;
    double stability_sum;
    double score_sum;
    double risk_sum;
    float breath_min;
    float breath_max;
    float heart_min;
    float heart_max;
    uint8_t max_wake_risk;
} sleep_profile_state_t;

typedef struct {
    mobile_app_config_t cfg;
    httpd_handle_t server;
    esp_netif_t *ap_netif;
    esp_netif_t *sta_netif;
    mobile_app_telemetry_t telemetry;
    sleep_profile_state_t profile;
    bool wifi_started;
    bool sta_has_credentials;
    bool sta_connected;
    uint32_t sta_retry_count;
    uint32_t sta_config_version;
    int64_t last_sta_retry_us;
    uint8_t sta_profile_count;
    uint8_t sta_profile_index;
    uint8_t sta_profiles_tried_in_cycle;
    char sta_profiles_ssid[WIFI_PROFILE_MAX][33];
    char sta_profiles_password[WIFI_PROFILE_MAX][65];
    char sta_ssid[33];
    char sta_password[65];
    char sta_ip[16];
    portMUX_TYPE lock;
} mobile_app_ctx_t;

typedef struct {
    char provider[CLAW_FIELD_PROVIDER_MAX];
    char backend_type[CLAW_FIELD_BACKEND_MAX];
    char model[CLAW_FIELD_MODEL_MAX];
    char base_url[CLAW_FIELD_URL_MAX];
    char api_key[CLAW_FIELD_API_KEY_MAX];
    char auth_type[CLAW_FIELD_AUTH_MAX];
    char max_tokens[CLAW_FIELD_NUM_MAX];
    char timeout_ms[CLAW_FIELD_NUM_MAX];
    char default_image_max_bytes[CLAW_FIELD_NUM_MAX];
    char max_tokens_field[CLAW_FIELD_BACKEND_MAX];
    char supports_tools[CLAW_FIELD_BOOL_MAX];
    char supports_vision[CLAW_FIELD_BOOL_MAX];
    char image_remote_url_only[CLAW_FIELD_BOOL_MAX];
} claw_llm_config_t;

typedef struct {
    char qq_app_id[CLAW_FIELD_APP_ID_MAX];
    char qq_app_secret[CLAW_FIELD_SECRET_MAX];
    char qq_msg_type[CLAW_FIELD_MSG_MAX];
    char feishu_app_id[CLAW_FIELD_APP_ID_MAX];
    char feishu_app_secret[CLAW_FIELD_SECRET_MAX];
    char tg_bot_token[CLAW_FIELD_TOKEN_MAX];
    char wechat_token[CLAW_FIELD_TOKEN_MAX];
    char wechat_base_url[CLAW_FIELD_URL_MAX];
    char wechat_cdn_base_url[CLAW_FIELD_URL_MAX];
    char wechat_account_id[CLAW_FIELD_APP_ID_MAX];
} claw_im_config_t;

typedef struct {
    char enabled[CLAW_FIELD_BOOL_MAX];
    char model[CLAW_FIELD_MODEL_MAX];
    char base_url[CLAW_FIELD_URL_MAX];
    char api_key[CLAW_FIELD_API_KEY_MAX];
    char language[CLAW_FIELD_MSG_MAX];
    char timeout_ms[CLAW_FIELD_NUM_MAX];
} claw_asr_config_t;

static mobile_app_ctx_t s_app = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};
static esp_err_t s_http_start_result = ESP_ERR_INVALID_STATE;
static esp_err_t s_wifi_start_result = ESP_ERR_INVALID_STATE;
static uint16_t s_http_port = 80;
static int64_t s_last_http_watchdog_us;
static uint8_t s_http_watchdog_failures;

static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>DreamGuardian Go</title>"
"<style>body{margin:0;background:#050201;color:#f8f2e8;font-family:Arial,Helvetica,sans-serif}"
"header{padding:18px 16px;background:#14100d;border-bottom:1px solid #2a211b}h1{margin:0;font-size:22px}"
"main{padding:14px;display:grid;gap:12px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}"
".card{background:#15110e;border:1px solid #30251d;border-radius:8px;padding:12px}.wide{grid-column:1/-1}"
".k{color:#b7a99b;font-size:12px}.v{font-size:26px;font-weight:700;margin-top:5px}.s{font-size:14px;color:#d7c7b8;line-height:1.5}"
"a{color:#ff9a42}button{height:42px;border:0;border-radius:6px;background:#ff7a1a;color:#160a02;font-weight:700;font-size:15px}"
"input{width:100%;height:38px;box-sizing:border-box;margin-top:8px;border-radius:6px;border:1px solid #4a3a2f;background:#0d0a08;color:#f8f2e8;padding:0 10px;font-size:14px}"
"input[type=checkbox]{width:auto;height:auto;margin:0 8px 0 0}"
"button.secondary{background:#3a3028;color:#f8f2e8}.bar{height:8px;background:#332820;border-radius:8px;overflow:hidden}.bar i{display:block;height:100%;background:#ff7a1a;width:0}"
"</style></head><body><header><h1>DreamGuardian Go</h1><div class='s'>Local sleep dashboard</div></header>"
"<main><div class='grid'>"
"<div class='card wide'><div class='k'>Network</div><div class='s' id='net'>--</div><div class='s'><a href='/provision'>Wi-Fi setup</a> | <a href='/claw'>Claw config</a></div></div>"
"<div class='card wide'><div class='k'>AI Bridge</div><div class='s' id='ai'>--</div><label class='s'><input id='aien' type='checkbox'>Enable Feishu webhook</label><input id='aiurl' placeholder='Feishu webhook URL'><input id='aiint' type='number' min='30' max='3600' placeholder='Minimum event interval seconds'><div class='grid' style='margin-top:10px'><button onclick=aiSave()>Save AI</button><button class='secondary' onclick=aiTest()>Test AI</button></div></div>"
"<div class='card wide'><div class='k'>Personal Sleep Plan</div><div class='s' id='profile'>--</div></div>"
"<div class='card'><div class='k'>Presence</div><div class='v' id='presence'>--</div></div>"
"<div class='card'><div class='k'>Assist</div><div class='v' id='assist'>--</div></div>"
"<div class='card'><div class='k'>Breath</div><div class='v'><span id='breath'>--</span></div></div>"
"<div class='card'><div class='k'>Heart</div><div class='v'><span id='heart'>--</span></div></div>"
"<div class='card'><div class='k'>Sleep Score</div><div class='v' id='score'>--</div></div>"
"<div class='card'><div class='k'>Wake Risk</div><div class='v' id='risk'>--</div></div>"
"<div class='card wide'><div class='k'>Wi-Fi CSI Stability</div><div class='v' id='csi'>--</div><div class='bar'><i id='csibar'></i></div><div class='s' id='csitxt'></div></div>"
"<div class='card wide'><div class='k'>Time</div><div class='s' id='time'>--</div></div>"
"<div class='card wide'><div class='k'>Status</div><div class='s' id='status'>--</div></div>"
"</div><div class='grid'><button onclick=cmd('sleep')>Start Sleep</button><button class='secondary' onclick=cmd('stop')>Stop</button><button class='secondary' onclick=cmd('selftest')>Self Test</button><button class='secondary' onclick=load()>Refresh</button></div></main>"
"<script>async function cmd(c){await fetch('/api/command?cmd='+c,{method:'POST'});setTimeout(load,300)}"
"async function aiSave(){let b='enabled='+(aien.checked?'1':'0')+'&interval='+encodeURIComponent(aiint.value||'180');if(aiurl.value)b+='&webhook='+encodeURIComponent(aiurl.value);await fetch('/api/ai_bridge',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});aiurl.value='';setTimeout(load,300)}"
"async function aiTest(){await fetch('/api/ai_bridge_test',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'text='+encodeURIComponent('DreamGuardian Go test event')});setTimeout(load,800)}"
"function f(x,d=1){return (x===undefined||x===null)?'--':Number(x).toFixed(d)}"
"async function load(){let r=await fetch('/api/dashboard');let j=await r.json();"
"net.textContent=j.wifi.connected?('Connected to '+j.wifi.ssid+' at '+j.wifi.ip):'Not connected to router. Open Wi-Fi setup.';"
"aien.checked=!!j.ai_bridge.enabled;aiint.value=j.ai_bridge.min_interval_sec||180;ai.textContent=(j.ai_bridge.enabled?'Enabled':'Disabled')+', '+(j.ai_bridge.configured?'configured':'not configured')+', sent '+j.ai_bridge.sent+', dropped '+j.ai_bridge.dropped+', last '+j.ai_bridge.last_result;"
"presence.textContent=j.presence?'YES':'NO';assist.textContent=j.assist.state;breath.textContent=f(j.bio.breath_bpm,1);heart.textContent=f(j.bio.heart_bpm,1);"
"score.textContent=j.sleep.score;risk.textContent=j.sleep.wake_risk;csi.textContent=Math.round(j.csi.stability*100)+'%';csibar.style.width=Math.round(j.csi.stability*100)+'%';"
"profile.textContent='samples '+j.profile.samples+', avg score '+f(j.profile.averages?j.profile.averages.sleep_score:0,1)+', avg risk '+f(j.profile.averages?j.profile.averages.wake_risk:0,1)+' | '+j.profile.recommendation;"
"time.textContent=j.time.valid?(j.time.date+' WEEK '+j.time.weekday+' '+j.time.clock):'Waiting for network time';"
"csitxt.textContent='packets '+j.csi.packets+', rssi '+j.csi.rssi+', motion '+f(j.csi.motion,2)+', breath proxy '+(j.csi.breath_valid?f(j.csi.breath_bpm,1)+' bpm':'calibrating');"
"status.textContent='state '+j.sleep.state+' | action '+j.action+' | target '+f(j.assist.target_breath,1)+' bpm | uptime '+j.uptime_sec+'s';}"
"setInterval(load,2000);load();</script></body></html>";

static const char PROVISION_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>DreamGuardian Wi-Fi Setup</title><style>body{margin:0;background:#050201;color:#f8f2e8;font-family:Arial,Helvetica,sans-serif}main{padding:18px;display:grid;gap:14px}.card{background:#15110e;border:1px solid #30251d;border-radius:8px;padding:14px}input,button{width:100%;height:44px;box-sizing:border-box;margin-top:8px;border-radius:6px;border:1px solid #4a3a2f;background:#0d0a08;color:#f8f2e8;padding:0 10px;font-size:16px}button{background:#ff7a1a;color:#160a02;border:0;font-weight:700}.s{color:#d7c7b8;line-height:1.5;font-size:14px}a{color:#ff9a42}</style></head>"
"<body><main><h2>DreamGuardian Wi-Fi Setup</h2><div class='card'><div class='s'>Enter the router or phone hotspot Wi-Fi. After the device connects, switch your phone back to the same Wi-Fi and open the shown device IP.</div>"
"<form method='post' action='/api/provision'><input name='ssid' placeholder='Wi-Fi SSID' maxlength='32' required><input name='pass' placeholder='Wi-Fi password' maxlength='64' type='password'><button type='submit'>Save and Connect</button></form></div>"
"<div class='card s'>Current setup AP: DreamGuardian-Setup / dream1234<br>Setup URL: http://192.168.4.1/provision<br><a href='/'>Dashboard</a> | <a href='/claw'>Claw config</a></div></main></body></html>";

static const char CLAW_HTML[] __attribute__((unused)) =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>DreamGuardian ESP-Claw Config</title><style>"
"body{margin:0;background:#0b0f14;color:#e8edf2;font-family:Arial,'Microsoft YaHei',sans-serif}header{padding:16px 18px;background:#121820;border-bottom:1px solid #25313d}"
"h1{margin:0;font-size:22px}.sub{color:#93a1ad;font-size:13px;margin-top:4px}main{max-width:1040px;margin:auto;padding:14px;display:grid;gap:12px}.top{display:flex;gap:12px;align-items:center;justify-content:space-between}"
".nav{display:flex;gap:8px;flex-wrap:wrap}.tab{width:auto;height:36px;padding:0 14px;border-radius:6px;border:1px solid #334250;background:#151d26;color:#cbd6df}.tab.on{background:#2cc2a3;color:#03120e;border-color:#2cc2a3}"
".panel{border:1px solid #293743;background:#121a22;border-radius:8px}.head{padding:13px 15px;border-bottom:1px solid #24313c;font-weight:700}.body{padding:14px;display:grid;gap:12px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}.row{display:grid;gap:7px}"
"label{font-size:12px;color:#a7b3be}input,select,button{height:40px;box-sizing:border-box;border-radius:6px;border:1px solid #3b4a57;background:#0a1016;color:#eef3f6;padding:0 10px;font-size:14px;width:100%}"
"button{background:#2cc2a3;color:#04100d;border:0;font-weight:700}.secondary{background:#25313c;color:#e8edf2}.danger{background:#40303a;color:#ffd7df}.muted{color:#93a1ad;font-size:13px;line-height:1.5}.status{color:#d7e1e8;font-size:13px;min-height:18px}.chips{display:flex;gap:8px;flex-wrap:wrap}.chip{width:auto;height:34px;background:#26313c;color:#e8edf2;padding:0 12px}.ok{color:#66e1b6}.warn{color:#ffc26b}a{color:#66c7ff}.hidden{display:none}"
"@media(max-width:720px){.grid{grid-template-columns:1fr}.top{align-items:flex-start;flex-direction:column}}</style></head><body>"
"<header><h1>ESP-Claw Config</h1><div class='sub'>DreamGuardian native-style LLM / IM / device configuration</div></header><main>"
"<div class='top'><div class='nav'><button id='tab_llm' class='tab on' onclick=\"show('llm')\">LLM 设置</button><button id='tab_im' class='tab' onclick=\"show('im')\">即时通讯 IM</button><button id='tab_device' class='tab' onclick=\"show('device')\">设备能力</button></div><div class='muted'><a href='/'>Dashboard</a> | <a href='/api/claw_tools'>Tools JSON</a></div></div>"
"<div class='panel'><div class='body'><div id='status' class='status'>Loading...</div></div></div>"
"<section id='page_llm' class='panel'><div class='head'>LLM 设置</div><div class='body'>"
"<div class='muted'>选择供应商会填入 ESP-Claw 常用默认值。API Key 留空保存时会保留已保存密钥。</div><div class='chips'>"
"<button class='chip' onclick=\"preset('openai')\">OpenAI</button><button class='chip' onclick=\"preset('bailian')\">百炼 / Qwen</button><button class='chip' onclick=\"preset('anthropic')\">Anthropic</button><button class='chip' onclick=\"preset('openai_compatible')\">OpenAI 兼容</button><button class='chip' onclick=\"preset('anthropic_compatible')\">Anthropic 兼容</button></div>"
"<div class='grid'><div class='row'><label>API 密钥</label><input id='llm_api_key' type='password' placeholder='留空保留已保存 API Key'></div><div class='row'><label>模型</label><input id='llm_model' placeholder='qwen3.5-omni-flash'></div>"
"<div class='row'><label>最大 Token 数</label><input id='llm_max_tokens' type='number' placeholder='8192'></div><div class='row'><label>超时 (毫秒)</label><input id='llm_timeout_ms' type='number' placeholder='120000'></div>"
"<div class='row'><label>后端类型</label><input id='llm_backend_type' placeholder='openai_compatible / anthropic_compatible'></div><div class='row'><label>Base URL</label><input id='llm_base_url' placeholder='https://dashscope.aliyuncs.com/compatible-mode/v1'></div>"
"<div class='row'><label>认证类型</label><input id='llm_auth_type' placeholder='bearer / none'></div><div class='row'><label>Max Tokens 字段名</label><input id='llm_max_tokens_field' placeholder='max_tokens / max_completion_tokens'></div>"
"<div class='row'><label>默认图片大小上限</label><input id='llm_default_image_max_bytes' type='number' placeholder='524288'></div><div class='row'><label>能力</label><select id='llm_supports_tools'><option value='true'>支持工具调用</option><option value='false'>不支持工具调用</option></select></div>"
"<div class='row'><label>视觉输入</label><select id='llm_supports_vision'><option value='true'>支持视觉</option><option value='false'>不支持视觉</option></select></div><div class='row'><label>图片输入</label><select id='llm_image_remote_url_only'><option value='false'>允许本地/远程图片</option><option value='true'>仅远程 URL</option></select></div></div></div></section>"
"<section id='page_im' class='panel hidden'><div class='head'>即时通讯 (IM)</div><div class='body'>"
"<div class='muted'>飞书使用开放平台的智能体应用 App ID / App Secret。配置后保存并重启，后续可接入群消息到 DreamGuardian 工具。</div>"
"<div class='panel'><div class='head'>飞书</div><div class='body grid'><div class='row'><label>飞书 App ID</label><input id='feishu_app_id' placeholder='cli_xxx'></div><div class='row'><label>飞书 App Secret</label><input id='feishu_app_secret' type='password' placeholder='留空保留已保存 Secret'></div></div></div>"
"<div class='panel'><div class='head'>Telegram</div><div class='body grid'><div class='row'><label>Telegram Bot Token</label><input id='tg_bot_token' type='password' placeholder='留空保留已保存 Token'></div></div></div>"
"<div class='panel'><div class='head'>QQ</div><div class='body grid'><div class='row'><label>QQ App ID</label><input id='qq_app_id'></div><div class='row'><label>QQ App Secret</label><input id='qq_app_secret' type='password' placeholder='留空保留已保存 Secret'></div><div class='row'><label>消息类型</label><select id='qq_msg_type'><option value='0'>纯文本</option><option value='1'>Markdown</option></select></div></div></div>"
"<div class='panel'><div class='head'>微信</div><div class='body grid'><div class='row'><label>微信 Token</label><input id='wechat_token' type='password' placeholder='留空保留已保存 Token'></div><div class='row'><label>微信账号 ID</label><input id='wechat_account_id'></div><div class='row'><label>微信 Base URL</label><input id='wechat_base_url' placeholder='https://ilinkai.weixin.qq.com'></div><div class='row'><label>微信 CDN Base URL</label><input id='wechat_cdn_base_url' placeholder='https://novac2c.cdn.weixin.qq.com/c2c'></div></div></div></div></section>"
"<section id='page_device' class='panel hidden'><div class='head'>DreamGuardian 设备能力</div><div class='body'><div class='muted'>这些能力会作为 DreamGuardian 工具层提供给 ESP-Claw/LLM：睡眠画像、睡眠建议、睡眠辅助控制、状态灯控制、本地音频播放。<a href='/sleep_music'>打开 SleepTide 混音器</a></div><div id='tools' class='muted'>Loading tools...</div><div class='grid'><button onclick=\"tool('profile.summary')\">读取睡眠画像</button><button onclick=\"tool('sleep_assist.start')\">开始睡眠干预</button><button onclick=\"tool('sleep_assist.stop')\" class='secondary'>停止干预</button><button onclick=\"tool('wake.ack')\" class='secondary'>唤醒回应</button><button onclick=\"tool('light.red')\" class='secondary'>红灯测试</button><button onclick=\"tool('audio.music')\" class='secondary'>播放音乐</button><button onclick=\"tool('audio.noise')\" class='secondary'>播放白噪声</button><button onclick=\"tool('audio.breathing')\" class='secondary'>呼吸引导音</button><button onclick=\"tool('story.play')\" class='secondary'>讲故事</button></div><div class='panel'><div class='head'>助眠场景</div><div class='body'><div class='grid'><button onclick=\"tool('scene.ppm')\">默认律动</button><button onclick=\"tool('scene.ocean')\" class='secondary'>深海潮汐</button><button onclick=\"tool('scene.forest')\" class='secondary'>林海晨曦</button><button onclick=\"tool('scene.rain')\" class='secondary'>雨夜入眠</button><button onclick=\"tool('scene.zen')\" class='secondary'>太虚禅音</button><button onclick=\"tool('scene.empty')\" class='secondary'>放空归零</button></div><div class='grid'><button onclick=\"tool('breath.relax')\" class='secondary'>舒缓5-5</button><button onclick=\"tool('breath.box')\" class='secondary'>箱式4-4</button><button onclick=\"tool('breath.deep')\" class='secondary'>深眠4-7-8</button><button onclick=\"tool('sleep_assist.start')\">按当前场景助眠</button></div></div></div><div class='panel'><div class='head'>音频文件</div><div class='body'><div class='muted'>上传 16kHz / 16-bit PCM WAV：music.wav、story.wav、noise.wav、breathing.wav、zai_ne.wav。</div><div class='grid'><select id='audio_name'><option>music.wav</option><option>story.wav</option><option>noise.wav</option><option>breathing.wav</option><option>zai_ne.wav</option></select><input id='audio_file' type='file' accept='.wav,audio/wav'></div><div class='grid'><button onclick='uploadAudio()'>上传音频</button><button class='secondary' onclick='audioFiles()'>查看文件</button></div><pre id='audioout' class='muted'></pre></div></div><pre id='toolout' class='muted'></pre></div></section>"
"<div class='grid'><button onclick='save()'>保存配置</button><button class='secondary' onclick='load()'>重新加载</button></div>"
"</main><script>"
"const $=id=>document.getElementById(id);let meta={};"
"function show(p){['llm','im','device'].forEach(x=>{ $('page_'+x).classList.toggle('hidden',x!==p); $('tab_'+x).classList.toggle('on',x===p);});}"
"const presets={openai:{llm_backend_type:'openai_compatible',llm_base_url:'https://api.openai.com/v1',llm_auth_type:'bearer',llm_max_tokens_field:'max_completion_tokens',llm_default_image_max_bytes:'524288',llm_supports_tools:'true',llm_supports_vision:'true',llm_image_remote_url_only:'false',llm_model:'gpt-5.4'},bailian:{llm_backend_type:'openai_compatible',llm_base_url:'https://dashscope.aliyuncs.com/compatible-mode/v1',llm_auth_type:'bearer',llm_max_tokens_field:'max_tokens',llm_default_image_max_bytes:'524288',llm_supports_tools:'true',llm_supports_vision:'true',llm_image_remote_url_only:'false',llm_model:'qwen3.5-omni-flash'},anthropic:{llm_backend_type:'anthropic_compatible',llm_base_url:'https://api.anthropic.com/v1',llm_auth_type:'none',llm_max_tokens_field:'max_tokens',llm_default_image_max_bytes:'524288',llm_supports_tools:'true',llm_supports_vision:'true',llm_image_remote_url_only:'false',llm_model:'claude-sonnet-4-6'},openai_compatible:{llm_backend_type:'openai_compatible',llm_base_url:'https://api.openai.com/v1',llm_auth_type:'bearer',llm_max_tokens_field:'max_completion_tokens',llm_default_image_max_bytes:'524288',llm_supports_tools:'true',llm_supports_vision:'true',llm_image_remote_url_only:'false',llm_model:'gpt-5.4'},anthropic_compatible:{llm_backend_type:'anthropic_compatible',llm_base_url:'https://api.anthropic.com/v1',llm_auth_type:'none',llm_max_tokens_field:'max_tokens',llm_default_image_max_bytes:'524288',llm_supports_tools:'true',llm_supports_vision:'true',llm_image_remote_url_only:'false',llm_model:'claude-sonnet-4-6'}};"
"function preset(k){let p=presets[k];Object.keys(p).forEach(x=>$(x).value=p[x]);}"
"function setv(k,v){if($(k))$(k).value=v||''}function getv(k){return $(k)?$(k).value.trim():''}"
"async function load(){let r=await fetch('/api/config?groups=llm,im');let j=await r.json();meta=j._dreamguardian||{};['llm_backend_type','llm_model','llm_base_url','llm_auth_type','llm_timeout_ms','llm_max_tokens','llm_default_image_max_bytes','llm_max_tokens_field','llm_supports_tools','llm_supports_vision','llm_image_remote_url_only','qq_app_id','qq_msg_type','feishu_app_id','wechat_base_url','wechat_cdn_base_url','wechat_account_id'].forEach(k=>setv(k,j[k]));$('status').innerHTML='LLM Key '+(meta.llm_api_key_configured?'<span class=ok>已配置</span>':'<span class=warn>未配置</span>')+' | 飞书 '+(meta.feishu_configured?'<span class=ok>已配置</span>':'<span class=warn>未配置</span>')+' | Telegram '+(meta.tg_configured?'<span class=ok>已配置</span>':'未配置')+' | 微信 '+(meta.wechat_configured?'<span class=ok>已配置</span>':'未配置');loadTools();}"
"function patch(){let p={};['llm_backend_type','llm_model','llm_base_url','llm_auth_type','llm_timeout_ms','llm_max_tokens','llm_default_image_max_bytes','llm_max_tokens_field','llm_supports_tools','llm_supports_vision','llm_image_remote_url_only','qq_app_id','qq_msg_type','feishu_app_id','wechat_base_url','wechat_cdn_base_url','wechat_account_id'].forEach(k=>p[k]=getv(k));['llm_api_key','qq_app_secret','feishu_app_secret','tg_bot_token','wechat_token'].forEach(k=>{let v=getv(k);if(v)p[k]=v});return p}"
"async function save(){let r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(patch())});let j=await r.json();['llm_api_key','qq_app_secret','feishu_app_secret','tg_bot_token','wechat_token'].forEach(k=>setv(k,''));$('status').textContent=j.message||'saved';setTimeout(load,600)}"
"async function loadTools(){let r=await fetch('/api/claw_tools');let j=await r.json();$('tools').textContent=j.tools.map(t=>t.name).join(' / ')}"
"async function tool(t){let r=await fetch('/api/claw_tool',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'tool='+encodeURIComponent(t)});$('toolout').textContent=await r.text()}"
"async function audioFiles(){let r=await fetch('/api/audio_files');$('audioout').textContent=await r.text()}"
"async function uploadAudio(){let f=$('audio_file').files[0];if(!f){$('audioout').textContent='请选择 WAV 文件';return}if(f.size>6*1024*1024){$('audioout').textContent='文件超过 6MB，请压缩后再上传';return}$('audioout').textContent='Uploading '+f.name+' ('+f.size+' bytes)，请保持页面打开...';let r=await fetch('/api/audio_upload?name='+encodeURIComponent($('audio_name').value),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});let txt=await r.text();$('audioout').textContent=(r.ok?'OK ':'ERR '+r.status+' ')+txt;audioFiles()}"
"load();</script></body></html>";

static const char CLAW_LITE_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP-Claw Lite</title><style>body{margin:0;background:#0b0f14;color:#e8edf2;font-family:Arial,'Microsoft YaHei',sans-serif}"
"main{padding:12px;display:grid;gap:10px}h2{margin:4px 0 0}.card{border:1px solid #2b3742;background:#121a22;border-radius:8px;padding:12px;display:grid;gap:9px}"
"label{font-size:12px;color:#aab6c1}input,button{height:39px;border-radius:6px;border:1px solid #3a4855;background:#081018;color:#eef3f6;padding:0 10px;font-size:14px;box-sizing:border-box;width:100%}"
"button{background:#2cc2a3;color:#04100d;border:0;font-weight:700}.sec{background:#25313c;color:#e8edf2}.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}.s{font-size:13px;color:#9facb8;line-height:1.45}a{color:#66c7ff}@media(max-width:640px){.grid{grid-template-columns:1fr}}</style></head>"
"<body><main><h2>ESP-Claw Lite</h2><div class='s'><a href='/'>Dashboard</a> | <a href='/sleep_music'>Sleep Music</a> | <a href='/wake_word'>Wake Word</a> | <a href='/api/status'>Status JSON</a></div>"
"<div class='card'><b>LLM</b><div class='grid'><div><label>Model</label><input id='llm_model'></div><div><label>Base URL</label><input id='llm_base_url'></div><div><label>Backend</label><input id='llm_backend_type' placeholder='openai_compatible'></div><div><label>API Key</label><input id='llm_api_key' type='password' placeholder='leave blank to keep'></div></div></div>"
"<div class='card'><b>Feishu Agent</b><div class='grid'><div><label>App ID</label><input id='feishu_app_id'></div><div><label>App Secret</label><input id='feishu_app_secret' type='password' placeholder='leave blank to keep'></div></div></div>"
"<div class='grid'><button onclick='save()'>Save Config</button><button class='sec' onclick='load()'>Reload</button></div><div id='st' class='s'>Loading...</div>"
"<div class='card'><b>Device Tools</b><div class='grid'><button onclick=\"tool('sleep_assist.start')\">Start Assist</button><button class='sec' onclick=\"tool('sleep_assist.stop')\">Stop</button><button class='sec' onclick=\"tool('light.red')\">Light Red</button><button class='sec' onclick=\"tool('audio.music')\">Play Music</button><button class='sec' onclick=\"tool('wake.ack')\">Wake Ack</button><button class='sec' onclick=\"tool('profile.summary')\">Profile</button></div><pre id='out' class='s'></pre></div>"
"<div class='card'><b>Audio Upload</b><div class='s'>Upload WAV PCM 16-bit, 8000-48000 Hz, mono or stereo, up to 6MB. Use rain.wav for Rain Night preview and sleep assist.</div><div class='grid'><select id='audio_name'><option>rain.wav</option><option>music.wav</option><option>default.wav</option><option>ocean.wav</option><option>forest.wav</option><option>zen.wav</option><option>zai_ne.wav</option></select><input id='audio_file' type='file' accept='.wav,audio/wav'></div><div class='grid'><button onclick='uploadAudio()'>Upload WAV</button><button class='sec' onclick='audioFiles()'>File List</button></div><pre id='audioout' class='s'></pre></div>"
"</main><script>const ids=['llm_model','llm_base_url','llm_backend_type','feishu_app_id'];const $=x=>document.getElementById(x);"
"async function load(){let r=await fetch('/api/config?groups=llm,im');let j=await r.json();ids.forEach(k=>$(k).value=j[k]||'');let m=j._dreamguardian||{};$('st').textContent='LLM key '+(m.llm_api_key_configured?'OK':'not set')+' | Feishu '+(m.feishu_configured?'OK':'not set')}"
"function payload(){let p={};ids.forEach(k=>p[k]=$(k).value.trim());['llm_api_key','feishu_app_secret'].forEach(k=>{let v=$(k).value.trim();if(v)p[k]=v});return p}"
"async function save(){let r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload())});$('st').textContent=await r.text();['llm_api_key','feishu_app_secret'].forEach(k=>$(k).value='')}"
"async function tool(t){let r=await fetch('/api/claw_tool',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'tool='+encodeURIComponent(t)});$('out').textContent=await r.text()}"
"async function audioFiles(){let r=await fetch('/api/audio_files');$('audioout').textContent=await r.text()}"
"async function uploadAudio(){let f=$('audio_file').files[0];if(!f){$('audioout').textContent='Select a WAV file first';return}if(f.size>6*1024*1024){$('audioout').textContent='File is over 6MB. Please compress it first.';return}let name=$('audio_name').value;$('audioout').textContent='Uploading '+f.name+' ('+f.size+' bytes). Keep this page open...';let r=await fetch('/api/audio_upload?name='+encodeURIComponent(name),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});let txt=await r.text();$('audioout').textContent=(r.ok?'OK ':'ERR '+r.status+' ')+txt;audioFiles()}"
"load();</script></body></html>";

static const char SLEEP_MUSIC_LITE_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Sleep Music Lite</title><style>body{margin:0;background:#071014;color:#edf7f4;font-family:Arial,'Microsoft YaHei',sans-serif}main{padding:12px;display:grid;gap:10px}"
".card{border:1px solid #244048;background:#101b20;border-radius:8px;padding:12px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}button{height:40px;border:0;border-radius:6px;background:#2cc2a3;color:#03120f;font-weight:700}.sec{background:#263840;color:#e8f3f0}.on{outline:2px solid #ffd166}.s{color:#a8bab7;font-size:13px;line-height:1.45}a{color:#6ed0ff}@media(max-width:640px){.grid{grid-template-columns:1fr}}</style></head>"
"<body><main><h2>Sleep Music Lite</h2><div class='s'><a href='/'>Dashboard</a> | <a href='/claw'>Claw Config</a></div><div class='card'><div class='s'>Select a sleep scene, save it for sleep assist, or preview the packaged WAV through the BOX3 ES8311 speaker.</div><div id='scenes' class='grid'></div></div>"
"<div class='grid'><button onclick='saveScene()'>Confirm Scene</button><button class='sec' onclick='playScene()'>Speaker Preview</button><button class='sec' onclick=\"tool('sleep_assist.start')\">Start Assist</button><button class='sec' onclick=\"tool('sleep_assist.stop')\">Stop</button></div><pre id='out' class='s'></pre></main>"
"<script>const names={ppm:'Default Tide',ocean:'Ocean',forest:'Forest',rain:'Rain Night',zen:'Zen',empty:'Empty'};let sel='ocean';const $=x=>document.getElementById(x);"
"function draw(){let h='';Object.keys(names).forEach(k=>h+=`<button class='${k==sel?'on':''}' onclick=\"sel='${k}';draw()\">${names[k]}</button>`);$('scenes').innerHTML=h}"
"async function load(){draw()}"
"async function tool(t){let r=await fetch('/api/claw_tool',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'tool='+encodeURIComponent(t)});$('out').textContent=await r.text()}"
"async function saveScene(){await tool('scene.'+sel)}async function playScene(){if(sel==='empty'){await tool('scene.empty');return}await tool('scene.'+sel+'.play')}load();</script></body></html>";

static const char WAKE_WORD_HTML[] __attribute__((unused)) =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Wake Word Capture</title><style>body{margin:0;background:#0b0f14;color:#e8edf2;font-family:Arial,'Microsoft YaHei',sans-serif}main{padding:12px;display:grid;gap:10px;max-width:760px;margin:auto}"
".card{border:1px solid #2b3742;background:#121a22;border-radius:8px;padding:12px;display:grid;gap:10px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}"
"input,button{height:40px;border-radius:6px;border:1px solid #3a4855;background:#081018;color:#eef3f6;padding:0 10px;font-size:14px;box-sizing:border-box;width:100%}button{background:#2cc2a3;color:#04100d;border:0;font-weight:700}.sec{background:#25313c;color:#e8edf2}.danger{background:#40303a;color:#ffd7df}.s{font-size:13px;color:#a8b5bf;line-height:1.5}pre{white-space:pre-wrap}a{color:#66c7ff}@media(max-width:640px){.grid{grid-template-columns:1fr}}</style></head>"
"<body><main><h2>Wake Word Capture</h2><div class='s'><a href='/'>Dashboard</a> | <a href='/claw'>Claw Config</a> | <a href='/api/wake_samples'>Samples JSON</a></div>"
"<div class='card'><b>Current Runtime</b><div id='rt' class='s'>Loading...</div><div class='s'>The active ESP-SR WakeNet model is fixed by the model partition. These samples prepare a custom model for the target phrase; saving samples does not instantly change the acoustic wake word.</div></div>"
"<div class='card'><b>Target Phrase</b><div class='grid'><input id='phrase' value='hi 小梦'><button onclick='savePhrase()'>Save Target</button></div></div>"
"<div class='card'><b>Record Sample</b><div class='s'>Read the phrase naturally once per sample, with 0.5s quiet before and after. Capture at least 20 samples from different distances and directions.</div><div class='grid'><button onclick='startRec()'>Start</button><button class='sec' onclick='stopRec()'>Stop & Upload</button></div><div id='rec' class='s'>Idle</div></div>"
"<div class='card'><b>Upload WAV Fallback</b><div class='s'>If browser microphone access is blocked on HTTP, upload a WAV file recorded elsewhere. Recommended: mono, 16kHz, 16-bit PCM, 1-3 seconds.</div><div class='grid'><input id='file' type='file' accept='.wav,audio/wav'><button onclick='uploadFile()'>Upload WAV</button></div></div>"
"<div class='card'><b>Samples</b><pre id='out' class='s'></pre><button class='sec' onclick='load()'>Refresh</button></div>"
"</main><script>const $=x=>document.getElementById(x);let ctx,stream,src,proc,chunks=[],recording=false;"
"function wav(samples,rate){let n=samples.length,b=new ArrayBuffer(44+n*2),v=new DataView(b),p=0;function s(x){for(let i=0;i<x.length;i++)v.setUint8(p++,x.charCodeAt(i))}function u32(x){v.setUint32(p,x,true);p+=4}function u16(x){v.setUint16(p,x,true);p+=2}s('RIFF');u32(36+n*2);s('WAVEfmt ');u32(16);u16(1);u16(1);u32(rate);u32(rate*2);u16(2);u16(16);s('data');u32(n*2);for(let i=0;i<n;i++){let x=Math.max(-1,Math.min(1,samples[i]));v.setInt16(p,x<0?x*32768:x*32767,true);p+=2}return new Blob([b],{type:'audio/wav'})}"
"function downmix(buf,inRate,outRate){let ratio=inRate/outRate,len=Math.floor(buf.length/ratio),out=new Float32Array(len);for(let i=0;i<len;i++){let a=Math.floor(i*ratio),b=Math.min(buf.length,Math.floor((i+1)*ratio)),sum=0,c=0;for(let j=a;j<b;j++){sum+=buf[j];c++}out[i]=c?sum/c:0}return out}"
"async function uploadBlob(blob){let r=await fetch('/api/wake_sample_upload',{method:'POST',headers:{'Content-Type':'audio/wav'},body:blob});$('out').textContent=await r.text();load()}"
"async function startRec(){try{stream=await navigator.mediaDevices.getUserMedia({audio:true});ctx=new AudioContext();src=ctx.createMediaStreamSource(stream);proc=ctx.createScriptProcessor(4096,1,1);chunks=[];proc.onaudioprocess=e=>{if(recording)chunks.push(new Float32Array(e.inputBuffer.getChannelData(0)))};src.connect(proc);proc.connect(ctx.destination);recording=true;$('rec').textContent='Recording...'}catch(e){$('rec').textContent='Mic unavailable: '+e.message}}"
"async function stopRec(){if(!recording)return;recording=false;if(proc)proc.disconnect();if(src)src.disconnect();if(stream)stream.getTracks().forEach(t=>t.stop());let total=chunks.reduce((a,c)=>a+c.length,0),all=new Float32Array(total),o=0;chunks.forEach(c=>{all.set(c,o);o+=c.length});let ds=downmix(all,ctx.sampleRate,16000);await uploadBlob(wav(ds,16000));$('rec').textContent='Uploaded '+Math.round(ds.length/16)/1000+'s'}"
"async function uploadFile(){let f=$('file').files[0];if(!f){$('out').textContent='Select a WAV first';return}await uploadBlob(f)}"
"async function savePhrase(){let r=await fetch('/api/wake_word_config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'phrase='+encodeURIComponent($('phrase').value)});$('out').textContent=await r.text();load()}"
"async function load(){let r=await fetch('/api/wake_samples');let j=await r.json();$('phrase').value=j.target_phrase||'hi 小梦';let sc=j.sample_count>=0?j.sample_count+'/'+j.sample_target:'stored as wake_xm_XX.wav';$('rt').textContent='Active model: '+j.active_model+' | loaded '+j.wakenet_loaded+' | runtime wake word: '+j.runtime_wake_word+' | samples '+sc;let t='';(j.samples||[]).forEach(x=>t+=x.name+' '+(x.present?x.bytes+' bytes':'missing')+'\\n');$('out').textContent=t||j.note||'Ready'}load();</script></body></html>";

static const char WAKE_WORD_GUIDED_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Wake Word Capture</title><style>body{margin:0;background:#0b0f14;color:#e8edf2;font-family:Arial,'Microsoft YaHei',sans-serif}main{padding:12px;display:grid;gap:10px;max-width:820px;margin:auto}"
".card{border:1px solid #2b3742;background:#121a22;border-radius:8px;padding:12px;display:grid;gap:10px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}.step{display:flex;gap:10px;align-items:flex-start}.badge{flex:0 0 28px;height:28px;border-radius:50%;background:#2cc2a3;color:#04100d;display:grid;place-items:center;font-weight:700}.title{font-weight:700}.ok{color:#66e1b6}.warn{color:#ffc26b}.err{color:#ff9aa8}"
"input,button{height:40px;border-radius:6px;border:1px solid #3a4855;background:#081018;color:#eef3f6;padding:0 10px;font-size:14px;box-sizing:border-box;width:100%}button{background:#2cc2a3;color:#04100d;border:0;font-weight:700}.sec{background:#25313c;color:#e8edf2}.s{font-size:13px;color:#a8b5bf;line-height:1.5}pre{white-space:pre-wrap}a{color:#66c7ff}@media(max-width:640px){.grid{grid-template-columns:1fr}}</style></head>"
"<body><main><h2>&#21796;&#37266;&#35789;&#37319;&#38598;</h2><div class='s'><a href='/'>Dashboard</a> | <a href='/claw'>Claw Config</a> | <a href='/api/wake_samples'>Samples JSON</a></div>"
"<div class='card'><b>&#24403;&#21069;&#29366;&#24577;</b><div id='rt' class='s'>Loading...</div><div class='s warn'>&#24403;&#21069;&#30495;&#27491;&#36816;&#34892;&#30340;&#21796;&#37266;&#27169;&#22411;&#36824;&#26159; Hi ESP&#12290;&#26412;&#39029;&#38754;&#29992;&#20110;&#37319;&#38598; hi &#23567;&#26790; &#35757;&#32451;&#26679;&#26412;&#65292;&#19981;&#20250;&#31435;&#21051;&#25913;&#21464;&#22768;&#23398;&#21796;&#37266;&#35789;&#12290;</div></div>"
"<div class='card'><div class='step'><div class='badge'>1</div><div><div class='title'>&#30830;&#35748;&#30446;&#26631;&#21796;&#37266;&#35789;</div><div class='s'>&#20445;&#25345;&#40664;&#35748;&#30340; hi &#23567;&#26790;&#65292;&#28857;&#20987;&#20445;&#23384;&#12290;&#20445;&#23384;&#25104;&#21151;&#21518;&#20250;&#26174;&#31034;&#25552;&#31034;&#12290;</div></div></div><div class='grid'><input id='phrase' value='hi &#23567;&#26790;'><button onclick='savePhrase()'>&#20445;&#23384;&#30446;&#26631;&#35789;</button></div><div id='phraseStatus' class='s'>--</div></div>"
"<div class='card'><div class='step'><div class='badge'>2</div><div><div class='title'>&#24405;&#21046;&#19968;&#26465;&#26679;&#26412;</div><div class='s'>&#28857;&#20987;&#24320;&#22987;&#24405;&#38899;&#65292;&#21069;&#21518;&#30041;&#21322;&#31186;&#23433;&#38745;&#65292;&#33258;&#28982;&#35828;&#19968;&#27425;&#8220;hi &#23567;&#26790;&#8221;&#65292;1-3 &#31186;&#21518;&#28857;&#20987;&#20572;&#27490;&#24182;&#19978;&#20256;&#12290;</div></div></div><div class='grid'><button onclick='startRec()'>&#24320;&#22987;&#24405;&#38899;</button><button class='sec' onclick='stopRec()'>&#20572;&#27490;&#24182;&#19978;&#20256;</button></div><div id='rec' class='s'>Ready</div></div>"
"<div class='card'><div class='step'><div class='badge'>3</div><div><div class='title'>&#22914;&#26524;&#27983;&#35272;&#22120;&#19981;&#35753;&#24405;&#38899;</div><div class='s'>&#29992;&#25163;&#26426;&#25110;&#30005;&#33041;&#24405;&#22909; WAV &#21518;&#19978;&#20256;&#12290;&#25512;&#33616;&#65306;&#21333;&#22768;&#36947;&#12289;16kHz&#12289;16-bit PCM&#12289;1-3 &#31186;&#65292;&#25991;&#20214;&#19981;&#36229;&#36807; 512 KB&#12290;</div></div></div><div class='grid'><input id='file' type='file' accept='.wav,audio/wav'><button onclick='uploadFile()'>Upload WAV</button></div><div id='fileStatus' class='s'>--</div></div>"
"<div class='card'><b>&#32467;&#26524;&#21644;&#19979;&#19968;&#27493;</b><pre id='out' class='s'></pre><button class='sec' onclick='load()'>&#21047;&#26032;&#29366;&#24577;</button></div>"
"</main><script>const $=x=>document.getElementById(x);let ctx,stream,src,proc,chunks=[],recording=false,timer=0,started=0;"
"function msg(id,text,cls){let e=$(id);e.className='s '+(cls||'');e.textContent=text}function fmtBytes(n){return n>1024?Math.round(n/1024)+' KB':n+' B'}"
"function wav(samples,rate){let n=samples.length,b=new ArrayBuffer(44+n*2),v=new DataView(b),p=0;function s(x){for(let i=0;i<x.length;i++)v.setUint8(p++,x.charCodeAt(i))}function u32(x){v.setUint32(p,x,true);p+=4}function u16(x){v.setUint16(p,x,true);p+=2}s('RIFF');u32(36+n*2);s('WAVEfmt ');u32(16);u16(1);u16(1);u32(rate);u32(rate*2);u16(2);u16(16);s('data');u32(n*2);for(let i=0;i<n;i++){let x=Math.max(-1,Math.min(1,samples[i]));v.setInt16(p,x<0?x*32768:x*32767,true);p+=2}return new Blob([b],{type:'audio/wav'})}"
"function downmix(buf,inRate,outRate){let ratio=inRate/outRate,len=Math.floor(buf.length/ratio),out=new Float32Array(len);for(let i=0;i<len;i++){let a=Math.floor(i*ratio),b=Math.min(buf.length,Math.floor((i+1)*ratio)),sum=0,c=0;for(let j=a;j<b;j++){sum+=buf[j];c++}out[i]=c?sum/c:0}return out}"
"async function uploadBlob(blob,where){try{msg(where,'Uploading '+fmtBytes(blob.size)+' ...','warn');let r=await fetch('/api/wake_sample_upload',{method:'POST',headers:{'Content-Type':'audio/wav'},body:blob});let txt=await r.text();let j;try{j=JSON.parse(txt)}catch(e){}if(!r.ok){msg(where,'Upload failed: HTTP '+r.status+' '+txt,'err');$('out').textContent=txt;return false}if(j){let ok='Upload OK: '+j.name+' ('+fmtBytes(j.bytes)+') saved to '+j.path;msg(where,ok,'ok');$('out').textContent=ok+'\\n\\nNext: record another sample. Target is at least 20 samples.'}else{msg(where,'Upload OK','ok');$('out').textContent=txt}await load();return true}catch(e){msg(where,'Upload failed: '+e.message,'err');return false}}"
"async function startRec(){try{if(recording)return;if(!navigator.mediaDevices||!navigator.mediaDevices.getUserMedia){msg('rec','This browser does not allow microphone recording on this page. Use WAV upload below.','err');return}stream=await navigator.mediaDevices.getUserMedia({audio:true});ctx=new AudioContext();src=ctx.createMediaStreamSource(stream);proc=ctx.createScriptProcessor(4096,1,1);chunks=[];proc.onaudioprocess=e=>{if(recording)chunks.push(new Float32Array(e.inputBuffer.getChannelData(0)))};src.connect(proc);proc.connect(ctx.destination);recording=true;started=Date.now();timer=setInterval(()=>msg('rec','Recording '+((Date.now()-started)/1000).toFixed(1)+'s. Say: hi \\u5c0f\\u68a6, then click Stop & Upload.','warn'),200);msg('rec','Recording started. Say: hi \\u5c0f\\u68a6','warn')}catch(e){msg('rec','Mic unavailable: '+e.message+'. Use WAV upload below.','err')}}"
"async function stopRec(){if(!recording){msg('rec','Not recording. Click Start first.','warn');return}recording=false;clearInterval(timer);if(proc)proc.disconnect();if(src)src.disconnect();if(stream)stream.getTracks().forEach(t=>t.stop());let total=chunks.reduce((a,c)=>a+c.length,0);if(!total){msg('rec','No audio captured. Try again.','err');return}let all=new Float32Array(total),o=0;chunks.forEach(c=>{all.set(c,o);o+=c.length});let ds=downmix(all,ctx.sampleRate,16000),sec=ds.length/16000;if(sec<0.5)msg('rec','Sample is very short ('+sec.toFixed(1)+'s). Uploading anyway, but record 1-3s next time.','warn');await uploadBlob(wav(ds,16000),'rec')}"
"async function uploadFile(){let f=$('file').files[0];if(!f){msg('fileStatus','Select a WAV file first.','warn');return}if(f.size>512*1024){msg('fileStatus','File is over 512 KB. Please trim to 1-3 seconds.','err');return}await uploadBlob(f,'fileStatus')}"
"async function savePhrase(){try{msg('phraseStatus','Saving target phrase...','warn');let r=await fetch('/api/wake_word_config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'phrase='+encodeURIComponent($('phrase').value)});let txt=await r.text();let j;try{j=JSON.parse(txt)}catch(e){}if(!r.ok){msg('phraseStatus','Save failed: HTTP '+r.status,'err');$('out').textContent=txt;return}msg('phraseStatus','Saved: '+(j&&j.target_phrase?j.target_phrase:$('phrase').value)+'. Now record samples.','ok');$('out').textContent=txt;load()}catch(e){msg('phraseStatus','Save failed: '+e.message,'err')}}"
"async function load(){try{let r=await fetch('/api/wake_samples');let j=await r.json();$('phrase').value=j.target_phrase||'hi \\u5c0f\\u68a6';let sc=j.sample_count>=0?j.sample_count+'/'+j.sample_target:'stored as wake_xm_XX.wav';let last=j.last_sample_name?(' | last '+j.last_sample_name+' '+fmtBytes(j.last_sample_bytes||0)):' | no uploaded sample recorded';$('rt').textContent='Active model: '+j.active_model+' | loaded '+j.wakenet_loaded+' | runtime wake word: '+j.runtime_wake_word+' | samples '+sc+last;let t=(j.note||'Ready')+'\\n\\n';(j.samples||[]).forEach(x=>{t+=x.name+' '+(x.present?fmtBytes(x.bytes)+' '+x.download_url:'missing')+'\\n'});$('out').textContent=t}catch(e){$('out').textContent='Status load failed: '+e.message}}load();</script></body></html>";

static esp_err_t ensure_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t ensure_spiffs(void)
{
    size_t total = 0;
    size_t used = 0;
    esp_err_t info_err = esp_spiffs_info("storage", &total, &used);
    if (info_err == ESP_OK) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t load_wifi_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MOBILE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    memset(s_app.sta_profiles_ssid, 0, sizeof(s_app.sta_profiles_ssid));
    memset(s_app.sta_profiles_password, 0, sizeof(s_app.sta_profiles_password));
    s_app.sta_profile_count = 0;
    s_app.sta_profile_index = 0;
    s_app.sta_profiles_tried_in_cycle = 0;
    s_app.sta_ssid[0] = '\0';
    s_app.sta_password[0] = '\0';
    s_app.sta_has_credentials = false;

    uint8_t saved_count = 0;
    esp_err_t count_err = nvs_get_u8(nvs, MOBILE_NVS_KEY_PROFILE_COUNT, &saved_count);
    bool migrate_legacy_profiles = count_err == ESP_OK && saved_count > WIFI_PROFILE_MAX;
    if (count_err == ESP_OK && saved_count > WIFI_PROFILE_MAX) {
        saved_count = WIFI_PROFILE_MAX;
    }

    if (count_err == ESP_OK) {
        for (uint8_t i = 0; i < saved_count && s_app.sta_profile_count < WIFI_PROFILE_MAX; ++i) {
            char ssid_key[8];
            char pass_key[8];
            snprintf(ssid_key, sizeof(ssid_key), "ssid%u", (unsigned)i);
            snprintf(pass_key, sizeof(pass_key), "pass%u", (unsigned)i);

            char ssid[33] = {0};
            char pass[65] = {0};
            size_t ssid_len = sizeof(ssid);
            size_t pass_len = sizeof(pass);
            if (nvs_get_str(nvs, ssid_key, ssid, &ssid_len) == ESP_OK && ssid[0] != '\0') {
                if (nvs_get_str(nvs, pass_key, pass, &pass_len) != ESP_OK) {
                    pass[0] = '\0';
                }
                uint8_t dst = s_app.sta_profile_count++;
                snprintf(s_app.sta_profiles_ssid[dst], sizeof(s_app.sta_profiles_ssid[dst]), "%s", ssid);
                snprintf(s_app.sta_profiles_password[dst], sizeof(s_app.sta_profiles_password[dst]), "%s", pass);
            }
        }
    }

    if (s_app.sta_profile_count == 0) {
        size_t ssid_len = sizeof(s_app.sta_profiles_ssid[0]);
        size_t pass_len = sizeof(s_app.sta_profiles_password[0]);
        err = nvs_get_str(nvs, MOBILE_NVS_KEY_SSID, s_app.sta_profiles_ssid[0], &ssid_len);
        if (err == ESP_OK && s_app.sta_profiles_ssid[0][0] != '\0') {
            if (nvs_get_str(nvs, MOBILE_NVS_KEY_PASS, s_app.sta_profiles_password[0], &pass_len) != ESP_OK) {
                s_app.sta_profiles_password[0][0] = '\0';
            }
            s_app.sta_profile_count = 1;
        }
    } else {
        err = ESP_OK;
    }

    if (s_app.sta_profile_count > 0) {
        snprintf(s_app.sta_ssid, sizeof(s_app.sta_ssid), "%s", s_app.sta_profiles_ssid[0]);
        snprintf(s_app.sta_password, sizeof(s_app.sta_password), "%s", s_app.sta_profiles_password[0]);
        s_app.sta_has_credentials = s_app.sta_ssid[0] != '\0';
        ESP_LOGI(TAG, "Loaded %u saved Wi-Fi profile(s), current ssid=%s",
                 (unsigned)s_app.sta_profile_count, s_app.sta_ssid);
    }

    if (migrate_legacy_profiles) {
        esp_err_t migrate_err = nvs_set_u8(nvs, MOBILE_NVS_KEY_PROFILE_COUNT, 1);
        for (uint8_t i = 1; migrate_err == ESP_OK && i < WIFI_PROFILE_STORAGE_SLOTS; ++i) {
            char ssid_key[8];
            char pass_key[8];
            snprintf(ssid_key, sizeof(ssid_key), "ssid%u", (unsigned)i);
            snprintf(pass_key, sizeof(pass_key), "pass%u", (unsigned)i);
            migrate_err = nvs_set_str(nvs, ssid_key, "");
            if (migrate_err == ESP_OK) {
                migrate_err = nvs_set_str(nvs, pass_key, "");
            }
        }
        if (migrate_err == ESP_OK) {
            migrate_err = nvs_commit(nvs);
        }
        ESP_LOGI(TAG, "Legacy Wi-Fi profiles removed; single network=%s result=%s",
                 s_app.sta_ssid, esp_err_to_name(migrate_err));
    }

    nvs_close(nvs);
    return err;
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(MOBILE_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open wifi nvs");
    if (!ssid || ssid[0] == '\0') {
        nvs_close(nvs);
        return ESP_ERR_INVALID_ARG;
    }

    char old_ssid[WIFI_PROFILE_MAX][33] = {{0}};
    char old_pass[WIFI_PROFILE_MAX][65] = {{0}};
    uint8_t old_count = s_app.sta_profile_count;
    if (old_count > WIFI_PROFILE_MAX) {
        old_count = WIFI_PROFILE_MAX;
    }
    for (uint8_t i = 0; i < old_count; ++i) {
        snprintf(old_ssid[i], sizeof(old_ssid[i]), "%s", s_app.sta_profiles_ssid[i]);
        snprintf(old_pass[i], sizeof(old_pass[i]), "%s", s_app.sta_profiles_password[i]);
    }

    if (old_count == 0) {
        uint8_t saved_count = 0;
        if (nvs_get_u8(nvs, MOBILE_NVS_KEY_PROFILE_COUNT, &saved_count) == ESP_OK) {
            if (saved_count > WIFI_PROFILE_MAX) {
                saved_count = WIFI_PROFILE_MAX;
            }
            for (uint8_t i = 0; i < saved_count && old_count < WIFI_PROFILE_MAX; ++i) {
                char ssid_key[8];
                char pass_key[8];
                snprintf(ssid_key, sizeof(ssid_key), "ssid%u", (unsigned)i);
                snprintf(pass_key, sizeof(pass_key), "pass%u", (unsigned)i);
                size_t ssid_len = sizeof(old_ssid[old_count]);
                size_t pass_len = sizeof(old_pass[old_count]);
                if (nvs_get_str(nvs, ssid_key, old_ssid[old_count], &ssid_len) == ESP_OK &&
                    old_ssid[old_count][0] != '\0') {
                    if (nvs_get_str(nvs, pass_key, old_pass[old_count], &pass_len) != ESP_OK) {
                        old_pass[old_count][0] = '\0';
                    }
                    old_count++;
                }
            }
        }
    }

    if (old_count == 0) {
        size_t ssid_len = sizeof(old_ssid[0]);
        size_t pass_len = sizeof(old_pass[0]);
        if (nvs_get_str(nvs, MOBILE_NVS_KEY_SSID, old_ssid[0], &ssid_len) == ESP_OK &&
            old_ssid[0][0] != '\0') {
            if (nvs_get_str(nvs, MOBILE_NVS_KEY_PASS, old_pass[0], &pass_len) != ESP_OK) {
                old_pass[0][0] = '\0';
            }
            old_count = 1;
        }
    }

    char new_ssid[WIFI_PROFILE_MAX][33] = {{0}};
    char new_pass[WIFI_PROFILE_MAX][65] = {{0}};
    uint8_t new_count = 0;
    snprintf(new_ssid[new_count], sizeof(new_ssid[new_count]), "%s", ssid);
    snprintf(new_pass[new_count], sizeof(new_pass[new_count]), "%s", password ? password : "");
    new_count++;
    for (uint8_t i = 0; i < old_count && new_count < WIFI_PROFILE_MAX; ++i) {
        if (old_ssid[i][0] == '\0' || strcmp(old_ssid[i], ssid) == 0) {
            continue;
        }
        snprintf(new_ssid[new_count], sizeof(new_ssid[new_count]), "%s", old_ssid[i]);
        snprintf(new_pass[new_count], sizeof(new_pass[new_count]), "%s", old_pass[i]);
        new_count++;
    }

    esp_err_t err = nvs_set_u8(nvs, MOBILE_NVS_KEY_PROFILE_COUNT, new_count);
    /* Clear every legacy slot as part of the same committed update. */
    for (uint8_t i = 0; err == ESP_OK && i < WIFI_PROFILE_STORAGE_SLOTS; ++i) {
        char ssid_key[8];
        char pass_key[8];
        snprintf(ssid_key, sizeof(ssid_key), "ssid%u", (unsigned)i);
        snprintf(pass_key, sizeof(pass_key), "pass%u", (unsigned)i);
        err = nvs_set_str(nvs, ssid_key, (i < new_count) ? new_ssid[i] : "");
        if (err == ESP_OK) {
            err = nvs_set_str(nvs, pass_key, (i < new_count) ? new_pass[i] : "");
        }
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, MOBILE_NVS_KEY_SSID, new_ssid[0]);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, MOBILE_NVS_KEY_PASS, new_pass[0]);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        memset(s_app.sta_profiles_ssid, 0, sizeof(s_app.sta_profiles_ssid));
        memset(s_app.sta_profiles_password, 0, sizeof(s_app.sta_profiles_password));
        for (uint8_t i = 0; i < new_count; ++i) {
            snprintf(s_app.sta_profiles_ssid[i], sizeof(s_app.sta_profiles_ssid[i]), "%s", new_ssid[i]);
            snprintf(s_app.sta_profiles_password[i], sizeof(s_app.sta_profiles_password[i]), "%s", new_pass[i]);
        }
        s_app.sta_profile_count = new_count;
        s_app.sta_profile_index = 0;
        s_app.sta_profiles_tried_in_cycle = 0;
        ESP_LOGI(TAG, "Saved %u Wi-Fi profile(s), newest ssid=%s", (unsigned)new_count, new_ssid[0]);
    }
    return err;
}

void mobile_app_get_default_config(mobile_app_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->ssid = SETUP_AP_DEFAULT_SSID;
    config->password = SETUP_AP_DEFAULT_PASSWORD;
    config->channel = SETUP_AP_DEFAULT_CHANNEL;
    config->hostname = "dreamguardian";
    config->http_enabled = true;
}

static void add_bool(cJSON *obj, const char *name, bool value)
{
    cJSON_AddBoolToObject(obj, name, value);
}

static void add_number_safe(cJSON *obj, const char *name, double value)
{
    if (value == value && value > -1.0e100 && value < 1.0e100) {
        cJSON_AddNumberToObject(obj, name, value);
    } else {
        cJSON_AddNullToObject(obj, name);
    }
}

static void prepare_http_response(httpd_req_t *req, const char *type)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static void close_http_session(httpd_req_t *req)
{
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) {
        (void)httpd_sess_trigger_close(req->handle, sockfd);
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc failed");
    }
    prepare_http_response(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return err;
}

static esp_err_t send_html_chunked(httpd_req_t *req, const char *html)
{
    prepare_http_response(req, "text/html; charset=utf-8");
    size_t len = strlen(html);
    char *body = malloc(len);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "html alloc failed");
    }
    memcpy(body, html, len);
    esp_err_t err = httpd_resp_send(req, body, (ssize_t)len);
    free(body);
    return err;
}

static esp_err_t send_html_static(httpd_req_t *req, const char *html)
{
    return send_html_chunked(req, html);
}

static const char *assist_state_to_text(sleep_assist_state_t state)
{
    return sleep_assist_state_name(state);
}

static void add_wifi_json(cJSON *root)
{
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    add_bool(wifi, "connected", s_app.sta_connected);
    cJSON_AddStringToObject(wifi, "ip", s_app.sta_ip);
    cJSON_AddStringToObject(wifi, "ssid", s_app.sta_ssid);
    cJSON_AddNumberToObject(wifi, "saved_count", s_app.sta_profile_count);
    cJSON_AddNumberToObject(wifi, "profile_index", s_app.sta_profile_index);
    cJSON_AddStringToObject(wifi, "setup_ssid", s_app.cfg.ssid ? s_app.cfg.ssid : SETUP_AP_DEFAULT_SSID);
}

static void add_time_json(cJSON *root)
{
    cJSON *time_obj = cJSON_AddObjectToObject(root, "time");
    struct tm timeinfo = {0};
    bool valid = time_sync_get_local_time(&timeinfo, NULL);
    add_bool(time_obj, "valid", valid);
    if (valid) {
        char date_text[32];
        char time_text[32];
        snprintf(date_text, sizeof(date_text), "%04d-%02d-%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        snprintf(time_text, sizeof(time_text), "%02d:%02d:%02d",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        cJSON_AddStringToObject(time_obj, "date", date_text);
        cJSON_AddStringToObject(time_obj, "clock", time_text);
        cJSON_AddStringToObject(time_obj, "weekday", time_sync_weekday_name(timeinfo.tm_wday));
    } else {
        cJSON_AddStringToObject(time_obj, "date", "");
        cJSON_AddStringToObject(time_obj, "clock", "");
        cJSON_AddStringToObject(time_obj, "weekday", "---");
    }
}

static void add_ai_bridge_json(cJSON *root)
{
    ai_bridge_config_t cfg = {0};
    ai_bridge_status_t status = {0};
    (void)ai_bridge_get_config(&cfg);
    ai_bridge_get_status(&status);

    cJSON *ai = cJSON_AddObjectToObject(root, "ai_bridge");
    add_bool(ai, "enabled", status.enabled);
    add_bool(ai, "configured", status.configured);
    add_bool(ai, "worker_running", status.worker_running);
    cJSON_AddNumberToObject(ai, "min_interval_sec", cfg.min_interval_sec);
    cJSON_AddNumberToObject(ai, "queued", status.queued_count);
    cJSON_AddNumberToObject(ai, "sent", status.sent_count);
    cJSON_AddNumberToObject(ai, "dropped", status.dropped_count);
    cJSON_AddNumberToObject(ai, "last_http_status", status.last_http_status);
    cJSON_AddStringToObject(ai, "last_result", status.last_result);
}

static void add_feishu_agent_json(cJSON *root)
{
    feishu_agent_status_t status = {0};
    feishu_agent_get_status(&status);

    cJSON *feishu = cJSON_AddObjectToObject(root, "feishu_agent");
    add_bool(feishu, "running", status.running);
    add_bool(feishu, "configured", status.configured);
    add_bool(feishu, "ws_connected", status.ws_connected);
    add_bool(feishu, "ws_ever_connected", status.ws_ever_connected);
    cJSON_AddNumberToObject(feishu, "received", status.received_count);
    cJSON_AddNumberToObject(feishu, "commands", status.command_count);
    cJSON_AddNumberToObject(feishu, "replies", status.reply_count);
    cJSON_AddNumberToObject(feishu, "last_http_status", status.last_http_status);
    cJSON_AddStringToObject(feishu, "last_result", status.last_result);
}

static void add_sleep_preferences_json(cJSON *root)
{
    char scene[16] = "ppm";
    char breath[16] = "relax";
    int32_t counts[6] = {0};
    const char *names[] = {"ppm", "ocean", "forest", "rain", "zen", "empty"};
    nvs_handle_t nvs;
    if (nvs_open(PREF_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(scene);
        (void)nvs_get_str(nvs, PREF_NVS_SCENE, scene, &len);
        len = sizeof(breath);
        (void)nvs_get_str(nvs, PREF_NVS_BREATH, breath, &len);
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            char key[16];
            snprintf(key, sizeof(key), "%s%s", PREF_COUNT_PREFIX, names[i]);
            (void)nvs_get_i32(nvs, key, &counts[i]);
        }
        nvs_close(nvs);
    }

    cJSON *prefs = cJSON_AddObjectToObject(root, "sleep_preferences");
    cJSON_AddStringToObject(prefs, "scene", scene);
    cJSON_AddStringToObject(prefs, "breath_mode", breath);
    cJSON *usage = cJSON_AddObjectToObject(prefs, "usage");
    int32_t best_count = -1;
    const char *best = "ppm";
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        cJSON_AddNumberToObject(usage, names[i], counts[i]);
        if (counts[i] > best_count) {
            best_count = counts[i];
            best = names[i];
        }
    }
    cJSON_AddStringToObject(prefs, "adaptive_default", best);
}

static void ip_to_text(const esp_ip_addr_t *ip, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!ip || ip->type != ESP_IPADDR_TYPE_V4) {
        return;
    }
    snprintf(out, out_size, IPSTR, IP2STR(&ip->u_addr.ip4));
}

static bool parse_url_host_port(const char *url, char *host, size_t host_size, uint16_t *port)
{
    if (!url || !host || host_size == 0 || !port) {
        return false;
    }
    const char *p = strstr(url, "://");
    bool https = false;
    if (p) {
        https = strncmp(url, "https://", 8) == 0;
        p += 3;
    } else {
        p = url;
    }
    const char *end = strchr(p, '/');
    if (!end) {
        end = p + strlen(p);
    }
    const char *colon = memchr(p, ':', (size_t)(end - p));
    size_t len = (size_t)((colon ? colon : end) - p);
    if (len == 0 || len >= host_size) {
        return false;
    }
    memcpy(host, p, len);
    host[len] = '\0';
    *port = https ? 443 : 80;
    if (colon && colon + 1 < end) {
        unsigned long parsed = strtoul(colon + 1, NULL, 10);
        if (parsed > 0 && parsed <= 65535) {
            *port = (uint16_t)parsed;
        }
    }
    return true;
}

static int tcp_connect_probe(const char *ip, uint16_t port, int timeout_ms, int *out_errno)
{
    if (out_errno) {
        *out_errno = 0;
    }
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        if (out_errno) *out_errno = errno;
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        if (out_errno) *out_errno = errno;
        close(sock);
        return -1;
    }

    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        close(sock);
        return 0;
    }
    if (errno != EINPROGRESS) {
        if (out_errno) *out_errno = errno;
        close(sock);
        return -1;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    rc = select(sock + 1, NULL, &wfds, NULL, &tv);
    if (rc <= 0) {
        if (out_errno) *out_errno = rc == 0 ? ETIMEDOUT : errno;
        close(sock);
        return -1;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
        if (out_errno) *out_errno = errno;
        close(sock);
        return -1;
    }
    close(sock);
    if (so_error != 0) {
        if (out_errno) *out_errno = so_error;
        return -1;
    }
    return 0;
}

static void add_host_probe_json(cJSON *root, const char *name, const char *host, uint16_t port)
{
    cJSON *obj = cJSON_AddObjectToObject(root, name);
    cJSON_AddStringToObject(obj, "host", host ? host : "");
    cJSON_AddNumberToObject(obj, "port", port);
    if (!host || !host[0]) {
        cJSON_AddStringToObject(obj, "dns_result", "missing_host");
        return;
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, NULL, &hints, &res);
    cJSON_AddNumberToObject(obj, "dns_err", gai);
    if (gai != 0 || !res) {
        cJSON_AddStringToObject(obj, "dns_result", "failed");
        return;
    }

    char ip[INET_ADDRSTRLEN] = {0};
    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
    freeaddrinfo(res);
    cJSON_AddStringToObject(obj, "ip", ip);

    int sock_errno = 0;
    int tcp = tcp_connect_probe(ip, port, 5000, &sock_errno);
    cJSON_AddNumberToObject(obj, "tcp_result", tcp);
    cJSON_AddNumberToObject(obj, "tcp_errno", sock_errno);
}

static void add_https_probe_json(cJSON *root, const char *name, const char *url)
{
    cJSON *obj = cJSON_AddObjectToObject(root, name);
    cJSON_AddStringToObject(obj, "url_kind", url && strstr(url, "open.feishu.cn") ? "feishu" : "configured");
    if (!url || !url[0]) {
        cJSON_AddStringToObject(obj, "result", "missing_url");
        return;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .addr_type = HTTP_ADDR_TYPE_INET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        cJSON_AddStringToObject(obj, "result", "client_alloc_failed");
        return;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int tls_error = 0;
    int tls_flags = 0;
    (void)esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
    esp_http_client_cleanup(client);

    cJSON_AddNumberToObject(obj, "err", err);
    cJSON_AddStringToObject(obj, "name", esp_err_to_name(err));
    cJSON_AddNumberToObject(obj, "status", status);
    cJSON_AddNumberToObject(obj, "tls_error", tls_error);
    cJSON_AddNumberToObject(obj, "tls_flags", tls_flags);
}

static esp_err_t net_diag_handler(httpd_req_t *req)
{
    char query[32] = {0};
    char deep_text[8] = {0};
    bool deep_probe = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "deep", deep_text, sizeof(deep_text)) == ESP_OK) {
        deep_probe = deep_text[0] == '1' || strcasecmp(deep_text, "true") == 0 ||
                     strcasecmp(deep_text, "on") == 0;
    }

    cJSON *root = cJSON_CreateObject();
    add_wifi_json(root);
    add_time_json(root);

    cJSON *dns = cJSON_AddObjectToObject(root, "dns_servers");
    if (s_app.sta_netif) {
        esp_netif_dns_info_t info = {0};
        char ip[32] = {0};
        if (esp_netif_get_dns_info(s_app.sta_netif, ESP_NETIF_DNS_MAIN, &info) == ESP_OK) {
            ip_to_text(&info.ip, ip, sizeof(ip));
            cJSON_AddStringToObject(dns, "main", ip);
        }
        if (esp_netif_get_dns_info(s_app.sta_netif, ESP_NETIF_DNS_BACKUP, &info) == ESP_OK) {
            ip_to_text(&info.ip, ip, sizeof(ip));
            cJSON_AddStringToObject(dns, "backup", ip);
        }
    }

    cJSON_AddBoolToObject(root, "deep_probe", deep_probe);
    if (deep_probe) {
        add_host_probe_json(root, "open_feishu_tcp", "open.feishu.cn", 443);
        add_https_probe_json(root, "open_feishu_https", "https://open.feishu.cn/open-apis");
    } else {
        cJSON_AddStringToObject(root, "open_feishu_probe", "skipped; use /api/net_diag?deep=1");
    }

    ai_bridge_config_t cfg = {0};
    (void)ai_bridge_get_config(&cfg);
    char webhook_host[128] = {0};
    uint16_t webhook_port = 0;
    if (parse_url_host_port(cfg.feishu_webhook_url, webhook_host, sizeof(webhook_host), &webhook_port)) {
        if (deep_probe) {
            add_host_probe_json(root, "webhook_tcp", webhook_host, webhook_port);
            add_https_probe_json(root, "webhook_https_get", cfg.feishu_webhook_url);
        } else {
            cJSON_AddStringToObject(root, "webhook", "configured; deep probe skipped");
        }
    } else {
        cJSON_AddStringToObject(root, "webhook", cfg.feishu_webhook_url[0] ? "parse_failed" : "not_configured");
    }

    return send_json(req, root);
}

static esp_err_t feishu_min_handler(httpd_req_t *req)
{
    feishu_agent_status_t status = {0};
    feishu_agent_get_status(&status);
    cJSON *root = cJSON_CreateObject();
    add_bool(root, "running", status.running);
    add_bool(root, "configured", status.configured);
    add_bool(root, "ws_connected", status.ws_connected);
    add_bool(root, "ws_ever_connected", status.ws_ever_connected);
    cJSON_AddNumberToObject(root, "ws_data", status.ws_data_count);
    cJSON_AddNumberToObject(root, "ws_binary", status.ws_binary_count);
    cJSON_AddNumberToObject(root, "ws_text", status.ws_text_count);
    cJSON_AddNumberToObject(root, "last_ws_opcode", status.last_ws_opcode);
    cJSON_AddNumberToObject(root, "last_ws_data_len", status.last_ws_data_len);
    cJSON_AddNumberToObject(root, "last_ws_payload_len", status.last_ws_payload_len);
    cJSON_AddNumberToObject(root, "frames", status.frame_count);
    cJSON_AddNumberToObject(root, "events", status.event_count);
    cJSON_AddNumberToObject(root, "message_events", status.message_event_count);
    cJSON_AddNumberToObject(root, "parse_fails", status.parse_fail_count);
    cJSON_AddNumberToObject(root, "received", status.received_count);
    cJSON_AddNumberToObject(root, "commands", status.command_count);
    cJSON_AddNumberToObject(root, "replies", status.reply_count);
    cJSON_AddNumberToObject(root, "last_http_status", status.last_http_status);
    cJSON_AddStringToObject(root, "last_event_type", status.last_event_type);
    cJSON_AddStringToObject(root, "last_result", status.last_result);
    return send_json(req, root);
}

static esp_err_t feishu_restart_handler(httpd_req_t *req)
{
    esp_err_t err = feishu_agent_restart();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "result", err);
    cJSON_AddStringToObject(root, "name", esp_err_to_name(err));
    return send_json(req, root);
}

static esp_err_t light_diag_handler(httpd_req_t *req)
{
    status_light_diag_t diag = {0};
    status_light_get_diag(&diag);
    cJSON *root = cJSON_CreateObject();
    add_bool(root, "ready", diag.ready);
    add_bool(root, "ws2812_enabled", diag.ws2812_enabled);
    cJSON_AddNumberToObject(root, "ws2812_gpio", diag.ws2812_gpio);
    cJSON_AddNumberToObject(root, "ws2812_count", (double)diag.ws2812_count);
    cJSON_AddNumberToObject(root, "ws2812_tx_count", diag.ws2812_tx_count);
    cJSON_AddNumberToObject(root, "ws2812_last_error", diag.ws2812_last_error);
    cJSON_AddStringToObject(root, "ws2812_last_error_name", esp_err_to_name(diag.ws2812_last_error));
    cJSON_AddNumberToObject(root, "last_red", diag.last_red);
    cJSON_AddNumberToObject(root, "last_green", diag.last_green);
    cJSON_AddNumberToObject(root, "last_blue", diag.last_blue);
    return send_json(req, root);
}

static double average_or_zero(double sum, uint32_t count)
{
    return count > 0 ? sum / (double)count : 0.0;
}

static void sleep_profile_update_locked(const mobile_app_telemetry_t *t, int64_t now_us)
{
    sleep_profile_state_t *p = &s_app.profile;
    if (p->samples == 0) {
        p->start_us = now_us;
    }
    p->last_us = now_us;
    p->samples++;
    if (t->radar.human_present || t->bio.presence) {
        p->present_samples++;
    }
    if (t->assist.active) {
        p->assist_active_samples++;
    }
    if (t->assist.sleep_locked) {
        p->sleep_locked_samples++;
    }
    if (t->assessment.wake_risk >= 70) {
        p->high_risk_samples++;
    }
    if (t->assessment.wake_risk > p->max_wake_risk) {
        p->max_wake_risk = t->assessment.wake_risk;
    }

    p->score_sum += t->assessment.sleep_score;
    p->risk_sum += t->assessment.wake_risk;

    if (t->bio.valid) {
        float breath = t->bio.breath_bpm_smooth;
        float heart = t->bio.heart_bpm_smooth;
        p->valid_samples++;
        if (isfinite(breath)) {
            p->breath_sum += breath;
            if (p->valid_samples == 1 || breath < p->breath_min) {
                p->breath_min = breath;
            }
            if (p->valid_samples == 1 || breath > p->breath_max) {
                p->breath_max = breath;
            }
        }
        if (isfinite(heart)) {
            p->heart_sum += heart;
            if (p->valid_samples == 1 || heart < p->heart_min) {
                p->heart_min = heart;
            }
            if (p->valid_samples == 1 || heart > p->heart_max) {
                p->heart_max = heart;
            }
        }
        if (isfinite(t->bio.motion_smooth)) {
            p->motion_sum += t->bio.motion_smooth;
        }
        if (isfinite(t->bio.stability_score)) {
            p->stability_sum += t->bio.stability_score;
        }
    }
}

static void sleep_profile_recommendation(const sleep_profile_state_t *p, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!p || p->samples < 30) {
        snprintf(out, out_size, "%s", "Collecting baseline. Keep the device facing the sleeper for several minutes.");
        return;
    }
    double valid_ratio = (double)p->valid_samples / (double)p->samples;
    double avg_score = average_or_zero(p->score_sum, p->samples);
    double avg_risk = average_or_zero(p->risk_sum, p->samples);
    double avg_stability = average_or_zero(p->stability_sum, p->valid_samples);
    double avg_motion = average_or_zero(p->motion_sum, p->valid_samples);

    if (valid_ratio < 0.35) {
        snprintf(out, out_size, "%s", "Signal quality is low. Adjust radar angle and distance before trusting long-term advice.");
    } else if (avg_risk >= 55.0 || p->max_wake_risk >= 80) {
        snprintf(out, out_size, "%s", "Wake risk is high. Reduce light/audio stimulation and run a longer settle phase tonight.");
    } else if (avg_stability < 0.55 || avg_motion > 0.45) {
        snprintf(out, out_size, "%s", "Sleep is unstable. Prefer slower breathing guidance and avoid aggressive intervention changes.");
    } else if (avg_score >= 75.0 && avg_risk <= 30.0) {
        snprintf(out, out_size, "%s", "Current plan is working. Keep the same intervention profile and compare tomorrow's baseline.");
    } else {
        snprintf(out, out_size, "%s", "Use baseline breathing, then gradually lower target breath rate while monitoring wake risk.");
    }
}

static void add_sleep_profile_json(cJSON *root, const sleep_profile_state_t *p)
{
    cJSON *profile = cJSON_AddObjectToObject(root, "profile");
    if (!p || p->samples == 0) {
        cJSON_AddNumberToObject(profile, "samples", 0);
        cJSON_AddStringToObject(profile, "recommendation", "Collecting baseline.");
        return;
    }

    uint32_t duration_sec = 0;
    if (p->last_us > p->start_us) {
        duration_sec = (uint32_t)((p->last_us - p->start_us) / 1000000LL);
    }
    cJSON_AddNumberToObject(profile, "samples", p->samples);
    cJSON_AddNumberToObject(profile, "duration_sec", duration_sec);
    add_number_safe(profile, "presence_ratio", average_or_zero(p->present_samples, p->samples));
    add_number_safe(profile, "valid_ratio", average_or_zero(p->valid_samples, p->samples));
    add_number_safe(profile, "assist_ratio", average_or_zero(p->assist_active_samples, p->samples));
    add_number_safe(profile, "sleep_locked_ratio", average_or_zero(p->sleep_locked_samples, p->samples));
    cJSON_AddNumberToObject(profile, "high_risk_samples", p->high_risk_samples);
    cJSON_AddNumberToObject(profile, "max_wake_risk", p->max_wake_risk);

    cJSON *avg = cJSON_AddObjectToObject(profile, "averages");
    add_number_safe(avg, "breath_bpm", average_or_zero(p->breath_sum, p->valid_samples));
    add_number_safe(avg, "heart_bpm", average_or_zero(p->heart_sum, p->valid_samples));
    add_number_safe(avg, "motion", average_or_zero(p->motion_sum, p->valid_samples));
    add_number_safe(avg, "stability", average_or_zero(p->stability_sum, p->valid_samples));
    add_number_safe(avg, "sleep_score", average_or_zero(p->score_sum, p->samples));
    add_number_safe(avg, "wake_risk", average_or_zero(p->risk_sum, p->samples));

    cJSON *range = cJSON_AddObjectToObject(profile, "ranges");
    add_number_safe(range, "breath_min", p->valid_samples ? p->breath_min : 0.0);
    add_number_safe(range, "breath_max", p->valid_samples ? p->breath_max : 0.0);
    add_number_safe(range, "heart_min", p->valid_samples ? p->heart_min : 0.0);
    add_number_safe(range, "heart_max", p->valid_samples ? p->heart_max : 0.0);

    char recommendation[192];
    sleep_profile_recommendation(p, recommendation, sizeof(recommendation));
    cJSON_AddStringToObject(profile, "recommendation", recommendation);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    mobile_app_telemetry_t t;
    sleep_profile_state_t profile;
    portENTER_CRITICAL(&s_app.lock);
    t = s_app.telemetry;
    profile = s_app.profile;
    portEXIT_CRITICAL(&s_app.lock);

    cJSON *root = cJSON_CreateObject();
    add_number_safe(root, "uptime_sec", t.uptime_sec);
    add_bool(root, "presence", t.radar.human_present);
    add_number_safe(root, "range_cm", t.radar.range_cm);
    add_wifi_json(root);
    add_time_json(root);
    add_ai_bridge_json(root);
    add_feishu_agent_json(root);
    add_sleep_preferences_json(root);

    cJSON *bio = cJSON_AddObjectToObject(root, "bio");
    add_bool(bio, "valid", t.bio.valid);
    add_number_safe(bio, "breath_bpm", t.bio.breath_bpm_smooth);
    add_number_safe(bio, "heart_bpm", t.bio.heart_bpm_smooth);
    add_number_safe(bio, "motion", t.bio.motion_smooth);
    add_number_safe(bio, "stability", t.bio.stability_score);

    cJSON *sleep = cJSON_AddObjectToObject(root, "sleep");
    cJSON_AddStringToObject(sleep, "state", sleep_state_to_name(t.assessment.state));
    add_number_safe(sleep, "score", t.assessment.sleep_score);
    add_number_safe(sleep, "wake_risk", t.assessment.wake_risk);
    add_number_safe(sleep, "stable_index", t.assessment.stable_sleep_index);
    cJSON_AddStringToObject(sleep, "reason", t.assessment.reason);

    sleep_ui_status_t ui_status;
    sleep_ui_get_status(&ui_status);
    cJSON *ui = cJSON_AddObjectToObject(root, "ui");
    add_bool(ui, "display_ready", ui_status.display_ready);
    add_bool(ui, "touch_ready", ui_status.touch_ready);
    add_bool(ui, "touch_indev_ready", ui_status.touch_indev_ready);
    add_number_safe(ui, "last_error", ui_status.last_error);
    add_number_safe(ui, "press_count", ui_status.press_count);
    add_number_safe(ui, "command_count", ui_status.command_count);
    add_number_safe(ui, "last_x", ui_status.last_x);
    add_number_safe(ui, "last_y", ui_status.last_y);
    add_number_safe(ui, "last_command", ui_status.last_command);
    cJSON_AddStringToObject(ui, "last_target", ui_status.last_target);

    cJSON *assist = cJSON_AddObjectToObject(root, "assist");
    add_bool(assist, "active", t.assist.active);
    add_bool(assist, "sleep_locked", t.assist.sleep_locked);
    cJSON_AddStringToObject(assist, "state", assist_state_to_text(t.assist.state));
    add_number_safe(assist, "target_breath", t.assist.target_breath_bpm);
    add_number_safe(assist, "audio", t.assist.audio_volume);
    add_number_safe(assist, "led", t.assist.led_brightness);

    cJSON *csi = cJSON_AddObjectToObject(root, "csi");
    add_bool(csi, "enabled", t.csi.enabled);
    add_bool(csi, "breath_valid", t.csi.breath_proxy_valid);
    add_number_safe(csi, "packets", t.csi.packet_count);
    add_number_safe(csi, "rssi", t.csi.last_rssi);
    add_number_safe(csi, "amplitude", t.csi.amplitude_ema);
    add_number_safe(csi, "motion", t.csi.motion_index);
    add_number_safe(csi, "stability", t.csi.stability_score);
    add_number_safe(csi, "breath_bpm", t.csi.breath_proxy_bpm);

    audio_monitor_status_t mic_status;
    audio_monitor_get_status(&mic_status);
    cJSON *mic = cJSON_AddObjectToObject(root, "mic");
    add_bool(mic, "enabled", mic_status.enabled);
    add_bool(mic, "running", mic_status.running);
    add_bool(mic, "initialized", mic_status.initialized);
    add_number_safe(mic, "last_error", mic_status.last_error);
    add_number_safe(mic, "sample_rate_hz", mic_status.sample_rate_hz);
    add_number_safe(mic, "frame_ms", mic_status.frame_ms);
    add_number_safe(mic, "frames", mic_status.frames);
    add_number_safe(mic, "read_errors", mic_status.read_errors);
    add_number_safe(mic, "level_dbfs", mic_status.level_dbfs);
    add_number_safe(mic, "noise_floor_dbfs", mic_status.noise_floor_dbfs);
    add_number_safe(mic, "above_floor_db", mic_status.level_dbfs - mic_status.noise_floor_dbfs);
    add_number_safe(mic, "instant_above_floor_db", mic_status.instant_above_floor_db);
    add_number_safe(mic, "rms", mic_status.rms);
    add_number_safe(mic, "peak", mic_status.peak);
    add_number_safe(mic, "phrase_ms", mic_status.phrase_ms);
    add_number_safe(mic, "phrase_peak", mic_status.phrase_peak);
    add_number_safe(mic, "phrase_above_floor_db", mic_status.phrase_above_floor_db);
    add_number_safe(mic, "environment_score", mic_status.environment_score);
    add_number_safe(mic, "volume_gain", mic_status.volume_gain);
    add_bool(mic, "noise_high", mic_status.noise_high);
    add_bool(mic, "voice_activity", mic_status.voice_activity);
    add_bool(mic, "wake_word_configured", mic_status.wake_word_configured);
    add_bool(mic, "wake_listening", mic_status.wake_listening);
    add_number_safe(mic, "wake_events", mic_status.wake_events);
    add_bool(mic, "snore_active", mic_status.snore_active);
    add_number_safe(mic, "snore_events", mic_status.snore_events);
    add_number_safe(mic, "snore_rejected_events", mic_status.snore_rejected_events);
    add_number_safe(mic, "snore_last_duration_ms", mic_status.snore_last_duration_ms);
    add_number_safe(mic, "snore_last_peak_dbfs", mic_status.snore_last_peak_dbfs);
    add_number_safe(mic, "snore_last_peak_above_floor_db",
                    mic_status.snore_last_peak_above_floor_db);
    add_number_safe(mic, "snore_last_zcr", mic_status.snore_last_zcr);
    add_number_safe(mic, "apnea_candidates", mic_status.apnea_candidates);
    add_number_safe(mic, "quiet_seconds", mic_status.quiet_seconds);

    voice_sr_status_t sr_status;
    voice_sr_get_status(&sr_status);
    cJSON *sr = cJSON_AddObjectToObject(root, "voice_sr");
    add_bool(sr, "enabled", sr_status.enabled);
    add_bool(sr, "running", sr_status.running);
    add_bool(sr, "initialized", sr_status.initialized);
    add_bool(sr, "models_loaded", sr_status.models_loaded);
    add_bool(sr, "wakenet_loaded", sr_status.wakenet_loaded);
    add_bool(sr, "afe_created", sr_status.afe_created);
    add_bool(sr, "feed_task_running", sr_status.feed_task_running);
    add_bool(sr, "detect_task_running", sr_status.detect_task_running);
    add_bool(sr, "template_experiment_enabled", sr_status.template_experiment_enabled);
    add_bool(sr, "assistant_suppressed", sr_status.assistant_suppressed);
    add_bool(sr, "template_learning", sr_status.template_learning);
    add_number_safe(sr, "last_error", sr_status.last_error);
    add_number_safe(sr, "wake_events", sr_status.wake_events);
    add_number_safe(sr, "template_wake_events", sr_status.template_wake_events);
    add_number_safe(sr, "command_events", sr_status.command_events);
    add_number_safe(sr, "last_command_id", sr_status.last_command_id);
    add_number_safe(sr, "template_count", sr_status.template_count);
    add_number_safe(sr, "template_checks", sr_status.template_checks);
    add_number_safe(sr, "template_learn_count", sr_status.template_learn_count);
    add_number_safe(sr, "template_learn_target", sr_status.template_learn_target);
    add_number_safe(sr, "feed_frames", sr_status.feed_frames);
    add_number_safe(sr, "fetch_frames", sr_status.fetch_frames);
    add_number_safe(sr, "read_errors", sr_status.read_errors);
    add_number_safe(sr, "mic_peak_ch0", sr_status.mic_peak_ch0);
    add_number_safe(sr, "mic_peak_ch1", sr_status.mic_peak_ch1);
    add_number_safe(sr, "i2s_overflow_count", sr_status.i2s_overflow_count);
    add_number_safe(sr, "audio_drop_count", sr_status.audio_drop_count);
    add_number_safe(sr, "feed_interval_max_us", sr_status.feed_interval_max_us);
    add_number_safe(sr, "fetch_interval_max_us", sr_status.fetch_interval_max_us);
    add_number_safe(sr, "feed_process_max_us", sr_status.feed_process_max_us);
    add_number_safe(sr, "fetch_process_max_us", sr_status.fetch_process_max_us);
    add_number_safe(sr, "inference_time_max_us", sr_status.inference_time_max_us);
    add_number_safe(sr, "feed_chunk", sr_status.feed_chunk);
    add_number_safe(sr, "fetch_chunk", sr_status.fetch_chunk);
    add_number_safe(sr, "template_best_score", sr_status.template_best_score);
    add_number_safe(sr, "template_threshold", sr_status.template_threshold);
    add_number_safe(sr, "template_confirm_threshold", sr_status.template_confirm_threshold);
    add_number_safe(sr, "template_min_confirmations", sr_status.template_min_confirmations);
    cJSON_AddStringToObject(sr, "wake_model", sr_status.wake_model);
    cJSON_AddStringToObject(sr, "command_model", sr_status.command_model);
    cJSON_AddStringToObject(sr, "template_best_name", sr_status.template_best_name);
    cJSON_AddStringToObject(sr, "template_source", sr_status.template_source);
    add_bool(sr, "debug_capture_enabled", sr_status.debug_capture_enabled);
    add_bool(sr, "debug_capture_active", sr_status.debug_capture_active);
    add_bool(sr, "debug_capture_write_pending", sr_status.debug_capture_write_pending);
    add_number_safe(sr, "debug_capture_seconds", sr_status.debug_capture_seconds);
    add_number_safe(sr, "debug_capture_raw_samples", sr_status.debug_capture_raw_samples);
    add_number_safe(sr, "debug_capture_afe_samples", sr_status.debug_capture_afe_samples);
    add_number_safe(sr, "debug_capture_score_frames", sr_status.debug_capture_score_frames);
    add_number_safe(sr, "debug_capture_raw_drops", sr_status.debug_capture_raw_drops);
    add_number_safe(sr, "debug_capture_afe_drops", sr_status.debug_capture_afe_drops);
    add_number_safe(sr, "debug_capture_score_drops", sr_status.debug_capture_score_drops);
    cJSON_AddStringToObject(sr, "debug_capture_label", sr_status.debug_capture_label);
    cJSON_AddStringToObject(sr, "debug_capture_raw_path", sr_status.debug_capture_raw_path);
    cJSON_AddStringToObject(sr, "debug_capture_afe_path", sr_status.debug_capture_afe_path);
    cJSON_AddStringToObject(sr, "debug_capture_score_path", sr_status.debug_capture_score_path);
    add_bool(sr, "command_audio_active", sr_status.command_audio_active);
    add_bool(sr, "command_audio_ready", sr_status.command_audio_ready);
    add_number_safe(sr, "command_audio_seconds", sr_status.command_audio_seconds);
    add_number_safe(sr, "command_audio_samples", sr_status.command_audio_samples);
    add_number_safe(sr, "command_audio_drops", sr_status.command_audio_drops);
    cJSON_AddStringToObject(sr, "last_result", sr_status.last_result);

    llm_intent_status_t intent_status = {0};
    llm_intent_get_status(&intent_status);
    cJSON *intent = cJSON_AddObjectToObject(root, "llm_intent");
    add_bool(intent, "configured", intent_status.configured);
    add_number_safe(intent, "local_count", intent_status.local_count);
    add_number_safe(intent, "llm_count", intent_status.llm_count);
    add_number_safe(intent, "fallback_count", intent_status.fallback_count);
    add_number_safe(intent, "last_error", intent_status.last_error);
    add_number_safe(intent, "last_http_status", intent_status.last_http_status);
    cJSON_AddStringToObject(intent, "last_result", intent_status.last_result);

    online_asr_status_t asr_status = {0};
    online_asr_get_status(&asr_status);
    cJSON *asr = cJSON_AddObjectToObject(root, "online_asr");
    add_bool(asr, "enabled", asr_status.enabled);
    add_bool(asr, "configured", asr_status.configured);
    add_bool(asr, "worker_running", asr_status.worker_running);
    add_bool(asr, "busy", asr_status.busy);
    add_number_safe(asr, "submitted_count", asr_status.submitted_count);
    add_number_safe(asr, "ok_count", asr_status.ok_count);
    add_number_safe(asr, "failed_count", asr_status.failed_count);
    add_number_safe(asr, "dropped_count", asr_status.dropped_count);
    add_number_safe(asr, "last_error", asr_status.last_error);
    add_number_safe(asr, "last_http_status", asr_status.last_http_status);
    cJSON_AddStringToObject(asr, "last_result", asr_status.last_result);
    cJSON_AddStringToObject(asr, "last_text", asr_status.last_text);

    stereo_audio_status_t audio_status;
    stereo_audio_get_status(&audio_status);
    cJSON *audio = cJSON_AddObjectToObject(root, "stereo_audio");
    add_bool(audio, "initialized", audio_status.initialized);
    add_number_safe(audio, "mode", audio_status.mode);
    add_number_safe(audio, "volume_percent", audio_status.volume_percent);
    add_number_safe(audio, "sample_rate_hz", audio_status.sample_rate_hz);
    add_number_safe(audio, "wav_request_id", audio_status.wav_request_id);
    add_number_safe(audio, "wav_open_request_id", audio_status.wav_open_request_id);
    add_number_safe(audio, "wav_data_remaining", audio_status.wav_data_remaining);
    add_number_safe(audio, "write_calls", audio_status.write_calls);
    add_number_safe(audio, "write_errors", audio_status.write_errors);
    add_number_safe(audio, "last_error", audio_status.last_error);
    cJSON_AddStringToObject(audio, "wav_path", audio_status.wav_path);
    cJSON_AddStringToObject(audio, "last_result", audio_status.last_result);

    cJSON_AddStringToObject(root, "action", t.decision.skill_name);
    add_sleep_profile_json(root, &profile);
    return send_json(req, root);
}

static esp_err_t dashboard_handler(httpd_req_t *req)
{
    mobile_app_telemetry_t t;
    sleep_profile_state_t profile;
    portENTER_CRITICAL(&s_app.lock);
    t = s_app.telemetry;
    profile = s_app.profile;
    portEXIT_CRITICAL(&s_app.lock);

    cJSON *root = cJSON_CreateObject();
    add_number_safe(root, "uptime_sec", t.uptime_sec);
    add_bool(root, "presence", t.radar.human_present);
    add_wifi_json(root);
    add_time_json(root);
    add_ai_bridge_json(root);

    cJSON *bio = cJSON_AddObjectToObject(root, "bio");
    add_number_safe(bio, "breath_bpm", t.bio.breath_bpm_smooth);
    add_number_safe(bio, "heart_bpm", t.bio.heart_bpm_smooth);

    cJSON *sleep = cJSON_AddObjectToObject(root, "sleep");
    cJSON_AddStringToObject(sleep, "state", sleep_state_to_name(t.assessment.state));
    add_number_safe(sleep, "score", t.assessment.sleep_score);
    add_number_safe(sleep, "wake_risk", t.assessment.wake_risk);

    cJSON *assist = cJSON_AddObjectToObject(root, "assist");
    cJSON_AddStringToObject(assist, "state", assist_state_to_text(t.assist.state));
    add_number_safe(assist, "target_breath", t.assist.target_breath_bpm);

    cJSON *csi = cJSON_AddObjectToObject(root, "csi");
    add_bool(csi, "breath_valid", t.csi.breath_proxy_valid);
    add_number_safe(csi, "packets", t.csi.packet_count);
    add_number_safe(csi, "rssi", t.csi.last_rssi);
    add_number_safe(csi, "motion", t.csi.motion_index);
    add_number_safe(csi, "stability", t.csi.stability_score);
    add_number_safe(csi, "breath_bpm", t.csi.breath_proxy_bpm);

    cJSON_AddStringToObject(root, "action", t.decision.skill_name);
    add_sleep_profile_json(root, &profile);
    return send_json(req, root);
}

static esp_err_t audio_status_handler(httpd_req_t *req)
{
    stereo_audio_status_t status;
    stereo_audio_get_status(&status);
    cJSON *root = cJSON_CreateObject();
    add_bool(root, "initialized", status.initialized);
    add_number_safe(root, "mode", status.mode);
    add_number_safe(root, "volume_percent", status.volume_percent);
    add_number_safe(root, "sample_rate_hz", status.sample_rate_hz);
    add_number_safe(root, "wav_request_id", status.wav_request_id);
    add_number_safe(root, "wav_open_request_id", status.wav_open_request_id);
    add_number_safe(root, "wav_data_remaining", status.wav_data_remaining);
    add_number_safe(root, "write_calls", status.write_calls);
    add_number_safe(root, "write_errors", status.write_errors);
    add_number_safe(root, "last_error", status.last_error);
    cJSON_AddStringToObject(root, "wav_path", status.wav_path);
    cJSON_AddStringToObject(root, "last_result", status.last_result);
    return send_json(req, root);
}

static mobile_app_command_t parse_command(const char *cmd)
{
    if (!cmd) {
        return MOBILE_APP_COMMAND_NONE;
    }
    mobile_app_command_t named = llm_intent_command_from_name(cmd);
    if (named != MOBILE_APP_COMMAND_NONE) {
        return named;
    }
    if (strcmp(cmd, "sleep") == 0) {
        return MOBILE_APP_COMMAND_SLEEP;
    }
    if (strcmp(cmd, "slept") == 0 || strcmp(cmd, "sleep_locked") == 0) {
        return MOBILE_APP_COMMAND_SLEPT;
    }
    if (strcmp(cmd, "stop") == 0) {
        return MOBILE_APP_COMMAND_STOP;
    }
    if (strcmp(cmd, "selftest") == 0) {
        return MOBILE_APP_COMMAND_SELF_TEST;
    }
    if (strcmp(cmd, "light_red") == 0) {
        return MOBILE_APP_COMMAND_LIGHT_RED;
    }
    if (strcmp(cmd, "light_green") == 0) {
        return MOBILE_APP_COMMAND_LIGHT_GREEN;
    }
    if (strcmp(cmd, "light_blue") == 0) {
        return MOBILE_APP_COMMAND_LIGHT_BLUE;
    }
    if (strcmp(cmd, "light_yellow") == 0) {
        return MOBILE_APP_COMMAND_LIGHT_YELLOW;
    }
    if (strcmp(cmd, "light_off") == 0) {
        return MOBILE_APP_COMMAND_LIGHT_OFF;
    }
    if (strcmp(cmd, "screen_red") == 0) {
        return MOBILE_APP_COMMAND_SCREEN_RED;
    }
    if (strcmp(cmd, "screen_green") == 0) {
        return MOBILE_APP_COMMAND_SCREEN_GREEN;
    }
    if (strcmp(cmd, "screen_blue") == 0) {
        return MOBILE_APP_COMMAND_SCREEN_BLUE;
    }
    if (strcmp(cmd, "screen_yellow") == 0) {
        return MOBILE_APP_COMMAND_SCREEN_YELLOW;
    }
    if (strcmp(cmd, "screen_off") == 0) {
        return MOBILE_APP_COMMAND_SCREEN_OFF;
    }
    if (strcmp(cmd, "audio_music") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_MUSIC;
    }
    if (strcmp(cmd, "audio_noise") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_NOISE;
    }
    if (strcmp(cmd, "audio_breathing") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_BREATHING;
    }
    if (strcmp(cmd, "audio_volume_max") == 0 || strcmp(cmd, "volume_max") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_VOLUME_MAX;
    }
    if (strcmp(cmd, "audio_volume_up") == 0 || strcmp(cmd, "volume_up") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_VOLUME_UP;
    }
    if (strcmp(cmd, "audio_volume_down") == 0 || strcmp(cmd, "volume_down") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_VOLUME_DOWN;
    }
    if (strcmp(cmd, "audio_mute") == 0 || strcmp(cmd, "mute") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_MUTE;
    }
    if (strcmp(cmd, "story") == 0) {
        return MOBILE_APP_COMMAND_STORY;
    }
    if (strcmp(cmd, "scene_ppm") == 0) return MOBILE_APP_COMMAND_SCENE_PPM;
    if (strcmp(cmd, "scene_ocean") == 0) return MOBILE_APP_COMMAND_SCENE_OCEAN;
    if (strcmp(cmd, "scene_forest") == 0) return MOBILE_APP_COMMAND_SCENE_FOREST;
    if (strcmp(cmd, "scene_rain") == 0) return MOBILE_APP_COMMAND_SCENE_RAIN;
    if (strcmp(cmd, "scene_zen") == 0) return MOBILE_APP_COMMAND_SCENE_ZEN;
    if (strcmp(cmd, "scene_empty") == 0) return MOBILE_APP_COMMAND_SCENE_EMPTY;
    if (strcmp(cmd, "breath_relax") == 0) return MOBILE_APP_COMMAND_BREATH_RELAX;
    if (strcmp(cmd, "breath_box") == 0) return MOBILE_APP_COMMAND_BREATH_BOX;
    if (strcmp(cmd, "breath_deep") == 0) return MOBILE_APP_COMMAND_BREATH_DEEP;
    if (strcmp(cmd, "mid_sleep_test_minor") == 0) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_MINOR;
    if (strcmp(cmd, "mid_sleep_test_restless") == 0) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_RESTLESS;
    if (strcmp(cmd, "mid_sleep_test_arousal") == 0) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_AROUSAL;
    if (strcmp(cmd, "mid_sleep_test_out_of_bed") == 0) return MOBILE_APP_COMMAND_MID_SLEEP_TEST_OUT_OF_BED;
    return MOBILE_APP_COMMAND_NONE;
}

static void json_copy_string(cJSON *root, const char *key, char *out, size_t out_size)
{
    if (!root || !key || !out || out_size == 0 || out[0]) {
        return;
    }
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        size_t len = strlen(item->valuestring);
        if (len >= out_size) {
            len = out_size - 1;
        }
        memcpy(out, item->valuestring, len);
        out[len] = '\0';
    }
}

static void read_command_request_text(httpd_req_t *req,
                                      char *cmd_text, size_t cmd_size,
                                      char *intent_text, size_t text_size)
{
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "cmd", cmd_text, cmd_size);
        if (intent_text && intent_text[0] == '\0') {
            (void)httpd_query_key_value(query, "text", intent_text, text_size);
        }
        if (intent_text && intent_text[0] == '\0') {
            (void)httpd_query_key_value(query, "transcript", intent_text, text_size);
        }
        if (intent_text && intent_text[0] == '\0') {
            (void)httpd_query_key_value(query, "utterance", intent_text, text_size);
        }
    }

    if (req->content_len <= 0 || req->content_len > 768) {
        return;
    }
    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        return;
    }
    int total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, body + total, req->content_len - total);
        if (received <= 0) break;
        total += received;
    }
    body[total] = '\0';
    if (total > 0) {
        cJSON *json = cJSON_Parse(body);
        if (json) {
            json_copy_string(json, "cmd", cmd_text, cmd_size);
            json_copy_string(json, "command", cmd_text, cmd_size);
            json_copy_string(json, "text", intent_text, text_size);
            json_copy_string(json, "transcript", intent_text, text_size);
            json_copy_string(json, "utterance", intent_text, text_size);
            cJSON_Delete(json);
        }
    }
    free(body);
}

static esp_err_t command_handler(httpd_req_t *req)
{
    char cmd_text[64] = {0};
    char intent_text[256] = {0};
    read_command_request_text(req, cmd_text, sizeof(cmd_text), intent_text, sizeof(intent_text));

    mobile_app_command_t cmd = parse_command(cmd_text);
    llm_intent_result_t intent = {0};
    esp_err_t intent_err = ESP_OK;
    if (cmd == MOBILE_APP_COMMAND_NONE && intent_text[0] != '\0') {
        intent_err = llm_intent_resolve_text(intent_text, true, &intent);
        if (intent_err == ESP_OK || intent.command != MOBILE_APP_COMMAND_NONE) {
            cmd = intent.command;
        }
    }

    esp_err_t cb_err = ESP_OK;
    if (cmd != MOBILE_APP_COMMAND_NONE && s_app.cfg.command_cb) {
        cb_err = s_app.cfg.command_cb(cmd, s_app.cfg.command_ctx);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command", cmd_text);
    cJSON_AddStringToObject(root, "text", intent_text);
    cJSON_AddNumberToObject(root, "code", cmd);
    cJSON_AddNumberToObject(root, "result", cb_err);
    cJSON_AddNumberToObject(root, "intent_error", intent_err);
    cJSON_AddNumberToObject(root, "intent_source", intent.source);
    cJSON_AddStringToObject(root, "intent_command", intent.command_name[0] ? intent.command_name : llm_intent_command_to_name(cmd));
    cJSON_AddStringToObject(root, "reply", intent.reply[0] ? intent.reply : llm_intent_command_reply(cmd, cb_err));
    printf("DG Command: http text=\"%s\" command=%s(%d) result=%s intent=%s source=%d\r\n",
           intent_text[0] ? intent_text : cmd_text,
           llm_intent_command_to_name(cmd),
           (int)cmd,
           esp_err_to_name(cb_err),
           esp_err_to_name(intent_err),
           (int)intent.source);
    return send_json(req, root);
}


static void copy_field(uint8_t *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    size_t n = src ? strlen(src) : 0;
    if (n >= dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, src ? src : "", n);
    dst[n] = 0;
}

static esp_err_t configure_setup_ap(const mobile_app_config_t *config)
{
    const char *ssid = (config && config->ssid) ? config->ssid : SETUP_AP_DEFAULT_SSID;
    const char *password = (config && config->password) ? config->password : SETUP_AP_DEFAULT_PASSWORD;
    uint8_t channel = (config && config->channel != 0) ? config->channel : SETUP_AP_DEFAULT_CHANNEL;

    wifi_config_t ap_config = {0};
    copy_field(ap_config.ap.ssid, sizeof(ap_config.ap.ssid), ssid);
    copy_field(ap_config.ap.password, sizeof(ap_config.ap.password), password);
    ap_config.ap.ssid_len = strlen((const char *)ap_config.ap.ssid);
    ap_config.ap.channel = channel;
    ap_config.ap.max_connection = 2;
    ap_config.ap.authmode = strlen((const char *)ap_config.ap.password) >= 8 ?
        WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap_config.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    ap_config.ap.ssid_hidden = 0;
    ap_config.ap.beacon_interval = 100;
    ap_config.ap.pmf_cfg.capable = false;
    ap_config.ap.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_AP, &ap_config);
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    size_t n = src ? strlen(src) : 0;
    if (n >= dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, src ? src : "", n);
    dst[n] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool form_value(const char *body, const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    char pattern[16];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *p = strstr(body, pattern);
    if (!p) {
        return false;
    }
    p += strlen(pattern);
    size_t out_pos = 0;
    for (size_t i = 0; p[i] && p[i] != '&' && out_pos + 1 < out_size; ++i) {
        if (p[i] == '+') {
            out[out_pos++] = ' ';
        } else if (p[i] == '%' && p[i + 1] && p[i + 2]) {
            int hi = hex_value(p[i + 1]);
            int lo = hex_value(p[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[out_pos++] = (char)((hi << 4) | lo);
                i += 2;
            }
        } else {
            out[out_pos++] = p[i];
        }
    }
    out[out_pos] = '\0';
    return true;
}

static void claw_llm_defaults(claw_llm_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->provider, sizeof(cfg->provider), "%s", "qwen");
    snprintf(cfg->backend_type, sizeof(cfg->backend_type), "%s", "openai_compatible");
    snprintf(cfg->model, sizeof(cfg->model), "%s", "qwen3.5-omni-flash");
    snprintf(cfg->base_url, sizeof(cfg->base_url), "%s", "https://dashscope.aliyuncs.com/compatible-mode/v1");
    snprintf(cfg->auth_type, sizeof(cfg->auth_type), "%s", "bearer");
    snprintf(cfg->max_tokens, sizeof(cfg->max_tokens), "%s", "8192");
    snprintf(cfg->timeout_ms, sizeof(cfg->timeout_ms), "%s", "120000");
    snprintf(cfg->default_image_max_bytes, sizeof(cfg->default_image_max_bytes), "%s", "524288");
    snprintf(cfg->max_tokens_field, sizeof(cfg->max_tokens_field), "%s", "max_completion_tokens");
    snprintf(cfg->supports_tools, sizeof(cfg->supports_tools), "%s", "true");
    snprintf(cfg->supports_vision, sizeof(cfg->supports_vision), "%s", "false");
    snprintf(cfg->image_remote_url_only, sizeof(cfg->image_remote_url_only), "%s", "false");
}

static void nvs_get_str_default(nvs_handle_t nvs, const char *key, char *out, size_t out_size, const char *fallback)
{
    if (!out || out_size == 0) {
        return;
    }
    size_t len = out_size;
    if (nvs_get_str(nvs, key, out, &len) != ESP_OK) {
        snprintf(out, out_size, "%s", fallback ? fallback : "");
    }
}

static esp_err_t claw_llm_load(claw_llm_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    claw_llm_defaults(cfg);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CLAW_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    nvs_get_str_default(nvs, CLAW_NVS_PROVIDER, cfg->provider, sizeof(cfg->provider), cfg->provider);
    nvs_get_str_default(nvs, CLAW_NVS_BACKEND_TYPE, cfg->backend_type, sizeof(cfg->backend_type), cfg->backend_type);
    nvs_get_str_default(nvs, CLAW_NVS_MODEL, cfg->model, sizeof(cfg->model), cfg->model);
    nvs_get_str_default(nvs, CLAW_NVS_BASE_URL, cfg->base_url, sizeof(cfg->base_url), cfg->base_url);
    nvs_get_str_default(nvs, CLAW_NVS_API_KEY, cfg->api_key, sizeof(cfg->api_key), "");
    nvs_get_str_default(nvs, CLAW_NVS_AUTH_TYPE, cfg->auth_type, sizeof(cfg->auth_type), cfg->auth_type);
    nvs_get_str_default(nvs, CLAW_NVS_MAX_TOKENS, cfg->max_tokens, sizeof(cfg->max_tokens), cfg->max_tokens);
    nvs_get_str_default(nvs, CLAW_NVS_TIMEOUT_MS, cfg->timeout_ms, sizeof(cfg->timeout_ms), cfg->timeout_ms);
    nvs_get_str_default(nvs, CLAW_NVS_IMAGE_MAX_BYTES, cfg->default_image_max_bytes, sizeof(cfg->default_image_max_bytes), cfg->default_image_max_bytes);
    nvs_get_str_default(nvs, CLAW_NVS_MAX_TOKENS_FIELD, cfg->max_tokens_field, sizeof(cfg->max_tokens_field), cfg->max_tokens_field);
    nvs_get_str_default(nvs, CLAW_NVS_SUPPORTS_TOOLS, cfg->supports_tools, sizeof(cfg->supports_tools), cfg->supports_tools);
    nvs_get_str_default(nvs, CLAW_NVS_SUPPORTS_VISION, cfg->supports_vision, sizeof(cfg->supports_vision), cfg->supports_vision);
    nvs_get_str_default(nvs, CLAW_NVS_IMAGE_REMOTE_ONLY, cfg->image_remote_url_only, sizeof(cfg->image_remote_url_only), cfg->image_remote_url_only);
    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t claw_llm_save(const claw_llm_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open claw nvs");
    esp_err_t err = nvs_set_str(nvs, CLAW_NVS_PROVIDER, cfg->provider);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_BACKEND_TYPE, cfg->backend_type);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_MODEL, cfg->model);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_BASE_URL, cfg->base_url);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_AUTH_TYPE, cfg->auth_type);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_MAX_TOKENS, cfg->max_tokens);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_TIMEOUT_MS, cfg->timeout_ms);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_IMAGE_MAX_BYTES, cfg->default_image_max_bytes);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_MAX_TOKENS_FIELD, cfg->max_tokens_field);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_SUPPORTS_TOOLS, cfg->supports_tools);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_SUPPORTS_VISION, cfg->supports_vision);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_IMAGE_REMOTE_ONLY, cfg->image_remote_url_only);
    if (err == ESP_OK && cfg->api_key[0]) err = nvs_set_str(nvs, CLAW_NVS_API_KEY, cfg->api_key);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void claw_im_defaults(claw_im_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->qq_msg_type, sizeof(cfg->qq_msg_type), "%s", "0");
    snprintf(cfg->wechat_base_url, sizeof(cfg->wechat_base_url), "%s", "https://ilinkai.weixin.qq.com");
    snprintf(cfg->wechat_cdn_base_url, sizeof(cfg->wechat_cdn_base_url), "%s", "https://novac2c.cdn.weixin.qq.com/c2c");
}

static esp_err_t claw_im_load(claw_im_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    claw_im_defaults(cfg);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CLAW_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    nvs_get_str_default(nvs, CLAW_NVS_QQ_APP_ID, cfg->qq_app_id, sizeof(cfg->qq_app_id), "");
    nvs_get_str_default(nvs, CLAW_NVS_QQ_APP_SECRET, cfg->qq_app_secret, sizeof(cfg->qq_app_secret), "");
    nvs_get_str_default(nvs, CLAW_NVS_QQ_MSG_TYPE, cfg->qq_msg_type, sizeof(cfg->qq_msg_type), cfg->qq_msg_type);
    nvs_get_str_default(nvs, CLAW_NVS_FEISHU_APP_ID, cfg->feishu_app_id, sizeof(cfg->feishu_app_id), "");
    nvs_get_str_default(nvs, CLAW_NVS_FEISHU_APP_SECRET, cfg->feishu_app_secret, sizeof(cfg->feishu_app_secret), "");
    nvs_get_str_default(nvs, CLAW_NVS_TG_BOT_TOKEN, cfg->tg_bot_token, sizeof(cfg->tg_bot_token), "");
    nvs_get_str_default(nvs, CLAW_NVS_WECHAT_TOKEN, cfg->wechat_token, sizeof(cfg->wechat_token), "");
    nvs_get_str_default(nvs, CLAW_NVS_WECHAT_BASE_URL, cfg->wechat_base_url, sizeof(cfg->wechat_base_url), cfg->wechat_base_url);
    nvs_get_str_default(nvs, CLAW_NVS_WECHAT_CDN_URL, cfg->wechat_cdn_base_url, sizeof(cfg->wechat_cdn_base_url), cfg->wechat_cdn_base_url);
    nvs_get_str_default(nvs, CLAW_NVS_WECHAT_ACCOUNT_ID, cfg->wechat_account_id, sizeof(cfg->wechat_account_id), "");
    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t claw_im_save(const claw_im_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open claw nvs");
    esp_err_t err = nvs_set_str(nvs, CLAW_NVS_QQ_APP_ID, cfg->qq_app_id);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_QQ_APP_SECRET, cfg->qq_app_secret);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_QQ_MSG_TYPE, cfg->qq_msg_type);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_FEISHU_APP_ID, cfg->feishu_app_id);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_FEISHU_APP_SECRET, cfg->feishu_app_secret);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_TG_BOT_TOKEN, cfg->tg_bot_token);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_WECHAT_TOKEN, cfg->wechat_token);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_WECHAT_BASE_URL, cfg->wechat_base_url);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_WECHAT_CDN_URL, cfg->wechat_cdn_base_url);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_WECHAT_ACCOUNT_ID, cfg->wechat_account_id);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void claw_asr_defaults(claw_asr_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->enabled, sizeof(cfg->enabled), "%s", "true");
    snprintf(cfg->model, sizeof(cfg->model), "%s", "gpt-4o-mini-transcribe");
    snprintf(cfg->base_url, sizeof(cfg->base_url), "%s", "https://api.openai.com/v1");
    snprintf(cfg->language, sizeof(cfg->language), "%s", "zh");
    snprintf(cfg->timeout_ms, sizeof(cfg->timeout_ms), "%s", "30000");
}

static esp_err_t claw_asr_load(claw_asr_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    claw_asr_defaults(cfg);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CLAW_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    uint8_t enabled = 1;
    if (nvs_get_u8(nvs, CLAW_NVS_ASR_ENABLED, &enabled) == ESP_OK) {
        snprintf(cfg->enabled, sizeof(cfg->enabled), "%s", enabled ? "true" : "false");
    }
    nvs_get_str_default(nvs, CLAW_NVS_ASR_MODEL, cfg->model, sizeof(cfg->model), cfg->model);
    nvs_get_str_default(nvs, CLAW_NVS_ASR_BASE_URL, cfg->base_url, sizeof(cfg->base_url), cfg->base_url);
    nvs_get_str_default(nvs, CLAW_NVS_ASR_API_KEY, cfg->api_key, sizeof(cfg->api_key), "");
    nvs_get_str_default(nvs, CLAW_NVS_ASR_LANGUAGE, cfg->language, sizeof(cfg->language), cfg->language);
    uint32_t timeout_ms = 0;
    if (nvs_get_u32(nvs, CLAW_NVS_ASR_TIMEOUT_MS, &timeout_ms) == ESP_OK && timeout_ms > 0) {
        snprintf(cfg->timeout_ms, sizeof(cfg->timeout_ms), "%lu", (unsigned long)timeout_ms);
    }
    nvs_close(nvs);
    return ESP_OK;
}

static bool text_truthy(const char *text)
{
    return text && (strcmp(text, "1") == 0 || strcasecmp(text, "true") == 0 ||
                    strcasecmp(text, "yes") == 0 || strcasecmp(text, "on") == 0);
}

static esp_err_t claw_asr_save(const claw_asr_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open claw nvs");
    esp_err_t err = nvs_set_u8(nvs, CLAW_NVS_ASR_ENABLED, text_truthy(cfg->enabled) ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_ASR_MODEL, cfg->model);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_ASR_BASE_URL, cfg->base_url);
    if (err == ESP_OK && cfg->api_key[0]) err = nvs_set_str(nvs, CLAW_NVS_ASR_API_KEY, cfg->api_key);
    if (err == ESP_OK) err = nvs_set_str(nvs, CLAW_NVS_ASR_LANGUAGE, cfg->language);
    uint32_t timeout_ms = (uint32_t)strtoul(cfg->timeout_ms, NULL, 10);
    if (timeout_ms < 5000) timeout_ms = 30000;
    if (err == ESP_OK) err = nvs_set_u32(nvs, CLAW_NVS_ASR_TIMEOUT_MS, timeout_ms);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void add_claw_llm_json(cJSON *root)
{
    claw_llm_config_t cfg = {0};
    (void)claw_llm_load(&cfg);
    cJSON *llm = cJSON_AddObjectToObject(root, "llm");
    cJSON_AddStringToObject(llm, "provider", cfg.provider);
    cJSON_AddStringToObject(llm, "backend_type", cfg.backend_type);
    cJSON_AddStringToObject(llm, "model", cfg.model);
    cJSON_AddStringToObject(llm, "base_url", cfg.base_url);
    cJSON_AddStringToObject(llm, "auth_type", cfg.auth_type);
    cJSON_AddStringToObject(llm, "max_tokens", cfg.max_tokens);
    cJSON_AddStringToObject(llm, "timeout_ms", cfg.timeout_ms);
    cJSON_AddStringToObject(llm, "default_image_max_bytes", cfg.default_image_max_bytes);
    cJSON_AddStringToObject(llm, "max_tokens_field", cfg.max_tokens_field);
    cJSON_AddStringToObject(llm, "supports_tools", cfg.supports_tools);
    cJSON_AddStringToObject(llm, "supports_vision", cfg.supports_vision);
    cJSON_AddStringToObject(llm, "image_remote_url_only", cfg.image_remote_url_only);
    add_bool(llm, "api_key_configured", cfg.api_key[0] != '\0');
}

static void add_claw_im_json(cJSON *root)
{
    claw_im_config_t cfg = {0};
    (void)claw_im_load(&cfg);
    cJSON *im = cJSON_AddObjectToObject(root, "im");
    cJSON_AddStringToObject(im, "qq_app_id", cfg.qq_app_id);
    add_bool(im, "qq_configured", cfg.qq_app_id[0] != '\0' && cfg.qq_app_secret[0] != '\0');
    cJSON_AddStringToObject(im, "qq_msg_type", cfg.qq_msg_type);
    cJSON_AddStringToObject(im, "feishu_app_id", cfg.feishu_app_id);
    add_bool(im, "feishu_configured", cfg.feishu_app_id[0] != '\0' && cfg.feishu_app_secret[0] != '\0');
    add_bool(im, "tg_configured", cfg.tg_bot_token[0] != '\0');
    add_bool(im, "wechat_configured", cfg.wechat_token[0] != '\0');
    cJSON_AddStringToObject(im, "wechat_base_url", cfg.wechat_base_url);
    cJSON_AddStringToObject(im, "wechat_cdn_base_url", cfg.wechat_cdn_base_url);
    cJSON_AddStringToObject(im, "wechat_account_id", cfg.wechat_account_id);
}

static void add_claw_feishu_json(cJSON *root)
{
    ai_bridge_config_t cfg = {0};
    ai_bridge_status_t status = {0};
    (void)ai_bridge_get_config(&cfg);
    ai_bridge_get_status(&status);
    cJSON *feishu = cJSON_AddObjectToObject(root, "feishu");
    add_bool(feishu, "enabled", status.enabled);
    add_bool(feishu, "configured", status.configured);
    cJSON_AddNumberToObject(feishu, "min_interval_sec", cfg.min_interval_sec);
    cJSON_AddNumberToObject(feishu, "queued", status.queued_count);
    cJSON_AddNumberToObject(feishu, "sent", status.sent_count);
    cJSON_AddNumberToObject(feishu, "dropped", status.dropped_count);
    cJSON_AddNumberToObject(feishu, "last_http_status", status.last_http_status);
    cJSON_AddStringToObject(feishu, "last_result", status.last_result);
}

static esp_err_t connect_sta_with_current_credentials(void)
{
    if (!s_app.sta_ssid[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_config_t sta_config = {0};
    copy_field(sta_config.sta.ssid, sizeof(sta_config.sta.ssid), s_app.sta_ssid);
    copy_field(sta_config.sta.password, sizeof(sta_config.sta.password), s_app.sta_password);
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    sta_config.sta.listen_interval = 1;
    sta_config.sta.failure_retry_cnt = 5;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), TAG, "wifi sta config");
    return esp_wifi_connect();
}

static esp_err_t retry_sta_connect(const char *reason)
{
    if (!s_app.sta_has_credentials) {
        return ESP_ERR_INVALID_STATE;
    }
    s_app.last_sta_retry_us = esp_timer_get_time();
    ESP_LOGI(TAG, "STA retry %u reason=%s profile=%u/%u ssid=%s",
             (unsigned)s_app.sta_retry_count, reason ? reason : "manual",
             (unsigned)(s_app.sta_profile_index + 1), (unsigned)s_app.sta_profile_count,
             s_app.sta_ssid);
    return connect_sta_with_current_credentials();
}

static esp_err_t provision_handler(httpd_req_t *req)
{
    char body[192] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty form");
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!form_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
    }
    (void)form_value(body, "pass", pass, sizeof(pass));

    ESP_RETURN_ON_ERROR(save_wifi_credentials(ssid, pass), TAG, "save wifi credentials");
    portENTER_CRITICAL(&s_app.lock);
    snprintf(s_app.sta_ssid, sizeof(s_app.sta_ssid), "%s", ssid);
    snprintf(s_app.sta_password, sizeof(s_app.sta_password), "%s", pass);
    s_app.sta_has_credentials = true;
    s_app.sta_connected = false;
    s_app.sta_retry_count = 0;
    s_app.sta_profile_index = 0;
    s_app.sta_profiles_tried_in_cycle = 0;
    s_app.sta_config_version++;
    s_app.last_sta_retry_us = 0;
    s_app.sta_ip[0] = '\0';
    portEXIT_CRITICAL(&s_app.lock);
    printf("DG Wi-Fi saved: ssid=%s; connecting. Setup AP stays at DreamGuardian-Setup / http://192.168.4.1/provision\r\n",
           ssid);
    (void)configure_setup_ap(&s_app.cfg);
    esp_err_t err = retry_sta_connect("provision_page");
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "connect after provision failed: %s", esp_err_to_name(err));
    }

    char response[768];
    snprintf(response, sizeof(response),
             "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Connecting</title></head>"
             "<body style='font-family:Arial;background:#050201;color:#f8f2e8;padding:20px'><h2>Wi-Fi saved</h2>"
             "<p>Device is connecting to <b>%s</b>. Wait 10-20 seconds, then switch your phone back to this Wi-Fi and open the IP shown on the device serial/screen.</p>"
             "<p>You can also refresh <a href='/api/status'>/api/status</a> while still connected to setup AP.</p></body></html>",
             ssid);
    prepare_http_response(req, "text/html; charset=utf-8");
    esp_err_t send_err = httpd_resp_sendstr(req, response);
    close_http_session(req);
    return send_err;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    return send_html_static(req, INDEX_HTML);
}

static esp_err_t provision_page_handler(httpd_req_t *req)
{
    return send_html_static(req, PROVISION_HTML);
}

static esp_err_t claw_page_handler(httpd_req_t *req)
{
    return send_html_static(req, CLAW_LITE_HTML);
}

static esp_err_t sleep_music_page_handler(httpd_req_t *req)
{
    return send_html_static(req, SLEEP_MUSIC_LITE_HTML);
#if 0
    FILE *file = fopen("/spiffs/sleep_music.html", "rb");
    if (!file) {
        return send_html_static(req, SLEEP_MUSIC_LITE_HTML);
    }
    prepare_http_response(req, "text/html; charset=utf-8");
    esp_err_t err = ESP_OK;
    char chunk[1024];
    while (true) {
        size_t read_len = fread(chunk, 1, sizeof(chunk), file);
        if (read_len > 0) {
            err = httpd_resp_send_chunk(req, chunk, read_len);
            if (err != ESP_OK) {
                break;
            }
        }
        if (read_len < sizeof(chunk)) {
            if (ferror(file)) {
                err = ESP_FAIL;
            }
            break;
        }
    }
    fclose(file);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
#endif
}

static esp_err_t wake_word_page_handler(httpd_req_t *req)
{
    return send_html_static(req, WAKE_WORD_GUIDED_HTML);
}

static esp_err_t claw_config_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    add_claw_llm_json(root);
    add_claw_im_json(root);
    add_claw_feishu_json(root);
    return send_json(req, root);
}

static esp_err_t claw_config_post_handler(httpd_req_t *req)
{
    char body[768] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid form");
    }
    body[received] = '\0';

    claw_llm_config_t llm = {0};
    (void)claw_llm_load(&llm);
    char field[CLAW_FIELD_URL_MAX] = {0};
    if (form_value(body, "provider", field, sizeof(field)) && field[0]) {
        copy_text(llm.provider, sizeof(llm.provider), field);
    }
    if (form_value(body, "model", field, sizeof(field)) && field[0]) {
        copy_text(llm.model, sizeof(llm.model), field);
    }
    if (form_value(body, "base_url", field, sizeof(field)) && field[0]) {
        copy_text(llm.base_url, sizeof(llm.base_url), field);
    }
    if (form_value(body, "auth_type", field, sizeof(field)) && field[0]) {
        copy_text(llm.auth_type, sizeof(llm.auth_type), field);
    }
    if (form_value(body, "max_tokens", field, sizeof(field)) && field[0]) {
        copy_text(llm.max_tokens, sizeof(llm.max_tokens), field);
    }
    if (form_value(body, "timeout_ms", field, sizeof(field)) && field[0]) {
        copy_text(llm.timeout_ms, sizeof(llm.timeout_ms), field);
    }
    if (form_value(body, "api_key", field, sizeof(field)) && field[0]) {
        copy_text(llm.api_key, sizeof(llm.api_key), field);
    }

    esp_err_t llm_err = claw_llm_save(&llm);

    ai_bridge_config_t ai_cfg = {0};
    (void)ai_bridge_get_config(&ai_cfg);
    char enabled[8] = {0};
    char interval[16] = {0};
    char webhook[AI_BRIDGE_WEBHOOK_URL_MAX] = {0};
    if (form_value(body, "feishu_enabled", enabled, sizeof(enabled))) {
        ai_cfg.enabled = strcmp(enabled, "1") == 0 || strcmp(enabled, "true") == 0 || strcmp(enabled, "on") == 0;
    }
    if (form_value(body, "feishu_interval", interval, sizeof(interval)) && interval[0]) {
        uint32_t value = (uint32_t)strtoul(interval, NULL, 10);
        if (value >= 30) {
            ai_cfg.min_interval_sec = value;
        }
    }
    if (form_value(body, "feishu_webhook", webhook, sizeof(webhook)) && webhook[0]) {
        snprintf(ai_cfg.feishu_webhook_url, sizeof(ai_cfg.feishu_webhook_url), "%s", webhook);
    }
    esp_err_t ai_err = ai_bridge_set_config(&ai_cfg);
    if (llm_err == ESP_OK && ai_err == ESP_OK && s_app.cfg.config_saved_cb) {
        s_app.cfg.config_saved_cb(s_app.cfg.config_saved_ctx);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "llm_result", llm_err);
    cJSON_AddNumberToObject(root, "feishu_result", ai_err);
    cJSON_AddStringToObject(root, "name", (llm_err == ESP_OK && ai_err == ESP_OK) ? "saved" : "save_failed");
    add_claw_llm_json(root);
    add_claw_im_json(root);
    add_claw_feishu_json(root);
    return send_json(req, root);
}

static void add_config_string(cJSON *root, const char *name, const char *value)
{
    cJSON_AddStringToObject(root, name, value ? value : "");
}

static void add_claw_config_json_flat(cJSON *root)
{
    claw_llm_config_t llm = {0};
    claw_im_config_t im = {0};
    claw_asr_config_t asr = {0};
    (void)claw_llm_load(&llm);
    (void)claw_im_load(&im);
    (void)claw_asr_load(&asr);

    add_config_string(root, "wifi_ssid", s_app.sta_ssid);
    add_config_string(root, "wifi_password", "");
    add_config_string(root, "ap_ssid", s_app.cfg.ssid ? s_app.cfg.ssid : SETUP_AP_DEFAULT_SSID);
    add_config_string(root, "ap_password", s_app.cfg.password ? s_app.cfg.password : "dream1234");
    add_config_string(root, "ap_behavior", "always");

    add_config_string(root, "llm_api_key", "");
    add_config_string(root, "llm_backend_type", llm.backend_type);
    add_config_string(root, "llm_model", llm.model);
    add_config_string(root, "llm_base_url", llm.base_url);
    add_config_string(root, "llm_auth_type", llm.auth_type);
    add_config_string(root, "llm_timeout_ms", llm.timeout_ms);
    add_config_string(root, "llm_max_tokens", llm.max_tokens);
    add_config_string(root, "llm_default_image_max_bytes", llm.default_image_max_bytes);
    add_config_string(root, "llm_max_tokens_field", llm.max_tokens_field);
    add_config_string(root, "llm_supports_tools", llm.supports_tools);
    add_config_string(root, "llm_supports_vision", llm.supports_vision);
    add_config_string(root, "llm_image_remote_url_only", llm.image_remote_url_only);

    add_config_string(root, "asr_enabled", asr.enabled);
    add_config_string(root, "asr_model", asr.model);
    add_config_string(root, "asr_base_url", asr.base_url);
    add_config_string(root, "asr_api_key", "");
    add_config_string(root, "asr_language", asr.language);
    add_config_string(root, "asr_timeout_ms", asr.timeout_ms);

    add_config_string(root, "qq_app_id", im.qq_app_id);
    add_config_string(root, "qq_app_secret", "");
    add_config_string(root, "qq_msg_type", im.qq_msg_type);
    add_config_string(root, "feishu_app_id", im.feishu_app_id);
    add_config_string(root, "feishu_app_secret", "");
    add_config_string(root, "tg_bot_token", "");
    add_config_string(root, "wechat_token", "");
    add_config_string(root, "wechat_base_url", im.wechat_base_url);
    add_config_string(root, "wechat_cdn_base_url", im.wechat_cdn_base_url);
    add_config_string(root, "wechat_account_id", im.wechat_account_id);

    add_config_string(root, "search_brave_key", "");
    add_config_string(root, "search_tavily_key", "");
    add_config_string(root, "search_http_allowlist", "");
    add_config_string(root, "enabled_cap_groups", "dreamguardian");
    add_config_string(root, "llm_visible_cap_groups", "dreamguardian");
    add_config_string(root, "enabled_lua_modules", "");
    add_config_string(root, "time_timezone", "Asia/Shanghai");

    cJSON *meta = cJSON_AddObjectToObject(root, "_dreamguardian");
    if (meta) {
        add_bool(meta, "llm_api_key_configured", llm.api_key[0] != '\0');
        add_bool(meta, "asr_api_key_configured", asr.api_key[0] != '\0' || llm.api_key[0] != '\0');
        add_bool(meta, "qq_configured", im.qq_app_id[0] != '\0' && im.qq_app_secret[0] != '\0');
        add_bool(meta, "feishu_configured", im.feishu_app_id[0] != '\0' && im.feishu_app_secret[0] != '\0');
        add_bool(meta, "tg_configured", im.tg_bot_token[0] != '\0');
        add_bool(meta, "wechat_configured", im.wechat_token[0] != '\0');
    }
}

static const char *json_string(cJSON *root, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    add_claw_config_json_flat(root);
    return send_json(req, root);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body size");
    }

    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, body + total, req->content_len - total);
        if (received <= 0) {
            break;
        }
        total += received;
    }
    body[total] = '\0';
    if (total <= 0) {
        free(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    claw_llm_config_t *llm = calloc(1, sizeof(*llm));
    claw_im_config_t *im = calloc(1, sizeof(*im));
    claw_asr_config_t *asr = calloc(1, sizeof(*asr));
    if (!llm || !im || !asr) {
        free(llm);
        free(im);
        free(asr);
        cJSON_Delete(json);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    (void)claw_llm_load(llm);
    (void)claw_im_load(im);
    (void)claw_asr_load(asr);

    size_t applied = 0;
#define APPLY_JSON_FIELD(name, dst, size) do { \
        const char *v__ = json_string(json, (name)); \
        if (v__) { copy_text((dst), (size), v__); applied++; } \
    } while (0)

    APPLY_JSON_FIELD("llm_api_key", llm->api_key, sizeof(llm->api_key));
    APPLY_JSON_FIELD("llm_backend_type", llm->backend_type, sizeof(llm->backend_type));
    APPLY_JSON_FIELD("llm_model", llm->model, sizeof(llm->model));
    APPLY_JSON_FIELD("llm_base_url", llm->base_url, sizeof(llm->base_url));
    APPLY_JSON_FIELD("llm_auth_type", llm->auth_type, sizeof(llm->auth_type));
    APPLY_JSON_FIELD("llm_timeout_ms", llm->timeout_ms, sizeof(llm->timeout_ms));
    APPLY_JSON_FIELD("llm_max_tokens", llm->max_tokens, sizeof(llm->max_tokens));
    APPLY_JSON_FIELD("llm_default_image_max_bytes", llm->default_image_max_bytes, sizeof(llm->default_image_max_bytes));
    APPLY_JSON_FIELD("llm_max_tokens_field", llm->max_tokens_field, sizeof(llm->max_tokens_field));
    APPLY_JSON_FIELD("llm_supports_tools", llm->supports_tools, sizeof(llm->supports_tools));
    APPLY_JSON_FIELD("llm_supports_vision", llm->supports_vision, sizeof(llm->supports_vision));
    APPLY_JSON_FIELD("llm_image_remote_url_only", llm->image_remote_url_only, sizeof(llm->image_remote_url_only));
    APPLY_JSON_FIELD("asr_enabled", asr->enabled, sizeof(asr->enabled));
    APPLY_JSON_FIELD("asr_api_key", asr->api_key, sizeof(asr->api_key));
    APPLY_JSON_FIELD("asr_model", asr->model, sizeof(asr->model));
    APPLY_JSON_FIELD("asr_base_url", asr->base_url, sizeof(asr->base_url));
    APPLY_JSON_FIELD("asr_language", asr->language, sizeof(asr->language));
    APPLY_JSON_FIELD("asr_timeout_ms", asr->timeout_ms, sizeof(asr->timeout_ms));
    APPLY_JSON_FIELD("qq_app_id", im->qq_app_id, sizeof(im->qq_app_id));
    APPLY_JSON_FIELD("qq_app_secret", im->qq_app_secret, sizeof(im->qq_app_secret));
    APPLY_JSON_FIELD("qq_msg_type", im->qq_msg_type, sizeof(im->qq_msg_type));
    APPLY_JSON_FIELD("feishu_app_id", im->feishu_app_id, sizeof(im->feishu_app_id));
    APPLY_JSON_FIELD("feishu_app_secret", im->feishu_app_secret, sizeof(im->feishu_app_secret));
    APPLY_JSON_FIELD("tg_bot_token", im->tg_bot_token, sizeof(im->tg_bot_token));
    APPLY_JSON_FIELD("wechat_token", im->wechat_token, sizeof(im->wechat_token));
    APPLY_JSON_FIELD("wechat_base_url", im->wechat_base_url, sizeof(im->wechat_base_url));
    APPLY_JSON_FIELD("wechat_cdn_base_url", im->wechat_cdn_base_url, sizeof(im->wechat_cdn_base_url));
    APPLY_JSON_FIELD("wechat_account_id", im->wechat_account_id, sizeof(im->wechat_account_id));
#undef APPLY_JSON_FIELD

    const char *wifi_ssid = json_string(json, "wifi_ssid");
    const char *wifi_pass = json_string(json, "wifi_password");
    esp_err_t wifi_err = ESP_OK;
    if (wifi_ssid && wifi_ssid[0]) {
        wifi_err = mobile_app_configure_wifi(wifi_ssid, wifi_pass ? wifi_pass : s_app.sta_password);
        applied++;
    }

    cJSON_Delete(json);

    if (applied == 0) {
        free(llm);
        free(im);
        free(asr);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request did not contain any recognised fields");
    }

    esp_err_t llm_err = claw_llm_save(llm);
    esp_err_t im_err = claw_im_save(im);
    esp_err_t asr_err = claw_asr_save(asr);
    bool saved_ok = llm_err == ESP_OK && im_err == ESP_OK && asr_err == ESP_OK && wifi_err == ESP_OK;
    free(llm);
    free(im);
    free(asr);
    if (saved_ok && s_app.cfg.config_saved_cb) {
        s_app.cfg.config_saved_cb(s_app.cfg.config_saved_ctx);
    }

    cJSON *root = cJSON_CreateObject();
    add_bool(root, "ok", saved_ok);
    cJSON_AddNumberToObject(root, "applied", applied);
    cJSON_AddNumberToObject(root, "llm_result", llm_err);
    cJSON_AddNumberToObject(root, "im_result", im_err);
    cJSON_AddNumberToObject(root, "asr_result", asr_err);
    cJSON_AddNumberToObject(root, "wifi_result", wifi_err);
    cJSON_AddStringToObject(root, "message", "Saved. Runtime agents are reloading the updated configuration.");
    return send_json(req, root);
}

static esp_err_t ai_bridge_config_handler(httpd_req_t *req)
{
    char body[448] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid form");
    }
    body[received] = '\0';

    ai_bridge_config_t cfg = {0};
    (void)ai_bridge_get_config(&cfg);

    char enabled[8] = {0};
    char interval[16] = {0};
    char webhook[AI_BRIDGE_WEBHOOK_URL_MAX] = {0};
    bool has_enabled = form_value(body, "enabled", enabled, sizeof(enabled));
    bool has_interval = form_value(body, "interval", interval, sizeof(interval));
    bool has_webhook = form_value(body, "webhook", webhook, sizeof(webhook));

    if (has_enabled) {
        cfg.enabled = strcmp(enabled, "1") == 0 || strcmp(enabled, "true") == 0 || strcmp(enabled, "on") == 0;
    }
    if (has_interval && interval[0]) {
        uint32_t value = (uint32_t)strtoul(interval, NULL, 10);
        if (value >= 30) {
            cfg.min_interval_sec = value;
        }
    }
    if (has_webhook && webhook[0]) {
        snprintf(cfg.feishu_webhook_url, sizeof(cfg.feishu_webhook_url), "%s", webhook);
    }
    if (cfg.enabled && cfg.feishu_webhook_url[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "webhook required when enabled");
    }

    esp_err_t err = ai_bridge_set_config(&cfg);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "result", err);
    cJSON_AddStringToObject(root, "name", esp_err_to_name(err));
    add_ai_bridge_json(root);
    return send_json(req, root);
}

static esp_err_t ai_bridge_test_handler(httpd_req_t *req)
{
    char body[192] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received > 0) {
        body[received] = '\0';
    }
    char text[128] = {0};
    (void)form_value(body, "text", text, sizeof(text));
    esp_err_t err = ai_bridge_send_test(text);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "result", err);
    cJSON_AddStringToObject(root, "name", esp_err_to_name(err));
    add_ai_bridge_json(root);
    return send_json(req, root);
}

static esp_err_t sleep_profile_handler(httpd_req_t *req)
{
    sleep_profile_state_t profile;
    portENTER_CRITICAL(&s_app.lock);
    profile = s_app.profile;
    portEXIT_CRITICAL(&s_app.lock);

    cJSON *root = cJSON_CreateObject();
    add_sleep_profile_json(root, &profile);
    return send_json(req, root);
}

static void add_tool(cJSON *tools, const char *name, const char *description, const char *params)
{
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", name);
    cJSON_AddStringToObject(tool, "description", description);
    cJSON_AddStringToObject(tool, "endpoint", "/api/claw_tool");
    cJSON_AddStringToObject(tool, "method", "POST");
    cJSON_AddStringToObject(tool, "params", params);
    cJSON_AddItemToArray(tools, tool);
}

static esp_err_t claw_tools_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *tools = cJSON_AddArrayToObject(root, "tools");
    add_tool(tools, "device.status", "Read current radar, bio, sleep score, Wi-Fi and AI bridge status.", "tool=device.status");
    add_tool(tools, "profile.summary", "Read rolling personal sleep statistics and the current plan recommendation.", "tool=profile.summary");
    add_tool(tools, "sleep_assist.start", "Start guided sleep intervention.", "tool=sleep_assist.start");
    add_tool(tools, "sleep_assist.locked", "Enter already-asleep monitoring mode for night-path testing.", "tool=sleep_assist.locked");
    add_tool(tools, "sleep_assist.stop", "Stop guided sleep intervention.", "tool=sleep_assist.stop");
    add_tool(tools, "light.red", "Set the DreamGuardian status light to red for a short manual hold.", "tool=light.red");
    add_tool(tools, "light.green", "Set the DreamGuardian status light to green for a short manual hold.", "tool=light.green");
    add_tool(tools, "light.blue", "Set the DreamGuardian status light to blue for a short manual hold.", "tool=light.blue");
    add_tool(tools, "light.yellow", "Set the DreamGuardian status light to yellow for a short manual hold.", "tool=light.yellow");
    add_tool(tools, "light.off", "Clear manual status light hold.", "tool=light.off");
    add_tool(tools, "screen.red", "Set the DreamGuardian LCD screen to solid red for a short manual hold.", "tool=screen.red");
    add_tool(tools, "screen.green", "Set the DreamGuardian LCD screen to solid green for a short manual hold.", "tool=screen.green");
    add_tool(tools, "screen.blue", "Set the DreamGuardian LCD screen to solid blue for a short manual hold.", "tool=screen.blue");
    add_tool(tools, "screen.yellow", "Set the DreamGuardian LCD screen to solid yellow for a short manual hold.", "tool=screen.yellow");
    add_tool(tools, "screen.off", "Clear manual LCD screen color hold and return to clock mode.", "tool=screen.off");
    add_tool(tools, "audio.music", "Play the currently saved DreamGuardian sleep music scene from the speaker.", "tool=audio.music");
    add_tool(tools, "audio.noise", "Play /spiffs/noise.wav when present; otherwise play generated low-volume pink noise.", "tool=audio.noise");
    add_tool(tools, "audio.breathing", "Play /spiffs/breathing.wav when present; otherwise play a stereo breathing guidance tone.", "tool=audio.breathing");
    add_tool(tools, "audio.volume_max", "Set current and future playback volume to maximum.", "tool=audio.volume_max");
    add_tool(tools, "audio.volume_up", "Increase current and future playback volume.", "tool=audio.volume_up");
    add_tool(tools, "audio.volume_down", "Decrease current and future playback volume.", "tool=audio.volume_down");
    add_tool(tools, "audio.mute", "Mute current audio playback.", "tool=audio.mute");
    add_tool(tools, "story.play", "Play /spiffs/story.wav when present; otherwise start soft background audio while the assistant replies with story text.", "tool=story.play");
    add_tool(tools, "wake.ack", "Play the local wake acknowledgement and flash the screen/light briefly.", "tool=wake.ack");
    add_tool(tools, "scene.ppm", "Use the default rhythm scene: pad, piano, stream, birds, chimes and light tide.", "tool=scene.ppm");
    add_tool(tools, "scene.ocean", "Use the deep ocean tide sleep scene.", "tool=scene.ocean");
    add_tool(tools, "scene.forest", "Use the forest morning scene with stream, wind, birds and chimes.", "tool=scene.forest");
    add_tool(tools, "scene.rain", "Use the rainy night scene with rain, soft pad and low stream.", "tool=scene.rain");
    add_tool(tools, "scene.zen", "Use the quiet meditation scene with pad, sparse piano and chimes.", "tool=scene.zen");
    add_tool(tools, "scene.ocean.play", "Select and immediately play the deep ocean tide scene from the speaker.", "tool=scene.ocean.play");
    add_tool(tools, "scene.forest.play", "Select and immediately play the forest morning scene from the speaker.", "tool=scene.forest.play");
    add_tool(tools, "scene.rain.play", "Select and immediately play the rainy night scene from the speaker.", "tool=scene.rain.play");
    add_tool(tools, "scene.zen.play", "Select and immediately play the quiet meditation scene from the speaker.", "tool=scene.zen.play");
    add_tool(tools, "breath.relax", "Use relaxed 5s inhale / 5s exhale guidance, around 6 breaths per minute.", "tool=breath.relax");
    add_tool(tools, "breath.box", "Use box breathing guidance, mapped to around 7.5 breaths per minute.", "tool=breath.box");
    add_tool(tools, "breath.deep", "Use deep sleep 4-7-8 style guidance, mapped to a slow 4 breaths per minute target.", "tool=breath.deep");
    add_tool(tools, "mid_sleep.test_minor", "Simulate a minor in-sleep disturbance and run the masking intervention.", "tool=mid_sleep.test_minor");
    add_tool(tools, "mid_sleep.test_restless", "Simulate restless sleep or turning and run breathing/light intervention.", "tool=mid_sleep.test_restless");
    add_tool(tools, "mid_sleep.test_arousal", "Simulate high wake risk or dreaming and run low-stimulus comfort intervention.", "tool=mid_sleep.test_arousal");
    add_tool(tools, "mid_sleep.test_out_of_bed", "Simulate an out-of-bed event and run night path lighting.", "tool=mid_sleep.test_out_of_bed");
    cJSON_AddStringToObject(root, "audio_upload", "POST raw WAV data to /api/audio_upload?name=music.wav; supported names: music.wav, story.wav, noise.wav, breathing.wav, zai_ne.wav, default.wav, ocean.wav, forest.wav, rain.wav, zen.wav.");
    cJSON_AddStringToObject(root, "note", "Feishu custom webhooks are outbound only; inbound group commands need the Feishu app WebSocket agent.");
    return send_json(req, root);
}

static mobile_app_command_t tool_to_command(const char *tool)
{
    if (!tool) {
        return MOBILE_APP_COMMAND_NONE;
    }
    if (strcmp(tool, "sleep_assist.start") == 0 || strcmp(tool, "sleep.start") == 0) {
        return MOBILE_APP_COMMAND_SLEEP;
    }
    if (strcmp(tool, "sleep_assist.locked") == 0 || strcmp(tool, "sleep.locked") == 0 ||
        strcmp(tool, "sleep.slept") == 0) {
        return MOBILE_APP_COMMAND_SLEPT;
    }
    if (strcmp(tool, "sleep_assist.stop") == 0 || strcmp(tool, "sleep.stop") == 0) {
        return MOBILE_APP_COMMAND_STOP;
    }
    if (strcmp(tool, "light.red") == 0) {
        return MOBILE_APP_COMMAND_LIGHT_RED;
    }
    if (strcmp(tool, "light.green") == 0) {
        return MOBILE_APP_COMMAND_LIGHT_GREEN;
    }
    if (strcmp(tool, "light.blue") == 0) {
        return MOBILE_APP_COMMAND_LIGHT_BLUE;
    }
    if (strcmp(tool, "light.yellow") == 0) {
        return MOBILE_APP_COMMAND_LIGHT_YELLOW;
    }
    if (strcmp(tool, "light.off") == 0) {
        return MOBILE_APP_COMMAND_LIGHT_OFF;
    }
    if (strcmp(tool, "screen.red") == 0) {
        return MOBILE_APP_COMMAND_SCREEN_RED;
    }
    if (strcmp(tool, "screen.green") == 0) {
        return MOBILE_APP_COMMAND_SCREEN_GREEN;
    }
    if (strcmp(tool, "screen.blue") == 0) {
        return MOBILE_APP_COMMAND_SCREEN_BLUE;
    }
    if (strcmp(tool, "screen.yellow") == 0) {
        return MOBILE_APP_COMMAND_SCREEN_YELLOW;
    }
    if (strcmp(tool, "screen.off") == 0) {
        return MOBILE_APP_COMMAND_SCREEN_OFF;
    }
    if (strcmp(tool, "audio.music") == 0 || strcmp(tool, "music.play") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_MUSIC;
    }
    if (strcmp(tool, "audio.noise") == 0 || strcmp(tool, "audio.pink_noise") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_NOISE;
    }
    if (strcmp(tool, "audio.breathing") == 0 || strcmp(tool, "audio.stereo_breathing") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_BREATHING;
    }
    if (strcmp(tool, "audio.volume_max") == 0 || strcmp(tool, "volume.max") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_VOLUME_MAX;
    }
    if (strcmp(tool, "audio.volume_up") == 0 || strcmp(tool, "volume.up") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_VOLUME_UP;
    }
    if (strcmp(tool, "audio.volume_down") == 0 || strcmp(tool, "volume.down") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_VOLUME_DOWN;
    }
    if (strcmp(tool, "audio.mute") == 0 || strcmp(tool, "mute") == 0) {
        return MOBILE_APP_COMMAND_AUDIO_MUTE;
    }
    if (strcmp(tool, "story.play") == 0 || strcmp(tool, "story") == 0) {
        return MOBILE_APP_COMMAND_STORY;
    }
    if (strcmp(tool, "wake.ack") == 0 || strcmp(tool, "voice.wake_ack") == 0) {
        return MOBILE_APP_COMMAND_WAKE_ACK;
    }
    if (strcmp(tool, "scene.ppm") == 0 || strcmp(tool, "scene.ppm3104e") == 0) {
        return MOBILE_APP_COMMAND_SCENE_PPM;
    }
    if (strcmp(tool, "scene.ppm.play") == 0 || strcmp(tool, "scene.default.play") == 0) {
        return MOBILE_APP_COMMAND_SCENE_PPM_PLAY;
    }
    if (strcmp(tool, "scene.ocean") == 0 || strcmp(tool, "scene.tide") == 0) {
        return MOBILE_APP_COMMAND_SCENE_OCEAN;
    }
    if (strcmp(tool, "scene.ocean.play") == 0 || strcmp(tool, "scene.tide.play") == 0) {
        return MOBILE_APP_COMMAND_SCENE_OCEAN_PLAY;
    }
    if (strcmp(tool, "scene.forest") == 0) {
        return MOBILE_APP_COMMAND_SCENE_FOREST;
    }
    if (strcmp(tool, "scene.forest.play") == 0) {
        return MOBILE_APP_COMMAND_SCENE_FOREST_PLAY;
    }
    if (strcmp(tool, "scene.rain") == 0 || strcmp(tool, "scene.rainy") == 0) {
        return MOBILE_APP_COMMAND_SCENE_RAIN;
    }
    if (strcmp(tool, "scene.rain.play") == 0 || strcmp(tool, "scene.rainy.play") == 0) {
        return MOBILE_APP_COMMAND_SCENE_RAIN_PLAY;
    }
    if (strcmp(tool, "scene.zen") == 0 || strcmp(tool, "scene.meditation") == 0) {
        return MOBILE_APP_COMMAND_SCENE_ZEN;
    }
    if (strcmp(tool, "scene.zen.play") == 0 || strcmp(tool, "scene.meditation.play") == 0) {
        return MOBILE_APP_COMMAND_SCENE_ZEN_PLAY;
    }
    if (strcmp(tool, "scene.empty") == 0 || strcmp(tool, "scene.custom") == 0) {
        return MOBILE_APP_COMMAND_SCENE_EMPTY;
    }
    if (strcmp(tool, "breath.relax") == 0 || strcmp(tool, "breath.5-5") == 0) {
        return MOBILE_APP_COMMAND_BREATH_RELAX;
    }
    if (strcmp(tool, "breath.box") == 0 || strcmp(tool, "breath.4-4") == 0) {
        return MOBILE_APP_COMMAND_BREATH_BOX;
    }
    if (strcmp(tool, "breath.deep") == 0 || strcmp(tool, "breath.4-7-8") == 0) {
        return MOBILE_APP_COMMAND_BREATH_DEEP;
    }
    if (strcmp(tool, "mid_sleep.test_minor") == 0 || strcmp(tool, "intervention.test_minor") == 0) {
        return MOBILE_APP_COMMAND_MID_SLEEP_TEST_MINOR;
    }
    if (strcmp(tool, "mid_sleep.test_restless") == 0 || strcmp(tool, "intervention.test_restless") == 0) {
        return MOBILE_APP_COMMAND_MID_SLEEP_TEST_RESTLESS;
    }
    if (strcmp(tool, "mid_sleep.test_arousal") == 0 || strcmp(tool, "intervention.test_arousal") == 0) {
        return MOBILE_APP_COMMAND_MID_SLEEP_TEST_AROUSAL;
    }
    if (strcmp(tool, "mid_sleep.test_out_of_bed") == 0 || strcmp(tool, "intervention.test_out_of_bed") == 0) {
        return MOBILE_APP_COMMAND_MID_SLEEP_TEST_OUT_OF_BED;
    }
    return MOBILE_APP_COMMAND_NONE;
}

static bool command_is_scene_tool(mobile_app_command_t command)
{
    return command == MOBILE_APP_COMMAND_SCENE_PPM ||
           command == MOBILE_APP_COMMAND_SCENE_OCEAN ||
           command == MOBILE_APP_COMMAND_SCENE_FOREST ||
           command == MOBILE_APP_COMMAND_SCENE_RAIN ||
           command == MOBILE_APP_COMMAND_SCENE_ZEN ||
           command == MOBILE_APP_COMMAND_SCENE_EMPTY ||
           command == MOBILE_APP_COMMAND_SCENE_PPM_PLAY ||
           command == MOBILE_APP_COMMAND_SCENE_OCEAN_PLAY ||
           command == MOBILE_APP_COMMAND_SCENE_FOREST_PLAY ||
           command == MOBILE_APP_COMMAND_SCENE_RAIN_PLAY ||
           command == MOBILE_APP_COMMAND_SCENE_ZEN_PLAY;
}

static esp_err_t claw_tool_handler(httpd_req_t *req)
{
    char body[192] = {0};
    char tool[48] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received > 0) {
        body[received] = '\0';
        (void)form_value(body, "tool", tool, sizeof(tool));
    }
    if (tool[0] == '\0') {
        char query[96] = {0};
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            (void)httpd_query_key_value(query, "tool", tool, sizeof(tool));
        }
    }

    if (strcmp(tool, "device.status") == 0) {
        return status_handler(req);
    }
    if (strcmp(tool, "profile.summary") == 0 || strcmp(tool, "report.sleep_plan") == 0) {
        return sleep_profile_handler(req);
    }
    if (strcmp(tool, "report.demo_seed") == 0) {
        bool ok = sleep_log_seed_demo_history(7);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "tool", tool);
        cJSON_AddBoolToObject(root, "ok", ok);
        cJSON_AddNumberToObject(root, "days", ok ? 7 : 0);
        cJSON_AddStringToObject(root, "name", ok ? "demo_history_seeded" : "demo_history_seed_failed");
        return send_json(req, root);
    }

    mobile_app_command_t command = tool_to_command(tool);
    esp_err_t cb_err = ESP_ERR_NOT_SUPPORTED;
    if (command != MOBILE_APP_COMMAND_NONE && s_app.cfg.command_cb) {
        cb_err = s_app.cfg.command_cb(command, s_app.cfg.command_ctx);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "tool", tool);
    cJSON_AddNumberToObject(root, "command", command);
    cJSON_AddNumberToObject(root, "result", cb_err);
    cJSON_AddStringToObject(root, "name", command == MOBILE_APP_COMMAND_NONE ? "unsupported_tool" : esp_err_to_name(cb_err));
    return send_json(req, root);
}

static bool audio_name_allowed(const char *name)
{
    return name &&
           (strcmp(name, "music.wav") == 0 ||
            strcmp(name, "story.wav") == 0 ||
            strcmp(name, "noise.wav") == 0 ||
            strcmp(name, "breathing.wav") == 0 ||
            strcmp(name, "zai_ne.wav") == 0 ||
            strcmp(name, "default.wav") == 0 ||
            strcmp(name, "ocean.wav") == 0 ||
            strcmp(name, "forest.wav") == 0 ||
            strcmp(name, "rain.wav") == 0 ||
            strcmp(name, "zen.wav") == 0);
}

static void audio_path_for_name(const char *name, char *path, size_t path_size)
{
    snprintf(path, path_size, "/spiffs/%s", name);
}

static uint16_t audio_read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t audio_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool audio_probe_wav(const char *path, uint16_t *channels, uint32_t *sample_rate,
                            uint16_t *bits, uint32_t *data_bytes, float *duration_sec)
{
    if (channels) *channels = 0;
    if (sample_rate) *sample_rate = 0;
    if (bits) *bits = 0;
    if (data_bytes) *data_bytes = 0;
    if (duration_sec) *duration_sec = 0.0f;

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

    uint16_t ch = 0;
    uint32_t rate = 0;
    uint16_t bps = 0;
    uint32_t data = 0;
    while (!feof(file)) {
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) {
            break;
        }
        uint32_t size = audio_read_le32(chunk + 4);
        long chunk_data = ftell(file);
        if (chunk_data < 0) {
            break;
        }
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[32] = {0};
            size_t n = size < sizeof(fmt) ? size : sizeof(fmt);
            if (fread(fmt, 1, n, file) == n && n >= 16) {
                ch = audio_read_le16(fmt + 2);
                rate = audio_read_le32(fmt + 4);
                bps = audio_read_le16(fmt + 14);
            }
        } else if (memcmp(chunk, "data", 4) == 0) {
            data = size;
        }
        long next = chunk_data + (long)size + (long)(size & 1U);
        if (fseek(file, next, SEEK_SET) != 0) {
            break;
        }
        if (ch && rate && bps && data) {
            break;
        }
    }
    fclose(file);

    if (!ch || !rate || !bps || !data) {
        return false;
    }
    if (channels) *channels = ch;
    if (sample_rate) *sample_rate = rate;
    if (bits) *bits = bps;
    if (data_bytes) *data_bytes = data;
    if (duration_sec) {
        uint32_t bytes_per_frame = (uint32_t)ch * ((uint32_t)bps / 8U);
        *duration_sec = bytes_per_frame ? (float)data / (float)(bytes_per_frame * rate) : 0.0f;
    }
    return true;
}

static void add_audio_file_json(cJSON *files, const char *name)
{
    char path[64];
    audio_path_for_name(name, path, sizeof(path));
    size_t bytes = 0;
    bool present = false;
    struct stat st = {0};
    if (stat(path, &st) == 0) {
        present = true;
        bytes = (size_t)st.st_size;
    }

    cJSON *file = cJSON_CreateObject();
    cJSON_AddStringToObject(file, "name", name);
    cJSON_AddStringToObject(file, "path", path);
    add_bool(file, "present", present);
    cJSON_AddNumberToObject(file, "bytes", bytes);
    if (present) {
        uint16_t channels = 0;
        uint16_t bits = 0;
        uint32_t sample_rate = 0;
        uint32_t data_bytes = 0;
        float duration = 0.0f;
        if (audio_probe_wav(path, &channels, &sample_rate, &bits, &data_bytes, &duration)) {
            cJSON_AddNumberToObject(file, "sample_rate_hz", sample_rate);
            cJSON_AddNumberToObject(file, "channels", channels);
            cJSON_AddNumberToObject(file, "bits_per_sample", bits);
            cJSON_AddNumberToObject(file, "data_bytes", data_bytes);
            add_number_safe(file, "duration_sec", duration);
        } else {
            cJSON_AddStringToObject(file, "format_status", "wav_probe_failed");
        }
    }
    cJSON_AddItemToArray(files, file);
}

static esp_err_t audio_files_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    size_t total = 0;
    size_t used = 0;
    esp_err_t info_err = esp_spiffs_info("storage", &total, &used);
    cJSON_AddNumberToObject(root, "spiffs_result", info_err);
    cJSON_AddStringToObject(root, "spiffs_name", esp_err_to_name(info_err));
    cJSON_AddNumberToObject(root, "total", total);
    cJSON_AddNumberToObject(root, "used", used);
    cJSON *files = cJSON_AddArrayToObject(root, "files");
    add_audio_file_json(files, "music.wav");
    add_audio_file_json(files, "story.wav");
    add_audio_file_json(files, "noise.wav");
    add_audio_file_json(files, "breathing.wav");
    add_audio_file_json(files, "zai_ne.wav");
    add_audio_file_json(files, "default.wav");
    add_audio_file_json(files, "ocean.wav");
    add_audio_file_json(files, "forest.wav");
    add_audio_file_json(files, "rain.wav");
    add_audio_file_json(files, "zen.wav");
    cJSON_AddStringToObject(root, "note", "File presence is read directly from SPIFFS. Use pre-rendered WAV files for final sleep music quality.");
    cJSON_AddStringToObject(root, "format", "WAV PCM 16-bit, 8000-48000 Hz, mono or stereo");
    return send_json(req, root);
}

static esp_err_t audio_upload_handler(httpd_req_t *req)
{
    char query[96] = {0};
    char name[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "name", name, sizeof(name));
    }
    if (!audio_name_allowed(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid audio name");
    }
    if (req->content_len <= 0 || req->content_len > AUDIO_UPLOAD_MAX_BYTES) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid audio size");
    }

    char path[64];
    char tmp_path[64];
    audio_path_for_name(name, path, sizeof(path));
    snprintf(tmp_path, sizeof(tmp_path), "/spiffs/.upload.tmp");
    (void)remove(tmp_path);

    size_t fs_total = 0;
    size_t fs_used = 0;
    struct stat old_st = {0};
    bool target_exists = stat(path, &old_st) == 0;
    if (esp_spiffs_info("storage", &fs_total, &fs_used) == ESP_OK) {
        size_t free_bytes = fs_total > fs_used ? fs_total - fs_used : 0;
        if (target_exists && free_bytes < (size_t)req->content_len) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "not enough free space for safe replace");
        }
    }

    FILE *file = fopen(tmp_path, "wb");
    if (!file) {
        ESP_LOGW(TAG, "audio upload open failed path=%s errno=%d", tmp_path, errno);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    }

    char buffer[1024];
    size_t total = 0;
    while (total < req->content_len) {
        size_t remaining = req->content_len - total;
        int to_read = remaining < sizeof(buffer) ? (int)remaining : (int)sizeof(buffer);
        int received = httpd_req_recv(req, buffer, to_read);
        if (received <= 0) {
            fclose(file);
            remove(tmp_path);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "receive failed");
        }
        if (fwrite(buffer, 1, (size_t)received, file) != (size_t)received) {
            fclose(file);
            remove(tmp_path);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        }
        total += (size_t)received;
    }
    if (fclose(file) != 0) {
        remove(tmp_path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "close failed");
    }

    if (target_exists) {
        (void)remove(path);
    }
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        ESP_LOGW(TAG, "audio upload rename failed tmp=%s path=%s errno=%d",
                 tmp_path, path, errno);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "path", path);
    cJSON_AddNumberToObject(root, "bytes", total);
    cJSON_AddBoolToObject(root, "replaced_existing", target_exists);
    cJSON_AddStringToObject(root, "format_required", "WAV PCM 16-bit, 8000-48000 Hz, mono or stereo");
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t sample_rate = 0;
    uint32_t data_bytes = 0;
    float duration = 0.0f;
    if (audio_probe_wav(path, &channels, &sample_rate, &bits, &data_bytes, &duration)) {
        cJSON_AddNumberToObject(root, "sample_rate_hz", sample_rate);
        cJSON_AddNumberToObject(root, "channels", channels);
        cJSON_AddNumberToObject(root, "bits_per_sample", bits);
        cJSON_AddNumberToObject(root, "data_bytes", data_bytes);
        add_number_safe(root, "duration_sec", duration);
    }
    return send_json(req, root);
}

static void wake_sample_name(int index, char *name, size_t name_size)
{
    snprintf(name, name_size, "wake_xm_%02d.wav", index);
}

static void wake_sample_path(int index, char *path, size_t path_size)
{
    char name[24];
    wake_sample_name(index, name, sizeof(name));
    snprintf(path, path_size, "/spiffs/%s", name);
}

static void wake_phrase_load(char *phrase, size_t phrase_size)
{
    if (!phrase || phrase_size == 0) {
        return;
    }
    snprintf(phrase, phrase_size, "%s", "hi 小梦");
    nvs_handle_t nvs;
    if (nvs_open(WAKE_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = phrase_size;
        (void)nvs_get_str(nvs, WAKE_NVS_PHRASE, phrase, &len);
        nvs_close(nvs);
    }
}

static esp_err_t wake_phrase_save(const char *phrase)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WAKE_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open wake nvs");
    esp_err_t err = nvs_set_str(nvs, WAKE_NVS_PHRASE, phrase && phrase[0] ? phrase : "hi 小梦");
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void wake_upload_stats_load(uint32_t *count, char *last_name, size_t last_name_size,
                                   uint32_t *last_bytes)
{
    if (count) {
        *count = 0;
    }
    if (last_name && last_name_size > 0) {
        last_name[0] = '\0';
    }
    if (last_bytes) {
        *last_bytes = 0;
    }

    nvs_handle_t nvs;
    if (nvs_open(WAKE_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    if (count) {
        (void)nvs_get_u32(nvs, WAKE_NVS_UPLOAD_COUNT, count);
    }
    if (last_name && last_name_size > 0) {
        size_t len = last_name_size;
        (void)nvs_get_str(nvs, WAKE_NVS_LAST_NAME, last_name, &len);
    }
    if (last_bytes) {
        (void)nvs_get_u32(nvs, WAKE_NVS_LAST_BYTES, last_bytes);
    }
    nvs_close(nvs);
}

static esp_err_t wake_upload_stats_save(const char *name, uint32_t bytes)
{
    uint32_t count = 0;
    wake_upload_stats_load(&count, NULL, 0, NULL);
    if (count < WAKE_SAMPLE_MAX_COUNT) {
        count++;
    }

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WAKE_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open wake stats nvs");
    esp_err_t err = nvs_set_u32(nvs, WAKE_NVS_UPLOAD_COUNT, count);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WAKE_NVS_LAST_NAME, name ? name : "");
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, WAKE_NVS_LAST_BYTES, bytes);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static int wake_next_sample_index(void)
{
    for (int i = 1; i <= WAKE_SAMPLE_MAX_COUNT; ++i) {
        char path[64];
        struct stat st = {0};
        wake_sample_path(i, path, sizeof(path));
        if (stat(path, &st) != 0) {
            return i;
        }
    }
    return 1;
}

static esp_err_t wake_samples_handler(httpd_req_t *req)
{
    char phrase[64];
    wake_phrase_load(phrase, sizeof(phrase));
    voice_sr_status_t sr = {0};
    voice_sr_get_status(&sr);
    uint32_t upload_count = 0;
    uint32_t last_bytes = 0;
    char last_name[24] = {0};
    wake_upload_stats_load(&upload_count, last_name, sizeof(last_name), &last_bytes);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "target_phrase", phrase);
    cJSON_AddStringToObject(root, "runtime_wake_word",
                            sr.template_experiment_enabled ? "Hi ESP + hi xiaomeng templates" : "Hi ESP");
    cJSON_AddStringToObject(root, "active_model", sr.wake_model[0] ? sr.wake_model : "not_loaded");
    add_bool(root, "wakenet_loaded", sr.wakenet_loaded);
    add_bool(root, "template_experiment_enabled", sr.template_experiment_enabled);
    add_bool(root, "template_learning", sr.template_learning);
    cJSON_AddNumberToObject(root, "template_count", sr.template_count);
    cJSON_AddStringToObject(root, "template_source", sr.template_source);
    cJSON_AddNumberToObject(root, "template_learn_count", sr.template_learn_count);
    cJSON_AddNumberToObject(root, "template_learn_target", sr.template_learn_target);
    cJSON_AddNumberToObject(root, "template_threshold", sr.template_threshold);
    cJSON_AddNumberToObject(root, "template_confirm_threshold", sr.template_confirm_threshold);
    cJSON_AddNumberToObject(root, "template_min_confirmations", sr.template_min_confirmations);
    cJSON_AddNumberToObject(root, "template_best_score", sr.template_best_score);
    cJSON_AddStringToObject(root, "template_best_name", sr.template_best_name);
    cJSON_AddNumberToObject(root, "sample_count", upload_count);
    cJSON_AddNumberToObject(root, "sample_target", WAKE_SAMPLE_MAX_COUNT);
    cJSON_AddStringToObject(root, "sample_prefix", "wake_xm_");
    cJSON_AddStringToObject(root, "last_sample_name", last_name);
    cJSON_AddNumberToObject(root, "last_sample_bytes", last_bytes);
    cJSON_AddStringToObject(root, "note", "Samples are for custom WakeNet training. They do not change the active wake model until a trained model partition is flashed.");
    cJSON *samples = cJSON_AddArrayToObject(root, "samples");
    for (int i = 1; i <= WAKE_SAMPLE_MAX_COUNT; ++i) {
        char name[24];
        char path[64];
        char url[48];
        struct stat st = {0};
        wake_sample_name(i, name, sizeof(name));
        wake_sample_path(i, path, sizeof(path));
        snprintf(url, sizeof(url), "/api/wake_sample?index=%d", i);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "name", name);
        cJSON_AddStringToObject(item, "path", path);
        cJSON_AddStringToObject(item, "download_url", url);
        add_bool(item, "present", stat(path, &st) == 0);
        cJSON_AddNumberToObject(item, "bytes", st.st_size);
        cJSON_AddItemToArray(samples, item);
    }
    return send_json(req, root);
}

static esp_err_t wake_sample_download_handler(httpd_req_t *req)
{
    char query[64] = {0};
    char index_text[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "index", index_text, sizeof(index_text)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing index");
    }

    int index = atoi(index_text);
    if (index < 1 || index > WAKE_SAMPLE_MAX_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid index");
    }

    char name[24];
    char path[64];
    wake_sample_name(index, name, sizeof(name));
    wake_sample_path(index, path, sizeof(path));

    FILE *file = fopen(path, "rb");
    if (!file) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "sample not found");
    }

    prepare_http_response(req, "audio/wav");
    char disposition[80];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    char buffer[1024];
    esp_err_t err = ESP_OK;
    while (!feof(file)) {
        size_t n = fread(buffer, 1, sizeof(buffer), file);
        if (n > 0) {
            err = httpd_resp_send_chunk(req, buffer, (ssize_t)n);
            if (err != ESP_OK) {
                break;
            }
        }
        if (ferror(file)) {
            err = ESP_FAIL;
            break;
        }
    }
    fclose(file);
    if (err != ESP_OK) {
        close_http_session(req);
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool debug_audio_name_allowed(const char *name)
{
    if (!name || strncmp(name, "capture_", 8) != 0) {
        return false;
    }
    size_t len = strlen(name);
    bool wav = len >= 4 && strcmp(name + len - 4, ".wav") == 0;
    bool csv = len >= 4 && strcmp(name + len - 4, ".csv") == 0;
    if (len < 13 || len > 90 || (!wav && !csv)) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            continue;
        }
        return false;
    }
    return strstr(name, "..") == NULL;
}

static esp_err_t debug_audio_file_handler(httpd_req_t *req)
{
    char query[128] = {0};
    char name[96] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
        !debug_audio_name_allowed(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
    }

    char path[112];
    snprintf(path, sizeof(path), "/spiffs/%s", name);
    FILE *file = fopen(path, "rb");
    if (!file) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "capture not found");
    }

    prepare_http_response(req, strstr(name, ".csv") ? "text/csv" : "audio/wav");
    char disposition[128];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    char buffer[1024];
    esp_err_t err = ESP_OK;
    while (!feof(file)) {
        size_t n = fread(buffer, 1, sizeof(buffer), file);
        if (n > 0) {
            err = httpd_resp_send_chunk(req, buffer, (ssize_t)n);
            if (err != ESP_OK) {
                break;
            }
        }
        if (ferror(file)) {
            err = ESP_FAIL;
            break;
        }
    }
    fclose(file);
    if (err != ESP_OK) {
        close_http_session(req);
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t debug_audio_capture_handler(httpd_req_t *req)
{
    char body[160] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid form");
    }
    body[received] = '\0';

    char label[80] = {0};
    char seconds_text[8] = {0};
    if (!form_value(body, "label", label, sizeof(label)) || !label[0]) {
        snprintf(label, sizeof(label), "capture");
    }
    uint32_t seconds = 5;
    if (form_value(body, "seconds", seconds_text, sizeof(seconds_text)) && seconds_text[0]) {
        seconds = (uint32_t)atoi(seconds_text);
    }

    esp_err_t err = voice_sr_debug_capture_start(label, seconds);
    voice_sr_status_t status = {0};
    voice_sr_get_status(&status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "result", err);
    cJSON_AddStringToObject(root, "name", esp_err_to_name(err));
    add_bool(root, "enabled", status.debug_capture_enabled);
    add_bool(root, "active", status.debug_capture_active);
    add_bool(root, "write_pending", status.debug_capture_write_pending);
    cJSON_AddStringToObject(root, "label", status.debug_capture_label);
    cJSON_AddNumberToObject(root, "seconds", status.debug_capture_seconds);
    cJSON_AddStringToObject(root, "raw_path", status.debug_capture_raw_path);
    cJSON_AddStringToObject(root, "afe_path", status.debug_capture_afe_path);
    cJSON_AddStringToObject(root, "score_path", status.debug_capture_score_path);
    const char *raw_name = strrchr(status.debug_capture_raw_path, '/');
    const char *afe_name = strrchr(status.debug_capture_afe_path, '/');
    raw_name = raw_name ? raw_name + 1 : status.debug_capture_raw_path;
    afe_name = afe_name ? afe_name + 1 : status.debug_capture_afe_path;
    char raw_url[128] = {0};
    char afe_url[128] = {0};
    char score_url[128] = {0};
    if (raw_name[0]) {
        snprintf(raw_url, sizeof(raw_url), "/api/debug_audio_file?name=%s", raw_name);
    }
    if (afe_name[0]) {
        snprintf(afe_url, sizeof(afe_url), "/api/debug_audio_file?name=%s", afe_name);
    }
    const char *score_name = strrchr(status.debug_capture_score_path, '/');
    score_name = score_name ? score_name + 1 : status.debug_capture_score_path;
    if (score_name[0]) {
        snprintf(score_url, sizeof(score_url), "/api/debug_audio_file?name=%s", score_name);
    }
    cJSON_AddStringToObject(root, "raw_download", raw_url);
    cJSON_AddStringToObject(root, "afe_download", afe_url);
    cJSON_AddStringToObject(root, "score_download", score_url);
    cJSON_AddStringToObject(root, "note", "Download files after debug_capture_active=false and debug_capture_write_pending=false.");
    return send_json(req, root);
}

static esp_err_t wake_word_config_handler(httpd_req_t *req)
{
    char body[160] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid form");
    }
    body[received] = '\0';

    char phrase[64] = {0};
    if (!form_value(body, "phrase", phrase, sizeof(phrase)) || !phrase[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing phrase");
    }
    esp_err_t err = wake_phrase_save(phrase);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "target_phrase", phrase);
    cJSON_AddNumberToObject(root, "result", err);
    cJSON_AddStringToObject(root, "name", esp_err_to_name(err));
    cJSON_AddStringToObject(root, "runtime_wake_word", "Hi ESP");
    cJSON_AddStringToObject(root, "note", "Target phrase saved for sample collection; active WakeNet model is unchanged.");
    return send_json(req, root);
}

static esp_err_t wake_template_handler(httpd_req_t *req)
{
    char body[192] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid form");
    }
    body[received] = '\0';

    char enabled_text[8] = {0};
    char duration_text[12] = {0};
    char threshold_text[16] = {0};
    char confirm_text[16] = {0};
    char min_text[8] = {0};
    char learn_text[8] = {0};
    bool enabled = false;
    bool has_enabled = false;
    uint32_t duration = 120;
    if (form_value(body, "enabled", enabled_text, sizeof(enabled_text))) {
        has_enabled = true;
        enabled = enabled_text[0] == '1' || strcasecmp(enabled_text, "true") == 0 ||
                  strcasecmp(enabled_text, "on") == 0;
    }
    if (form_value(body, "duration", duration_text, sizeof(duration_text)) && duration_text[0]) {
        duration = (uint32_t)atoi(duration_text);
    }

    esp_err_t cfg_err = ESP_OK;
    bool has_threshold = form_value(body, "threshold", threshold_text, sizeof(threshold_text)) && threshold_text[0];
    bool has_confirm = form_value(body, "confirm", confirm_text, sizeof(confirm_text)) && confirm_text[0];
    bool has_min = form_value(body, "min", min_text, sizeof(min_text)) && min_text[0];
    bool has_learn = form_value(body, "learn", learn_text, sizeof(learn_text)) && learn_text[0];
    if (has_threshold || has_confirm || has_min) {
        voice_sr_status_t before = {0};
        voice_sr_get_status(&before);
        float threshold = has_threshold ? (float)atof(threshold_text) : before.template_threshold;
        float confirm = has_confirm ? (float)atof(confirm_text) : before.template_confirm_threshold;
        uint32_t min_confirmations = has_min ? (uint32_t)atoi(min_text) : before.template_min_confirmations;
        cfg_err = voice_sr_template_match_configure(threshold, confirm, min_confirmations);
    }

    esp_err_t err = has_enabled ? voice_sr_template_experiment_enable(enabled, duration) : ESP_OK;
    esp_err_t learn_err = ESP_OK;
    if (has_learn) {
        uint32_t target = (uint32_t)atoi(learn_text);
        learn_err = voice_sr_template_learning_start(target);
    }
    voice_sr_status_t status = {0};
    voice_sr_get_status(&status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "result", err);
    cJSON_AddStringToObject(root, "name", esp_err_to_name(err));
    cJSON_AddNumberToObject(root, "config_result", cfg_err);
    cJSON_AddStringToObject(root, "config_name", esp_err_to_name(cfg_err));
    cJSON_AddNumberToObject(root, "learn_result", learn_err);
    cJSON_AddStringToObject(root, "learn_name", esp_err_to_name(learn_err));
    add_bool(root, "enabled", status.template_experiment_enabled);
    add_bool(root, "learning", status.template_learning);
    cJSON_AddNumberToObject(root, "template_count", status.template_count);
    cJSON_AddStringToObject(root, "template_source", status.template_source);
    cJSON_AddNumberToObject(root, "template_learn_count", status.template_learn_count);
    cJSON_AddNumberToObject(root, "template_learn_target", status.template_learn_target);
    cJSON_AddNumberToObject(root, "template_threshold", status.template_threshold);
    cJSON_AddNumberToObject(root, "template_confirm_threshold", status.template_confirm_threshold);
    cJSON_AddNumberToObject(root, "template_min_confirmations", status.template_min_confirmations);
    cJSON_AddStringToObject(root, "last_result", status.last_result);
    cJSON_AddStringToObject(root, "note", "Local template wake can be enabled/disabled; threshold, confirm, and min are saved.");
    return send_json(req, root);
}

static esp_err_t wake_sample_upload_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > WAKE_SAMPLE_MAX_BYTES) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid sample size");
    }

    char query[64] = {0};
    char index_text[8] = {0};
    int index = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "index", index_text, sizeof(index_text)) == ESP_OK) {
        index = atoi(index_text);
    }
    if (index < 1 || index > WAKE_SAMPLE_MAX_COUNT) {
        index = wake_next_sample_index();
    }

    char path[64];
    char tmp_path[64];
    wake_sample_path(index, path, sizeof(path));
    snprintf(tmp_path, sizeof(tmp_path), "/spiffs/.wake_upload.tmp");
    (void)remove(tmp_path);

    FILE *file = fopen(tmp_path, "wb");
    if (!file) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    }

    char buffer[1024];
    size_t total = 0;
    while (total < req->content_len) {
        size_t remaining = req->content_len - total;
        int to_read = remaining < sizeof(buffer) ? (int)remaining : (int)sizeof(buffer);
        int received = httpd_req_recv(req, buffer, to_read);
        if (received <= 0) {
            fclose(file);
            remove(tmp_path);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "receive failed");
        }
        if (fwrite(buffer, 1, (size_t)received, file) != (size_t)received) {
            fclose(file);
            remove(tmp_path);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        }
        total += (size_t)received;
    }
    if (fclose(file) != 0) {
        remove(tmp_path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "close failed");
    }
    (void)remove(path);
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
    }

    char name[24];
    wake_sample_name(index, name, sizeof(name));
    (void)wake_upload_stats_save(name, (uint32_t)total);
    uint32_t upload_count = 0;
    wake_upload_stats_load(&upload_count, NULL, 0, NULL);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "index", index);
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "path", path);
    cJSON_AddNumberToObject(root, "bytes", total);
    cJSON_AddNumberToObject(root, "sample_count", upload_count);
    cJSON_AddStringToObject(root, "format_recommended", "WAV PCM 16-bit mono 16kHz, 1-3 seconds");
    return send_json(req, root);
}

static esp_err_t start_http_server(void)
{
    if (s_app.server) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    s_http_port = config.server_port;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 38;
    config.max_open_sockets = 3;
    config.stack_size = 12288;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 20;
    config.send_wait_timeout = 20;
    ESP_RETURN_ON_ERROR(httpd_start(&s_app.server, &config), TAG, "httpd start");

    const httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    const httpd_uri_t provision_page_uri = {.uri = "/provision", .method = HTTP_GET, .handler = provision_page_handler};
    const httpd_uri_t claw_page_uri = {.uri = "/claw", .method = HTTP_GET, .handler = claw_page_handler};
    const httpd_uri_t sleep_music_page_uri = {.uri = "/sleep_music", .method = HTTP_GET, .handler = sleep_music_page_handler};
    const httpd_uri_t wake_word_page_uri = {.uri = "/wake_word", .method = HTTP_GET, .handler = wake_word_page_handler};
    const httpd_uri_t provision_uri = {.uri = "/api/provision", .method = HTTP_POST, .handler = provision_handler};
    const httpd_uri_t dashboard_uri = {.uri = "/api/dashboard", .method = HTTP_GET, .handler = dashboard_handler};
    const httpd_uri_t status_uri = {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler};
    const httpd_uri_t audio_status_uri = {.uri = "/api/audio_status", .method = HTTP_GET, .handler = audio_status_handler};
    const httpd_uri_t command_uri = {.uri = "/api/command*", .method = HTTP_POST, .handler = command_handler};
    const httpd_uri_t ai_bridge_uri = {.uri = "/api/ai_bridge", .method = HTTP_POST, .handler = ai_bridge_config_handler};
    const httpd_uri_t ai_bridge_test_uri = {.uri = "/api/ai_bridge_test", .method = HTTP_POST, .handler = ai_bridge_test_handler};
    const httpd_uri_t net_diag_uri = {.uri = "/api/net_diag", .method = HTTP_GET, .handler = net_diag_handler};
    const httpd_uri_t feishu_min_uri = {.uri = "/api/feishu_min", .method = HTTP_GET, .handler = feishu_min_handler};
    const httpd_uri_t feishu_restart_uri = {.uri = "/api/feishu_restart", .method = HTTP_POST, .handler = feishu_restart_handler};
    const httpd_uri_t light_diag_uri = {.uri = "/api/light_diag", .method = HTTP_GET, .handler = light_diag_handler};
    const httpd_uri_t claw_config_get_uri = {.uri = "/api/claw_config", .method = HTTP_GET, .handler = claw_config_get_handler};
    const httpd_uri_t claw_config_post_uri = {.uri = "/api/claw_config", .method = HTTP_POST, .handler = claw_config_post_handler};
    const httpd_uri_t config_get_uri = {.uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler};
    const httpd_uri_t config_post_uri = {.uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler};
    const httpd_uri_t sleep_profile_uri = {.uri = "/api/sleep_profile", .method = HTTP_GET, .handler = sleep_profile_handler};
    const httpd_uri_t claw_tools_uri = {.uri = "/api/claw_tools", .method = HTTP_GET, .handler = claw_tools_handler};
    const httpd_uri_t claw_tool_uri = {.uri = "/api/claw_tool*", .method = HTTP_POST, .handler = claw_tool_handler};
    const httpd_uri_t audio_files_uri = {.uri = "/api/audio_files", .method = HTTP_GET, .handler = audio_files_handler};
    const httpd_uri_t audio_upload_uri = {.uri = "/api/audio_upload*", .method = HTTP_POST, .handler = audio_upload_handler};
    const httpd_uri_t wake_samples_uri = {.uri = "/api/wake_samples", .method = HTTP_GET, .handler = wake_samples_handler};
    const httpd_uri_t wake_download_uri = {.uri = "/api/wake_sample*", .method = HTTP_GET, .handler = wake_sample_download_handler};
    const httpd_uri_t wake_config_uri = {.uri = "/api/wake_word_config", .method = HTTP_POST, .handler = wake_word_config_handler};
    const httpd_uri_t wake_template_uri = {.uri = "/api/wake_template", .method = HTTP_POST, .handler = wake_template_handler};
    const httpd_uri_t wake_upload_uri = {.uri = "/api/wake_sample_upload*", .method = HTTP_POST, .handler = wake_sample_upload_handler};
    const httpd_uri_t debug_audio_capture_uri = {.uri = "/api/debug_audio_capture", .method = HTTP_POST, .handler = debug_audio_capture_handler};
    const httpd_uri_t debug_audio_file_uri = {.uri = "/api/debug_audio_file*", .method = HTTP_GET, .handler = debug_audio_file_handler};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &root_uri), TAG, "root uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &provision_page_uri), TAG, "provision page uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &claw_page_uri), TAG, "claw page uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &sleep_music_page_uri), TAG, "sleep music page uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &wake_word_page_uri), TAG, "wake word page uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &provision_uri), TAG, "provision uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &dashboard_uri), TAG, "dashboard uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &status_uri), TAG, "status uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &audio_status_uri), TAG, "audio status uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &command_uri), TAG, "command uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &ai_bridge_uri), TAG, "ai bridge uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &ai_bridge_test_uri), TAG, "ai bridge test uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &net_diag_uri), TAG, "net diag uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &feishu_min_uri), TAG, "feishu min uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &feishu_restart_uri), TAG, "feishu restart uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &light_diag_uri), TAG, "light diag uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &claw_config_get_uri), TAG, "claw config get uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &claw_config_post_uri), TAG, "claw config post uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &config_get_uri), TAG, "config get uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &config_post_uri), TAG, "config post uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &sleep_profile_uri), TAG, "sleep profile uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &claw_tools_uri), TAG, "claw tools uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &claw_tool_uri), TAG, "claw tool uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &audio_files_uri), TAG, "audio files uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &audio_upload_uri), TAG, "audio upload uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &wake_samples_uri), TAG, "wake samples uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &wake_download_uri), TAG, "wake download uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &wake_config_uri), TAG, "wake config uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &wake_template_uri), TAG, "wake template uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &wake_upload_uri), TAG, "wake upload uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &debug_audio_capture_uri), TAG, "debug audio capture uri");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.server, &debug_audio_file_uri), TAG, "debug audio file uri");
    ESP_LOGI(TAG, "HTTP server ready on port %u", config.server_port);
    return ESP_OK;
}

static esp_err_t restart_http_server(const char *reason)
{
    ESP_LOGW(TAG, "HTTP watchdog restarting server reason=%s", reason ? reason : "unknown");
    if (s_app.server) {
        httpd_handle_t old_server = s_app.server;
        s_app.server = NULL;
        (void)httpd_stop(old_server);
    }
    s_http_watchdog_failures = 0;
    s_http_start_result = start_http_server();
    return s_http_start_result;
}

static void http_server_watchdog(int64_t now_us)
{
    if (now_us - s_last_http_watchdog_us < 15000000LL) {
        return;
    }
    s_last_http_watchdog_us = now_us;
    if (!s_app.cfg.http_enabled || !s_app.wifi_started || s_http_port == 0) {
        return;
    }

    if (!s_app.server) {
        ESP_LOGW(TAG, "HTTP watchdog: server handle missing; starting");
        (void)restart_http_server("missing_handle");
        return;
    }

    char ip[16] = {0};
    portENTER_CRITICAL(&s_app.lock);
    snprintf(ip, sizeof(ip), "%s", s_app.sta_ip[0] ? s_app.sta_ip : "127.0.0.1");
    portEXIT_CRITICAL(&s_app.lock);

    int tcp_errno = 0;
    esp_err_t probe = tcp_connect_probe(ip, s_http_port, 600, &tcp_errno);
    if (probe == ESP_OK) {
        s_http_watchdog_failures = 0;
        return;
    }

    s_http_watchdog_failures++;
    ESP_LOGW(TAG, "HTTP watchdog probe failed %u err=%s errno=%d",
             (unsigned)s_http_watchdog_failures, esp_err_to_name(probe), tcp_errno);
    if (s_http_watchdog_failures < 2) {
        return;
    }

    (void)restart_http_server("tcp_probe_failed");
}

static void wifi_reconnect_watchdog(int64_t now_us)
{
    if (!s_app.wifi_started || !s_app.sta_has_credentials || s_app.sta_connected) {
        return;
    }
    if (s_app.last_sta_retry_us != 0 && now_us - s_app.last_sta_retry_us < WIFI_STA_RETRY_INTERVAL_US) {
        return;
    }
    esp_err_t retry_err = retry_sta_connect("1s_watchdog");
    if (retry_err != ESP_OK && retry_err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "1s STA reconnect failed: %s", esp_err_to_name(retry_err));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *connected =
            (const wifi_event_ap_staconnected_t *)event_data;
        printf("DG Control AP: client connected aid=%u mac=" MACSTR "\r\n",
               (unsigned)connected->aid, MAC2STR(connected->mac));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *disconnected =
            (const wifi_event_ap_stadisconnected_t *)event_data;
        printf("DG Control AP: client disconnected aid=%u mac=" MACSTR "\r\n",
               (unsigned)disconnected->aid, MAC2STR(disconnected->mac));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_app.sta_has_credentials) {
            s_app.sta_retry_count = 0;
            (void)retry_sta_connect("sta_start");
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;
        portENTER_CRITICAL(&s_app.lock);
        s_app.sta_connected = false;
        s_app.sta_ip[0] = '\0';
        portEXIT_CRITICAL(&s_app.lock);
        if (s_app.sta_has_credentials) {
            s_app.sta_retry_count++;
            unsigned reason = disconnected ? (unsigned)disconnected->reason : 0U;
            int rssi = disconnected ? (int)disconnected->rssi : 0;
            printf("DG Wi-Fi drop: ssid=%s reason=%u rssi=%d reconnect=%u immediate=1\r\n",
                   s_app.sta_ssid, reason, rssi, (unsigned)s_app.sta_retry_count);
            esp_err_t retry_err = retry_sta_connect("disconnect_immediate");
            if (retry_err != ESP_OK && retry_err != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "immediate STA reconnect failed: %s", esp_err_to_name(retry_err));
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_dns_info_t dns = {0};
        dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(223, 5, 5, 5);
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_err_t dns_err = esp_netif_set_dns_info(s_app.sta_netif, ESP_NETIF_DNS_MAIN, &dns);
        if (dns_err != ESP_OK) {
            ESP_LOGW(TAG, "set main DNS failed: %s", esp_err_to_name(dns_err));
        }
        dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(119, 29, 29, 29);
        dns_err = esp_netif_set_dns_info(s_app.sta_netif, ESP_NETIF_DNS_BACKUP, &dns);
        if (dns_err != ESP_OK) {
            ESP_LOGW(TAG, "set backup DNS failed: %s", esp_err_to_name(dns_err));
        }
        portENTER_CRITICAL(&s_app.lock);
        s_app.sta_connected = true;
        s_app.sta_retry_count = 0;
        s_app.sta_profiles_tried_in_cycle = 0;
        s_app.last_sta_retry_us = 0;
        snprintf(s_app.sta_ip, sizeof(s_app.sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        portEXIT_CRITICAL(&s_app.lock);
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "STA connected ssid=%s ip=%s dashboard=http://%s/", s_app.sta_ssid, s_app.sta_ip, s_app.sta_ip);
        printf("DG Wi-Fi connected: ssid=%s ip=%s dashboard=http://%s/\r\n",
               s_app.sta_ssid, s_app.sta_ip, s_app.sta_ip);
    }
}

static esp_err_t start_wifi_apsta(const mobile_app_config_t *config)
{
    ESP_RETURN_ON_ERROR(ensure_nvs(), TAG, "nvs init");
    (void)load_wifi_credentials();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t event_err = esp_event_loop_create_default();
    if (event_err != ESP_OK && event_err != ESP_ERR_INVALID_STATE) {
        return event_err;
    }
    if (!s_app.ap_netif) {
        s_app.ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (!s_app.sta_netif) {
        s_app.sta_netif = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    /* This device also keeps ESP-SR, LCD, audio DMA and radar resident. Four
     * static RX/TX buffers are enough for its low-throughput HTTP/ASR traffic
     * and recover about 22 KiB of scarce internal RAM. */
    wifi_init.static_rx_buf_num = 4;
    wifi_init.dynamic_rx_buf_num = 8;
    wifi_init.static_tx_buf_num = 4;
    wifi_init.cache_tx_buf_num = 8;
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL), TAG, "wifi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL), TAG, "ip event handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "wifi mode apsta");

    ESP_RETURN_ON_ERROR(configure_setup_ap(config), TAG, "wifi ap config");

    if (s_app.sta_has_credentials) {
        wifi_config_t sta_config = {0};
        copy_field(sta_config.sta.ssid, sizeof(sta_config.sta.ssid), s_app.sta_ssid);
        copy_field(sta_config.sta.password, sizeof(sta_config.sta.password), s_app.sta_password);
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        sta_config.sta.listen_interval = 1;
        sta_config.sta.failure_retry_cnt = 5;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), TAG, "wifi sta config");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "wifi ps none");
    s_app.wifi_started = true;
    ESP_RETURN_ON_ERROR(configure_setup_ap(config), TAG, "wifi ap config after start");
    ESP_LOGI(TAG, "Setup AP ready ssid=%s password=%s provision=http://192.168.4.1/provision", config->ssid, config->password);
    ESP_LOGI(TAG, "Setup Wi-Fi QR payload: WIFI:T:WPA;S:%s;P:%s;;", config->ssid, config->password);
    if (s_app.sta_has_credentials) {
        ESP_LOGI(TAG, "Loaded saved Wi-Fi ssid=%s; connecting as STA", s_app.sta_ssid);
    }
    return ESP_OK;
}

esp_err_t mobile_app_configure_wifi(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || strlen(ssid) > 32 || (password && strlen(password) > 64)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *pass = password ? password : "";
    ESP_RETURN_ON_ERROR(save_wifi_credentials(ssid, pass), TAG, "save wifi credentials");
    portENTER_CRITICAL(&s_app.lock);
    snprintf(s_app.sta_ssid, sizeof(s_app.sta_ssid), "%s", ssid);
    snprintf(s_app.sta_password, sizeof(s_app.sta_password), "%s", pass);
    s_app.sta_has_credentials = true;
    s_app.sta_connected = false;
    s_app.sta_retry_count = 0;
    s_app.sta_profile_index = 0;
    s_app.sta_profiles_tried_in_cycle = 0;
    s_app.sta_config_version++;
    s_app.last_sta_retry_us = 0;
    s_app.sta_ip[0] = '\0';
    portEXIT_CRITICAL(&s_app.lock);
    ESP_LOGI(TAG, "Wi-Fi credentials saved for ssid=%s; connecting", s_app.sta_ssid);
    printf("DG Wi-Fi saved: ssid=%s; connecting. Setup AP stays at DreamGuardian-Setup / http://192.168.4.1/provision\r\n",
           s_app.sta_ssid);
    if (!s_app.wifi_started) {
        return ESP_OK;
    }
    (void)configure_setup_ap(&s_app.cfg);
    (void)esp_wifi_disconnect();
    esp_err_t err = retry_sta_connect("provision");
    return (err == ESP_ERR_WIFI_CONN) ? ESP_OK : err;
}
esp_err_t mobile_app_start(const mobile_app_config_t *config)
{
    mobile_app_config_t default_config;
    if (!config) {
        mobile_app_get_default_config(&default_config);
        config = &default_config;
    }
    s_app.cfg = *config;
    if (!s_app.cfg.ssid) {
        s_app.cfg.ssid = SETUP_AP_DEFAULT_SSID;
    }
    if (!s_app.cfg.password) {
        s_app.cfg.password = SETUP_AP_DEFAULT_PASSWORD;
    }
    if (!s_app.cfg.hostname) {
        s_app.cfg.hostname = "dreamguardian";
    }
    if (s_app.cfg.channel == 0) {
        s_app.cfg.channel = SETUP_AP_DEFAULT_CHANNEL;
    }

    ESP_RETURN_ON_ERROR(ensure_nvs(), TAG, "nvs init");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t event_err = esp_event_loop_create_default();
    if (event_err != ESP_OK && event_err != ESP_ERR_INVALID_STATE) {
        return event_err;
    }
    (void)ensure_spiffs();
    s_wifi_start_result = start_wifi_apsta(&s_app.cfg);
    if (s_wifi_start_result != ESP_OK) {
        printf("DG HTTP: wifi start failed err=%s\r\n", esp_err_to_name(s_wifi_start_result));
        return s_wifi_start_result;
    }
    if (s_app.cfg.http_enabled) {
        s_http_start_result = start_http_server();
        printf("DG HTTP: server start err=%s port=%u\r\n",
               esp_err_to_name(s_http_start_result), (unsigned)s_http_port);
    } else {
        s_http_port = 0;
        s_http_start_result = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGI(TAG, "HTTP server disabled by config");
        printf("DG HTTP: disabled by config\r\n");
        return ESP_OK;
    }
    return s_http_start_result;
}

void mobile_app_update(const mobile_app_telemetry_t *telemetry)
{
    if (!telemetry) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_app.lock);
    s_app.telemetry = *telemetry;
    s_app.telemetry.wifi_sta_connected = s_app.sta_connected;
    snprintf(s_app.telemetry.wifi_ip, sizeof(s_app.telemetry.wifi_ip), "%s", s_app.sta_ip);
    snprintf(s_app.telemetry.wifi_ssid, sizeof(s_app.telemetry.wifi_ssid), "%s", s_app.sta_ssid);
    sleep_profile_update_locked(&s_app.telemetry, now_us);
    portEXIT_CRITICAL(&s_app.lock);
    wifi_reconnect_watchdog(now_us);
    http_server_watchdog(now_us);
}

bool mobile_app_is_sta_connected(void)
{
    return s_app.sta_connected;
}

void mobile_app_get_ip(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return;
    }
    portENTER_CRITICAL(&s_app.lock);
    snprintf(buffer, buffer_size, "%s", s_app.sta_ip);
    portEXIT_CRITICAL(&s_app.lock);
}

void mobile_app_get_sta_ssid(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return;
    }
    portENTER_CRITICAL(&s_app.lock);
    snprintf(buffer, buffer_size, "%s", s_app.sta_ssid);
    portEXIT_CRITICAL(&s_app.lock);
}

void mobile_app_get_diag(mobile_app_diag_t *diag)
{
    if (!diag) {
        return;
    }
    memset(diag, 0, sizeof(*diag));
    char ip[16] = {0};
    portENTER_CRITICAL(&s_app.lock);
    diag->http_result = s_http_start_result;
    diag->wifi_result = s_wifi_start_result;
    diag->http_port = s_http_port;
    diag->server_running = s_app.server != NULL;
    diag->wifi_started = s_app.wifi_started;
    diag->sta_connected = s_app.sta_connected;
    diag->sta_config_version = s_app.sta_config_version;
    snprintf(diag->sta_ip, sizeof(diag->sta_ip), "%s", s_app.sta_ip);
    snprintf(diag->sta_ssid, sizeof(diag->sta_ssid), "%s", s_app.sta_ssid);
    snprintf(ip, sizeof(ip), "%s", s_app.sta_ip[0] ? s_app.sta_ip : "127.0.0.1");
    portEXIT_CRITICAL(&s_app.lock);

    if (diag->server_running && diag->wifi_started && s_http_port != 0) {
        int self_errno = 0;
        diag->self_tcp_result = tcp_connect_probe(ip, s_http_port, 1000, &self_errno);
        diag->self_tcp_errno = self_errno;
    } else {
        diag->self_tcp_result = -1;
        diag->self_tcp_errno = ENETDOWN;
    }
    TaskHandle_t http_task = xTaskGetHandle("httpd");
    diag->http_task_found = http_task != NULL;
    if (http_task) {
        diag->http_task_stack_free = uxTaskGetStackHighWaterMark(http_task);
    }
}
