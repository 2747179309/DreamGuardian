#pragma once

#define DG_DEVICE_ID "DG2-DEMO-001"

#define DG_LD6002_UART_NUM 1
#define DG_LD6002_UART_BAUD 115200

/* LD6002 radar TXD is wired to GPIO44. UART1/GPIO44 was validated on device.
   The radar is transmit-only for this firmware path, so ESP TX is left unused. */
#define DG_LD6002_UART_TX_GPIO (-1)
#define DG_LD6002_UART_RX_GPIO 44

/* Internal ESP32-S3-BOX-3 ES8311 codec via BSP audio. */
#define DG_STEREO_I2S_NUM 1
#define DG_STEREO_I2S_BCLK_GPIO 17
#define DG_STEREO_I2S_WS_GPIO 45
#define DG_STEREO_I2S_DOUT_GPIO 15
#define DG_STEREO_SAMPLE_RATE_HZ 16000

/* WS2812 serial status light strip. */
#define DG_STATUS_LIGHT_WS2812 1
#define DG_WS2812_LED_GPIO 38
#define DG_WS2812_LED_COUNT 24

/* Common-ground discrete RGB status light. */
#define DG_RGB_LED_R_GPIO 39
#define DG_RGB_LED_G_GPIO 40
#define DG_RGB_LED_B_GPIO 41
#define DG_RGB_LED_ACTIVE_LOW 0

#define DG_RADAR_TASK_STACK_SIZE 4096
#define DG_RADAR_TASK_PRIORITY 5
#define DG_APP_LOOP_PERIOD_MS 200

/* Enable the inbound Feishu app WebSocket agent.
   Outbound webhook reports are handled separately by ai_bridge. */
#define DG_FEISHU_AGENT_ENABLED 1
