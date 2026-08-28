/* Optional standalone example. Do not add this file to the existing application's
 * main component; it is intentionally not compiled by the massage component. */
#include "massage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
void massage_example_app_main(void) { massage_init(); massage_scan_start(); while (!massage_is_connected()) vTaskDelay(pdMS_TO_TICKS(500)); }
