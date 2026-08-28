#include "massage_cli.h"
#include "massage.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG="MASSAGE";
static int hexval(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
esp_err_t massage_cli_execute(const char *line){
    if(!line)return ESP_ERR_INVALID_ARG;
    while(*line==' '||*line=='\t')line++;
    if(!strcmp(line,"massage scan"))return massage_scan_start();
    if(!strcmp(line,"massage connect"))return massage_connect();
    if(!strcmp(line,"massage status")){ESP_LOGI(TAG,"connected=%d state=%d",massage_is_connected(),massage_get_state());return ESP_OK;}
    if(!strcmp(line,"massage start"))return massage_start();
    if(!strcmp(line,"massage stop"))return massage_stop();
    const char *p=NULL; if(!strncmp(line,"massage mode ",13)){p=line+13;return massage_set_mode((uint8_t)strtoul(p,NULL,10));}
    if(!strncmp(line,"massage intensity ",18)){p=line+18;return massage_set_intensity((uint8_t)strtoul(p,NULL,10));}
    if(!strncmp(line,"massage time ",13)){p=line+13;return massage_set_duration((uint16_t)strtoul(p,NULL,10));}
    if(!strncmp(line,"massage raw ",12)){uint8_t b[64];size_t n=0; p=line+12;while(*p){while(*p==' '||*p=='\t')p++;if(!*p)break;int h=hexval(*p++),l=hexval(*p++);if(h<0||l<0||n>=sizeof(b))return ESP_ERR_INVALID_ARG;b[n++]=(uint8_t)((h<<4)|l);}return massage_send_raw(b,n);}
    return ESP_ERR_NOT_FOUND;
}
