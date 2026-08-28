#include "ld6002_task.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "dg_debug_control.h"
#include "ld6002_parser.h"

#define LD6002_UART_BUF_SIZE 1024
#define LD6002_READ_CHUNK_SIZE 128
#define LD6002_TASK_STACK_SIZE 4096
#define LD6002_NO_DATA_RECOVERY_US 5000000LL
#define LD6002_RECOVERY_LOG_US 15000000LL

static const char *TAG = "ld6002";
static ld6002_snapshot_t s_snapshot;
static SemaphoreHandle_t s_snapshot_lock;
static StaticSemaphore_t s_snapshot_lock_storage;
static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[LD6002_TASK_STACK_SIZE / sizeof(StackType_t)];
static TaskHandle_t s_task;
static ld6002_task_config_t s_config;
static volatile bool s_recovery_requested;
static volatile uint32_t s_rx_edge_count;
static bool s_rx_edge_handler_registered;
static bool s_rx_edge_monitor_active;
static bool s_started;

static void IRAM_ATTR ld6002_rx_edge_isr(void *arg)
{
    (void)arg;
    s_rx_edge_count++;
}

static void rx_edge_monitor_restore(int rx_gpio)
{
    if (!s_rx_edge_handler_registered || rx_gpio < 0 || !s_rx_edge_monitor_active) {
        return;
    }
    (void)gpio_set_intr_type((gpio_num_t)rx_gpio, GPIO_INTR_ANYEDGE);
    (void)gpio_intr_enable((gpio_num_t)rx_gpio);
}

static esp_err_t uart_apply_config(const ld6002_task_config_t *config, bool flush_input)
{
    uart_config_t uart_config = {
        .baud_rate = config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        /* XTAL is stable while CPU/APB frequency changes during quiet sleep periods. */
        .source_clk = UART_SCLK_XTAL,
    };

    if (config->rx_gpio >= 0) {
        (void)gpio_reset_pin((gpio_num_t)config->rx_gpio);
    }
    ESP_RETURN_ON_ERROR(uart_param_config(config->uart_num, &uart_config), TAG,
                        "uart config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(config->uart_num, config->tx_gpio, config->rx_gpio,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart pin config failed");
    if (config->rx_gpio >= 0) {
        (void)gpio_set_pull_mode((gpio_num_t)config->rx_gpio, GPIO_PULLUP_ONLY);
        rx_edge_monitor_restore(config->rx_gpio);
    }
    if (flush_input) {
        (void)uart_flush_input(config->uart_num);
    }
    return ESP_OK;
}

static void recover_uart(const ld6002_task_config_t *config, const char *reason,
                         uint32_t recovery_count, int64_t now_us)
{
    static int64_t s_last_recovery_log_us;
    size_t buffered = 0;
    (void)uart_get_buffered_data_len(config->uart_num, &buffered);
    esp_err_t err = uart_apply_config(config, true);
    int level = config->rx_gpio >= 0 ? gpio_get_level((gpio_num_t)config->rx_gpio) : -1;
    if (recovery_count == 1 || now_us - s_last_recovery_log_us >= LD6002_RECOVERY_LOG_US ||
        err != ESP_OK) {
        printf("DG Radar: UART recovery #%u reason=%s err=%s rx_gpio=%d level=%d edges=%u buffered=%u\r\n",
               (unsigned)recovery_count, reason ? reason : "no_data", esp_err_to_name(err),
               config->rx_gpio, level, (unsigned)s_rx_edge_count, (unsigned)buffered);
        s_last_recovery_log_us = now_us;
    }
}

static void snapshot_report_uart_diag(int64_t now_us)
{
    static int64_t s_last_report_us;

    if (now_us - s_last_report_us < 3000000LL) {
        return;
    }
    if (dg_debug_quiet_enabled()) {
        s_last_report_us = now_us;
        return;
    }

    ld6002_snapshot_t snapshot = {0};
    xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    memcpy(&snapshot, &s_snapshot, sizeof(snapshot));
    xSemaphoreGive(s_snapshot_lock);

    s_last_report_us = now_us;

    printf("DG Radar: rx=%u edges=%u last=0x%02x raw=%u ok=%u unk=%u err=%u/%u type=0x%04x len=%u\r\n",
           (unsigned)snapshot.uart_bytes,
           (unsigned)s_rx_edge_count,
           snapshot.last_byte,
           (unsigned)snapshot.raw_frames,
           (unsigned)snapshot.frames,
           (unsigned)snapshot.unknown_frames,
           (unsigned)snapshot.checksum_errors,
           (unsigned)snapshot.parse_errors,
           (unsigned)snapshot.last_type,
           (unsigned)snapshot.last_len);
}

static void snapshot_update_from_parser(const ld6002_parser_t *parser)
{
    if (!s_snapshot_lock) {
        return;
    }
    xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    s_snapshot.checksum_errors = parser->checksum_errors;
    s_snapshot.parse_errors = parser->length_errors;
    xSemaphoreGive(s_snapshot_lock);
}

static void ld6002_uart_task(void *arg)
{
    const ld6002_task_config_t config = *(const ld6002_task_config_t *)arg;

    uint8_t bytes[LD6002_READ_CHUNK_SIZE];
    ld6002_parser_t parser;
    ld6002_parser_init(&parser);
    int64_t last_rx_us = esp_timer_get_time();
    uint32_t recovery_count = 0;

    ESP_LOGI(TAG, "UART%d started at %d baud, TX=%d RX=%d",
             config.uart_num, config.baud_rate, config.tx_gpio, config.rx_gpio);
    if (!dg_debug_quiet_enabled()) {
        printf("DG Radar: UART%d start baud=%d tx=GPIO%d rx=GPIO%d\r\n",
               (int)config.uart_num, config.baud_rate, config.tx_gpio, config.rx_gpio);
    }

    while (true) {
        if (s_recovery_requested) {
            s_recovery_requested = false;
            recovery_count++;
            recover_uart(&config, "sleep_entry", recovery_count, esp_timer_get_time());
            ld6002_parser_reset(&parser);
            last_rx_us = esp_timer_get_time();
        }

        int read = uart_read_bytes(config.uart_num, bytes, sizeof(bytes), pdMS_TO_TICKS(100));
        int64_t now_us = esp_timer_get_time();
        if (read <= 0) {
            if (now_us - last_rx_us >= LD6002_NO_DATA_RECOVERY_US) {
                recovery_count++;
                recover_uart(&config, "no_data", recovery_count, now_us);
                ld6002_parser_reset(&parser);
                last_rx_us = now_us;
            }
            snapshot_report_uart_diag(now_us);
            continue;
        }
        last_rx_us = now_us;

        /* UART data proves the signal path is healthy. Stop the temporary
         * edge interrupt so normal radar traffic has no diagnostic overhead. */
        if (s_rx_edge_monitor_active && config.rx_gpio >= 0) {
            (void)gpio_intr_disable((gpio_num_t)config.rx_gpio);
            s_rx_edge_monitor_active = false;
        }

        xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
        s_snapshot.uart_bytes += (uint32_t)read;
        s_snapshot.last_byte = bytes[read - 1];
        s_snapshot.last_byte_update_us = now_us;
        xSemaphoreGive(s_snapshot_lock);

        for (int i = 0; i < read; ++i) {
            ld6002_frame_t frame = {0};
            ld6002_parse_result_t result = ld6002_parser_feed(&parser, bytes[i], &frame);
            if (result == LD6002_PARSE_FRAME) {
                xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
                s_snapshot.raw_frames++;
                s_snapshot.last_type = frame.type;
                s_snapshot.last_len = frame.len;
                bool applied = ld6002_apply_frame(&frame, &s_snapshot, now_us);
                if (!applied) {
                    s_snapshot.unknown_frames++;
                }
                s_snapshot.checksum_errors = parser.checksum_errors;
                s_snapshot.parse_errors = parser.length_errors;
                xSemaphoreGive(s_snapshot_lock);
                if (applied) {
                    ESP_LOGD(TAG, "frame type=0x%04x len=%u", frame.type, frame.len);
                }
                ld6002_parser_reset(&parser);
            } else if (result == LD6002_PARSE_CHECKSUM_ERROR ||
                       result == LD6002_PARSE_LENGTH_ERROR) {
                snapshot_update_from_parser(&parser);
                ld6002_parser_reset(&parser);
            }
        }
        snapshot_report_uart_diag(now_us);
    }
}

esp_err_t ld6002_task_start(const ld6002_task_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_OK;
    }

    s_snapshot_lock = xSemaphoreCreateMutexStatic(&s_snapshot_lock_storage);
    if (!s_snapshot_lock) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = uart_apply_config(config, false);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_snapshot_lock);
        s_snapshot_lock = NULL;
        return err;
    }
    err = uart_driver_install(config->uart_num, LD6002_UART_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_snapshot_lock);
        s_snapshot_lock = NULL;
        return err;
    }

    if (config->rx_gpio >= 0) {
        esp_err_t isr_service_err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (isr_service_err == ESP_OK || isr_service_err == ESP_ERR_INVALID_STATE) {
            esp_err_t handler_err = gpio_isr_handler_add((gpio_num_t)config->rx_gpio,
                                                         ld6002_rx_edge_isr, NULL);
            if (handler_err == ESP_OK) {
                s_rx_edge_count = 0;
                s_rx_edge_handler_registered = true;
                s_rx_edge_monitor_active = true;
                rx_edge_monitor_restore(config->rx_gpio);
            } else {
                ESP_LOGW(TAG, "RX edge diagnostic handler failed: %s",
                         esp_err_to_name(handler_err));
            }
        } else {
            ESP_LOGW(TAG, "RX edge diagnostic ISR service failed: %s",
                     esp_err_to_name(isr_service_err));
        }
    }
    (void)uart_flush_input(config->uart_num);

    memcpy(&s_config, config, sizeof(s_config));
    s_recovery_requested = false;
    s_task = xTaskCreateStatic(ld6002_uart_task, "ld6002_uart", LD6002_TASK_STACK_SIZE,
                               &s_config, 5, s_task_stack, &s_task_tcb);
    if (!s_task) {
        (void)uart_driver_delete(config->uart_num);
        vSemaphoreDelete(s_snapshot_lock);
        s_snapshot_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    return ESP_OK;
}

esp_err_t ld6002_task_request_recovery(void)
{
    if (!s_started || !s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_recovery_requested = true;
    return ESP_OK;
}

void ld6002_get_snapshot(ld6002_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    if (!s_snapshot_lock) {
        memset(snapshot, 0, sizeof(*snapshot));
        return;
    }

    xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    memcpy(snapshot, &s_snapshot, sizeof(*snapshot));
    xSemaphoreGive(s_snapshot_lock);
}
