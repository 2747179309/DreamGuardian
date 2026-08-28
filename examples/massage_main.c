/* Standalone reference app_main for a separate BLE massage test project.
 * It is deliberately not included by the existing DreamGuardian application. */
#include "massage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    ESP_ERROR_CHECK(massage_init());
    ESP_ERROR_CHECK(massage_scan_start());
    while (!massage_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    /* No automatic stimulation: use the serial CLI/application layer to call start. */
}
