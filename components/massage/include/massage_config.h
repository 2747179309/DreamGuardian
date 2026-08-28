#pragma once

/* Verified by the PC Bleak test in 协议-提示词-电脑测试. */
#define MASSAGE_DEVICE_NAME "DF000001"
#define MASSAGE_DEVICE_NAME_PREFIX "DF"
#define MASSAGE_TARGET_MAC "00:00:00:00:00:00"
#define MASSAGE_TARGET_ADDR_TYPE 0 /* public; verify if a replacement unit differs */
#define MASSAGE_SERVICE_UUID "0000fff0-0000-1000-8000-00805f9b34fb"
#define MASSAGE_WRITE_UUID "0000fff2-0000-1000-8000-00805f9b34fb"
#define MASSAGE_NOTIFY_UUID "0000fff1-0000-1000-8000-00805f9b34fb"

#define MASSAGE_DEFAULT_MODE 1 /* reserved; not present in the verified protocol */
#define MASSAGE_DEFAULT_INTENSITY 1
#define MASSAGE_DEFAULT_DURATION_MIN 10 /* reserved; not present in the verified protocol */
#define MASSAGE_MIN_INTENSITY 1
#define MASSAGE_MAX_INTENSITY 5
#define MASSAGE_MIN_MODE 1
#define MASSAGE_MAX_MODE 6
#define MASSAGE_MAX_DURATION_MIN 30
#define MASSAGE_WRITE_WITH_RESPONSE 1
#define MASSAGE_SCAN_DURATION_MS 10000
#define MASSAGE_RECONNECT_DELAY_MS 3000
