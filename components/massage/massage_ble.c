#include "massage_ble.h"
#include "massage_config.h"
#include "massage_protocol.h"
#include "esp_log.h"
#include "esp_nimble_cfg.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"
#include <string.h>
#include <stdio.h>

static const char *TAG="MASSAGE_BLE";
static bool s_connected;
static uint16_t s_conn, s_write, s_notify;
static uint16_t s_service_end;
static ble_addr_t s_addr;
static bool s_have_addr;
static bool s_host_synced;
static bool s_connect_requested;
static bool s_connecting;
static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static TaskHandle_t s_host_task;
static uint8_t s_disc_phase;
static int ble_gap_event(struct ble_gap_event *e, void *arg);
static int svc_cb(uint16_t c,const struct ble_gatt_error *err,const struct ble_gatt_svc *svc,void *arg);
static int chr_cb(uint16_t c,const struct ble_gatt_error *err,const struct ble_gatt_chr *chr,void *arg);
static int dsc_cb(uint16_t c,const struct ble_gatt_error *err,uint16_t chr_val_handle,const struct ble_gatt_dsc *dsc,void *arg);
static int write_cb(uint16_t c,const struct ble_gatt_error *err,struct ble_gatt_attr *attr,void *arg);
static esp_err_t start_connect(void);
static void host_task(void *arg) { (void)arg; nimble_port_run(); s_host_task=NULL; vTaskDeleteWithCaps(NULL); }
static void on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Cannot infer own address type rc=%d", rc);
        return;
    }
    s_host_synced = true;
    printf("DG Massage BLE: host synced own_addr_type=%u internal=%u largest=%u\r\n",
           s_own_addr_type,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "NimBLE host synced own_addr_type=%u", s_own_addr_type);
    if (s_connect_requested && !s_connected && !s_connecting) {
        esp_err_t err = start_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Deferred connect failed err=%s", esp_err_to_name(err));
        }
    }
}

void massage_hex_dump(const char *tag,const uint8_t *d,size_t n) {
    if (!d) return;
    char line[3*64+1]; size_t p=0;
    for(size_t i=0;i<n && i<64;i++) { if(p+3>=sizeof(line))break; p += (size_t)snprintf(line+p,sizeof(line)-p,"%02X ",d[i]); }
    line[p]=0; ESP_LOGI(TAG,"%s len=%u %s",tag,(unsigned)n,line);
    printf("DG Massage %s len=%u %s\r\n",tag,(unsigned)n,line);
}
static bool name_match(const char *name) {
    if (!name) return false;
    return !strcmp(name,MASSAGE_DEVICE_NAME) || !strncmp(name,MASSAGE_DEVICE_NAME_PREFIX,strlen(MASSAGE_DEVICE_NAME_PREFIX));
}
static bool uuid_is(const ble_uuid_any_t *u,const char *text) {
    ble_uuid_any_t wanted; memset(&wanted,0,sizeof(wanted));
    return ble_uuid_from_str(&wanted,text)==0 && ble_uuid_cmp(&u->u,&wanted.u)==0;
}
static bool configured_addr(ble_addr_t *out) {
    unsigned int b[6];
    if (sscanf(MASSAGE_TARGET_MAC,"%02x:%02x:%02x:%02x:%02x:%02x",&b[0],&b[1],&b[2],&b[3],&b[4],&b[5]) != 6) return false;
    out->type=MASSAGE_TARGET_ADDR_TYPE;
    /* NimBLE stores addresses least-significant byte first. */
    for (int i=0;i<6;i++) out->val[i]=(uint8_t)b[5-i];
    return true;
}
static esp_err_t start_connect(void) {
    if (!s_host_synced) return ESP_ERR_INVALID_STATE;
    if (s_connected || s_connecting) return ESP_OK;
    int rc = ble_gap_connect(s_own_addr_type, &s_addr, 30000, NULL, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Connect start failed rc=%d", rc);
        return ESP_FAIL;
    }
    s_connecting = true;
    ESP_LOGI(TAG, "Connecting to configured device addr_type=%u", s_addr.type);
    printf("DG Massage BLE: connecting addr_type=%u\r\n",s_addr.type);
    return ESP_OK;
}
static int write_cb(uint16_t c,const struct ble_gatt_error *err,struct ble_gatt_attr *attr,void *arg) {
    (void)c; (void)attr; (void)arg;
    if (err->status == 0) ESP_LOGI(TAG,"Write With Response completed");
    else ESP_LOGE(TAG,"Write With Response failed status=%d",err->status);
    return 0;
}
static int svc_cb(uint16_t c,const struct ble_gatt_error *err,const struct ble_gatt_svc *svc,void *arg) {
    (void)arg;
    if (err->status==0 && svc) {
        if (uuid_is(&svc->uuid,MASSAGE_SERVICE_UUID)) {
            ESP_LOGI(TAG,"Service discovered start=0x%04x end=0x%04x",svc->start_handle,svc->end_handle);
            s_service_end=svc->end_handle;
            s_disc_phase=1; ble_gattc_disc_all_chrs(c,svc->start_handle,svc->end_handle,chr_cb,NULL);
        }
    } else if (err->status==BLE_HS_EDONE && !s_disc_phase) {
        ESP_LOGE(TAG,"Service UUID not found");
    }
    return 0;
}
static int chr_cb(uint16_t c,const struct ble_gatt_error *err,const struct ble_gatt_chr *chr,void *arg) {
    (void)arg;
    if (err->status==0 && chr) {
        if (uuid_is(&chr->uuid,MASSAGE_WRITE_UUID)) { s_write=chr->val_handle; ESP_LOGI(TAG,"Write characteristic=0x%04x",s_write); }
        if (uuid_is(&chr->uuid,MASSAGE_NOTIFY_UUID)) {
            s_notify=chr->val_handle;
            ESP_LOGI(TAG,"Notify characteristic=0x%04x",s_notify);
            ble_gattc_disc_all_dscs(c, (uint16_t)(chr->val_handle + 1), s_service_end, dsc_cb, NULL);
        }
    } else if (err->status==BLE_HS_EDONE) {
        if (!s_write) ESP_LOGE(TAG,"Write characteristic not found");
        else { s_connected=true; ESP_LOGI(TAG,"Service discovery complete; READY"); }
    }
    return 0;
}
static int dsc_cb(uint16_t c,const struct ble_gatt_error *err,uint16_t chr_val_handle,const struct ble_gatt_dsc *dsc,void *arg) {
    (void)arg;
    if (err->status==0 && dsc && chr_val_handle==s_notify && ble_uuid_u16(&dsc->uuid.u)==0x2902) {
        static const uint8_t enable_notify[2]={0x01,0x00};
        ESP_LOGI(TAG,"FFF1 Notify CCCD=0x%04x; enabling",dsc->handle);
        int rc=ble_gattc_write_flat(c,dsc->handle,enable_notify,sizeof(enable_notify),write_cb,NULL);
        if (rc) ESP_LOGE(TAG,"Notify enable failed rc=%d",rc);
    }
    return 0;
}
static int ble_gap_event(struct ble_gap_event *e, void *arg) {
    (void)arg;
    if (e->type==BLE_GAP_EVENT_DISC) {
        struct ble_hs_adv_fields f; memset(&f,0,sizeof(f));
        if (ble_hs_adv_parse_fields(&f,e->disc.data,e->disc.length_data)==0) {
            char name[BLE_HS_ADV_MAX_FIELD_SZ+1]={0};
            if (f.name && f.name_len) { size_t n=f.name_len<sizeof(name)-1?f.name_len:sizeof(name)-1; memcpy(name,f.name,n); name[n]=0; }
            if (name_match(name)) { s_addr=e->disc.addr; s_have_addr=true; s_connect_requested=true; ESP_LOGI(TAG,"Device found name=%s rssi=%d addr_type=%d",name,e->disc.rssi,e->disc.addr.type); ble_gap_disc_cancel(); (void)start_connect(); }
        }
    } else if (e->type==BLE_GAP_EVENT_CONNECT) {
        s_connecting=false;
        if (e->connect.status==0) { s_connected=false;s_conn=e->connect.conn_handle;s_disc_phase=0; ESP_LOGI(TAG,"Connected handle=%u",s_conn); printf("DG Massage BLE: connected handle=%u\r\n",s_conn); ble_gattc_disc_all_svcs(s_conn,svc_cb,NULL); }
        else { s_connected=false; ESP_LOGW(TAG,"Connect failed status=%d",e->connect.status); printf("DG Massage BLE: connect failed status=%d\r\n",e->connect.status); }
    } else if (e->type==BLE_GAP_EVENT_DISCONNECT) { s_connected=false;s_connecting=false;s_write=0;s_notify=0; ESP_LOGW(TAG,"Disconnected reason=%d",e->disconnect.reason); printf("DG Massage BLE: disconnected reason=%d\r\n",e->disconnect.reason); }
    else if (e->type==BLE_GAP_EVENT_NOTIFY_RX) {
        uint8_t rx[128]; uint16_t n=OS_MBUF_PKTLEN(e->notify_rx.om); if(n>sizeof(rx))n=sizeof(rx);
        if (os_mbuf_copydata(e->notify_rx.om,0,n,rx)==0) {
            massage_hex_dump("[Massage RX]",rx,n);
            if (n==6 && rx[0]==0x5A && rx[1]==0xC0 && rx[5]==0xA5)
                ESP_LOGI(TAG,"ACK received for CMD=0x%02X",rx[2]);
            else if (n==6 && rx[0]==0x5A && rx[1]==0x45 && rx[5]==0xA5)
                ESP_LOGI(TAG,"Periodic status Notify value=0x%02X",rx[4]);
        }
    }
    return 0;
}
esp_err_t massage_ble_init(void) {
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed err=%s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb=on_sync;
    if(xTaskCreatePinnedToCoreWithCaps(host_task,"nimble_host",CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE,
                                      NULL,configMAX_PRIORITIES-4,&s_host_task,
                                      CONFIG_BT_NIMBLE_PINNED_TO_CORE,
                                      MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){
        ESP_LOGE(TAG,"NimBLE PSRAM host task creation failed");
        printf("DG Massage BLE: host PSRAM task creation failed\r\n");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t massage_ble_scan_start(void) { if(!s_host_synced)return ESP_ERR_INVALID_STATE; struct ble_gap_disc_params p={0};p.passive=1;p.filter_duplicates=1;p.itvl=0x50;p.window=0x30; int rc=ble_gap_disc(s_own_addr_type,MASSAGE_SCAN_DURATION_MS,&p,ble_gap_event,NULL); return rc==0?ESP_OK:ESP_FAIL; }
esp_err_t massage_ble_connect(void) {
    if (!s_have_addr && !configured_addr(&s_addr)) return ESP_ERR_INVALID_ARG;
    s_have_addr=true;
    s_connect_requested=true;
    if (!s_host_synced) {
        ESP_LOGI(TAG, "Connect deferred until NimBLE host sync");
        return ESP_OK;
    }
    return start_connect();
}
esp_err_t massage_ble_disconnect(void) { if(!s_connected)return ESP_OK; return ble_gap_terminate(s_conn,BLE_ERR_REM_USER_CONN_TERM)==0?ESP_OK:ESP_FAIL; }
bool massage_ble_is_connected(void){return s_connected;}
esp_err_t massage_ble_write(const uint8_t *d,size_t n) {
    if(!d||!n||!s_connected||!s_write)return ESP_ERR_INVALID_STATE;
    massage_hex_dump("[Massage TX]",d,n);
    int rc;
    if (MASSAGE_WRITE_WITH_RESPONSE) rc=ble_gattc_write_flat(s_conn,s_write,d,n,write_cb,NULL);
    else rc=ble_gattc_write_no_rsp_flat(s_conn,s_write,d,n);
    return rc==0?ESP_OK:ESP_FAIL;
}
