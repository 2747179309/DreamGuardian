#include "massage_protocol.h"
#include <string.h>

/* Verified protocol from the PC Bleak test. Frame: 5A CMD P1 P2 P3 A5. */
static const uint8_t START_CMD[] = {0x5A,0x3A,0x00,0x00,0x00,0xA5};
static const uint8_t STOP_CMD[]  = {0x5A,0x38,0x00,0x00,0x00,0xA5};
static size_t copy_cmd(uint8_t *b, const uint8_t *c, size_t n) { if (!b) return 0; memcpy(b,c,n); return n; }
size_t massage_protocol_build_start(uint8_t *b) { return copy_cmd(b,START_CMD,sizeof(START_CMD)); }
size_t massage_protocol_build_stop(uint8_t *b) { return copy_cmd(b,STOP_CMD,sizeof(STOP_CMD)); }
size_t massage_protocol_build_mode(uint8_t *b,uint8_t mode) { (void)b; (void)mode; return 0; }
size_t massage_protocol_build_intensity(uint8_t *b,uint8_t intensity) {
    static const uint8_t map[6]={0,0x01,0x03,0x05,0x06,0x07};
    if(!b||intensity<1||intensity>5)return 0;
    b[0]=0x5A;b[1]=0x40;b[2]=0x00;b[3]=0x00;b[4]=map[intensity];b[5]=0xA5;return 6;
}
size_t massage_protocol_build_duration(uint8_t *b,uint16_t m) { (void)b; (void)m; return 0; }
esp_err_t massage_protocol_parse_notify(const uint8_t *data,size_t len,massage_device_status_t *s) {
    if (!data || !s) return ESP_ERR_INVALID_ARG;
    if (len != 6 || data[0] != 0x5A || data[5] != 0xA5) return ESP_ERR_NOT_SUPPORTED;
    if (data[1] == 0x45) { return ESP_ERR_NOT_SUPPORTED; }
    return data[1] == 0xC0 ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}
