#include "massage.h"
#include "massage_ble.h"
#include "massage_protocol.h"
#include "massage_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG="MASSAGE";
#define MASSAGE_QUEUE_DEPTH 8
typedef enum { EV_START,EV_STOP,EV_MODE,EV_INTENSITY,EV_DURATION,EV_RAW } event_type_t;
typedef struct { event_type_t type; uint16_t value; uint8_t data[64]; size_t len; } massage_event_t;
static QueueHandle_t s_q; static TaskHandle_t s_task; static volatile massage_state_t s_state=MASSAGE_STATE_IDLE;
static StaticQueue_t s_q_control;
static uint8_t *s_q_storage;
static uint8_t s_mode=MASSAGE_DEFAULT_MODE,s_intensity=MASSAGE_DEFAULT_INTENSITY; static uint16_t s_duration=MASSAGE_DEFAULT_DURATION_MIN;
static void set_state(massage_state_t st){s_state=st;ESP_LOGI(TAG,"state=%d",st);}
static esp_err_t send_built(size_t (*fn)(uint8_t*)){uint8_t b[32];size_t n=fn(b);return massage_ble_write(b,n);}
static void task(void *arg){massage_event_t e;for(;;){if(xQueueReceive(s_q,&e,portMAX_DELAY)!=pdTRUE)continue;esp_err_t r=ESP_OK;switch(e.type){case EV_START:r=send_built(massage_protocol_build_start);if(!r)set_state(MASSAGE_STATE_RUNNING);break;case EV_STOP:r=send_built(massage_protocol_build_stop);if(!r)set_state(MASSAGE_STATE_READY);break;case EV_MODE:{uint8_t b[8];size_t n=massage_protocol_build_mode(b,(uint8_t)e.value);r=massage_ble_write(b,n);if(!r)s_mode=e.value;break;}case EV_INTENSITY:{uint8_t b[8];size_t n=massage_protocol_build_intensity(b,(uint8_t)e.value);r=massage_ble_write(b,n);if(!r)s_intensity=e.value;break;}case EV_DURATION:{uint8_t b[8];size_t n=massage_protocol_build_duration(b,e.value);r=massage_ble_write(b,n);if(!r)s_duration=e.value;break;}case EV_RAW:r=massage_ble_write(e.data,e.len);break;}if(r!=ESP_OK) { set_state(massage_ble_is_connected()?MASSAGE_STATE_ERROR:MASSAGE_STATE_DISCONNECTED);ESP_LOGE(TAG,"command failed err=%s",esp_err_to_name(r));}}}
esp_err_t massage_init(void){
    if(s_q)return ESP_OK;
    s_q_storage=heap_caps_malloc(MASSAGE_QUEUE_DEPTH*sizeof(massage_event_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s_q_storage){printf("DG Massage: queue PSRAM allocation failed\r\n");return ESP_ERR_NO_MEM;}
    s_q=xQueueCreateStatic(MASSAGE_QUEUE_DEPTH,sizeof(massage_event_t),s_q_storage,&s_q_control);
    if(!s_q){heap_caps_free(s_q_storage);s_q_storage=NULL;return ESP_ERR_NO_MEM;}
    if(xTaskCreateWithCaps(task,"massage_task",4096,NULL,5,&s_task,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){
        vQueueDelete(s_q);s_q=NULL;heap_caps_free(s_q_storage);s_q_storage=NULL;
        printf("DG Massage: worker PSRAM task creation failed\r\n");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t r=massage_ble_init();
    printf("DG Massage: init err=%s internal=%u largest=%u\r\n",esp_err_to_name(r),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if(r)return r;
    set_state(MASSAGE_STATE_IDLE);
    return ESP_OK;
}
static esp_err_t post(event_type_t t,uint16_t v,const uint8_t*d,size_t n){if(!s_q)return ESP_ERR_INVALID_STATE;massage_event_t e={.type=t,.value=v,.len=n};if(d&&n)memcpy(e.data,d,n);return xQueueSend(s_q,&e,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;}
esp_err_t massage_scan_start(void){set_state(MASSAGE_STATE_SCANNING);return massage_ble_scan_start();}
esp_err_t massage_connect(void){set_state(MASSAGE_STATE_CONNECTING);return massage_ble_connect();}
esp_err_t massage_disconnect(void){set_state(MASSAGE_STATE_DISCONNECTED);return massage_ble_disconnect();}
esp_err_t massage_start(void){if(!massage_ble_is_connected())return ESP_ERR_INVALID_STATE;ESP_LOGI(TAG,"Start request");return post(EV_START,0,NULL,0);}
esp_err_t massage_stop(void){return post(EV_STOP,0,NULL,0);}
esp_err_t massage_set_mode(uint8_t v){if(v<MASSAGE_MIN_MODE||v>MASSAGE_MAX_MODE)return ESP_ERR_INVALID_ARG;return ESP_ERR_NOT_SUPPORTED;}
esp_err_t massage_set_intensity(uint8_t v){if(v<MASSAGE_MIN_INTENSITY||v>MASSAGE_MAX_INTENSITY)return ESP_ERR_INVALID_ARG;return post(EV_INTENSITY,v,NULL,0);}
esp_err_t massage_set_duration(uint16_t v){if(!v||v>MASSAGE_MAX_DURATION_MIN)return ESP_ERR_INVALID_ARG;return ESP_ERR_NOT_SUPPORTED;}
bool massage_is_connected(void){return massage_ble_is_connected();}
massage_state_t massage_get_state(void){return s_state;}
esp_err_t massage_send_raw(const uint8_t*d,size_t n){if(!d||!n||n>64)return ESP_ERR_INVALID_ARG;return post(EV_RAW,0,d,n);}
esp_err_t massage_send_hex_string(const char *s){if(!s)return ESP_ERR_INVALID_ARG;uint8_t b[64];size_t n=0;while(*s){while(*s==' '||*s=='\t'||*s=='\r'||*s=='\n')s++;if(!*s)break;char *end=NULL;long v=strtol(s,&end,16);if(end==s||v<0||v>255||n>=sizeof(b))return ESP_ERR_INVALID_ARG;b[n++]=(uint8_t)v;s=end;}return massage_send_raw(b,n);}
