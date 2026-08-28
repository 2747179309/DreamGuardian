#include "status_light.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RGB_LEDC_MODE LEDC_LOW_SPEED_MODE
#define RGB_LEDC_TIMER LEDC_TIMER_1
#define RGB_LEDC_RED_CHANNEL LEDC_CHANNEL_3
#define RGB_LEDC_GREEN_CHANNEL LEDC_CHANNEL_4
#define RGB_LEDC_BLUE_CHANNEL LEDC_CHANNEL_5
#define RGB_LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define RGB_LEDC_FREQ_HZ 5000
#define RGB_LEDC_MAX_DUTY ((1U << 10) - 1U)

#define WS2812_RMT_RESOLUTION_HZ 10000000
#define WS2812_SLEEP_ASSIST_MIN_TX_INTERVAL_US 150000LL
#define WS2812_NIGHT_PATH_MIN_TX_INTERVAL_US 50000LL
static const char *TAG = "status_light";

static bool s_ready;
static bool s_active_low;
static bool s_ws2812_enabled;
static rmt_channel_handle_t s_ws2812_channel;
static rmt_encoder_handle_t s_ws2812_encoder;
static uint8_t *s_ws2812_pixels;
static size_t s_ws2812_count;
static int s_ws2812_gpio = -1;
static uint32_t s_ws2812_tx_count;
static esp_err_t s_ws2812_last_error = ESP_OK;
static uint8_t s_last_red;
static uint8_t s_last_green;
static uint8_t s_last_blue;
static bool s_ws2812_night_path_local_off;
static int64_t s_ws2812_sleep_assist_last_tx_us;
static int64_t s_ws2812_night_path_last_tx_us;

static uint8_t scale(uint8_t value, uint8_t percent)
{
    return (uint8_t)(((uint16_t)value * percent) / 100U);
}

static uint32_t value_to_duty(uint8_t value)
{
    uint32_t duty = ((uint32_t)value * RGB_LEDC_MAX_DUTY) / 255U;
    return s_active_low ? (RGB_LEDC_MAX_DUTY - duty) : duty;
}

static uint32_t value_brightness_to_duty(uint8_t value, float brightness)
{
    float scaled = (float)value * brightness;
    if (scaled < 0.0f) {
        scaled = 0.0f;
    }
    if (scaled > 255.0f) {
        scaled = 255.0f;
    }
    uint32_t duty = (uint32_t)((scaled * (float)RGB_LEDC_MAX_DUTY) / 255.0f + 0.5f);
    return s_active_low ? (RGB_LEDC_MAX_DUTY - duty) : duty;
}

static uint8_t brightness_to_value(uint8_t value, float brightness)
{
    float scaled = (float)value * brightness;
    if (scaled <= 0.0f) {
        return 0;
    }
    if (scaled >= 255.0f) {
        return 255;
    }
    return (uint8_t)(scaled + 0.5f);
}

static esp_err_t configure_channel(ledc_channel_t channel, int gpio_num)
{
    ledc_channel_config_t channel_config = {
        .gpio_num = gpio_num,
        .speed_mode = RGB_LEDC_MODE,
        .channel = channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = RGB_LEDC_TIMER,
        .duty = value_to_duty(0),
        .hpoint = 0,
        .flags = {
            .output_invert = 0,
        },
    };
    return ledc_channel_config(&channel_config);
}

static esp_err_t ws2812_init(int gpio_num, size_t led_count)
{
    ESP_RETURN_ON_FALSE(led_count > 0, ESP_ERR_INVALID_ARG, TAG, "invalid ws2812 count");

    s_ws2812_pixels = heap_caps_calloc(led_count * 3, sizeof(uint8_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_ws2812_pixels != NULL, ESP_ERR_NO_MEM, TAG, "ws2812 pixel alloc failed");

    rmt_tx_channel_config_t tx_config = {
        .gpio_num = gpio_num,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS2812_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_config, &s_ws2812_channel), TAG, "rmt tx channel failed");

    rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 3,
            .level1 = 0,
            .duration1 = 9,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 8,
            .level1 = 0,
            .duration1 = 4,
        },
        .flags = {
            .msb_first = 1,
        },
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&encoder_config, &s_ws2812_encoder), TAG, "rmt encoder failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_ws2812_channel), TAG, "rmt enable failed");
    (void)gpio_set_drive_capability((gpio_num_t)gpio_num, GPIO_DRIVE_CAP_0);
    s_ws2812_gpio = gpio_num;
    s_ws2812_count = led_count;
    s_ws2812_tx_count = 0;
    s_ws2812_last_error = ESP_OK;
    ESP_LOGI(TAG, "WS2812 status light initialized gpio=%d count=%u", gpio_num, (unsigned)led_count);
    return ESP_OK;
}

static esp_err_t ws2812_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_FALSE(s_ws2812_channel && s_ws2812_encoder && s_ws2812_pixels,
                        ESP_ERR_INVALID_STATE, TAG, "ws2812 not ready");

    for (size_t i = 0; i < s_ws2812_count; ++i) {
        size_t offset = i * 3;
        s_ws2812_pixels[offset + 0] = green;
        s_ws2812_pixels[offset + 1] = red;
        s_ws2812_pixels[offset + 2] = blue;
    }

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(s_ws2812_channel, s_ws2812_encoder,
                                 s_ws2812_pixels, s_ws2812_count * 3, &tx_config);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_ws2812_channel, 100);
    }
    s_ws2812_last_error = err;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws2812 tx failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ws2812_tx_count++;
    esp_rom_delay_us(80);
    return ESP_OK;
}

static esp_err_t ws2812_force_off_frames(size_t frame_count)
{
    ESP_RETURN_ON_FALSE(s_ws2812_channel && s_ws2812_encoder && s_ws2812_pixels,
                        ESP_ERR_INVALID_STATE, TAG, "ws2812 not ready");

    memset(s_ws2812_pixels, 0, s_ws2812_count * 3);
    (void)rmt_tx_wait_all_done(s_ws2812_channel, 100);

    esp_err_t last_err = ESP_OK;
    for (size_t i = 0; i < frame_count; ++i) {
        rmt_transmit_config_t tx_config = {
            .loop_count = 0,
        };
        last_err = rmt_transmit(s_ws2812_channel, s_ws2812_encoder,
                                s_ws2812_pixels, s_ws2812_count * 3, &tx_config);
        if (last_err == ESP_ERR_INVALID_STATE || last_err == ESP_ERR_TIMEOUT) {
            (void)rmt_disable(s_ws2812_channel);
            vTaskDelay(pdMS_TO_TICKS(1));
            (void)rmt_enable(s_ws2812_channel);
            last_err = rmt_transmit(s_ws2812_channel, s_ws2812_encoder,
                                    s_ws2812_pixels, s_ws2812_count * 3, &tx_config);
        }
        if (last_err == ESP_OK) {
            last_err = rmt_tx_wait_all_done(s_ws2812_channel, 200);
        }
        s_ws2812_last_error = last_err;
        if (last_err != ESP_OK) {
            ESP_LOGW(TAG, "ws2812 force off failed frame=%u: %s",
                     (unsigned)i, esp_err_to_name(last_err));
            printf("DG Light: ws2812 force off failed frame=%u err=%d\r\n",
                   (unsigned)i, (int)last_err);
            return last_err;
        }
        s_ws2812_tx_count++;
        esp_rom_delay_us(300);
    }
    esp_rom_delay_us(80);
    return ESP_OK;
}

static esp_err_t ws2812_set_rgb_smooth(uint8_t red, uint8_t green, uint8_t blue, float brightness)
{
    ESP_RETURN_ON_FALSE(s_ws2812_channel && s_ws2812_encoder && s_ws2812_pixels,
                        ESP_ERR_INVALID_STATE, TAG, "ws2812 not ready");

    int64_t now_us = esp_timer_get_time();
    if (s_ws2812_sleep_assist_last_tx_us > 0 &&
        now_us - s_ws2812_sleep_assist_last_tx_us < WS2812_SLEEP_ASSIST_MIN_TX_INTERVAL_US) {
        return ESP_OK;
    }

    bool changed = false;
    uint8_t out_red = brightness_to_value(red, brightness);
    uint8_t out_green = brightness_to_value(green, brightness);
    uint8_t out_blue = brightness_to_value(blue, brightness);
    for (size_t i = 0; i < s_ws2812_count; ++i) {
        size_t offset = i * 3;
        if (s_ws2812_pixels[offset + 0] != out_green ||
            s_ws2812_pixels[offset + 1] != out_red ||
            s_ws2812_pixels[offset + 2] != out_blue) {
            changed = true;
            s_ws2812_pixels[offset + 0] = out_green;
            s_ws2812_pixels[offset + 1] = out_red;
            s_ws2812_pixels[offset + 2] = out_blue;
        }
    }

    if (!changed) {
        return ESP_OK;
    }

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(s_ws2812_channel, s_ws2812_encoder,
                                 s_ws2812_pixels, s_ws2812_count * 3, &tx_config);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_ws2812_channel, 100);
    }
    s_ws2812_last_error = err;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws2812 smooth tx failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ws2812_tx_count++;
    s_ws2812_sleep_assist_last_tx_us = now_us;
    esp_rom_delay_us(80);
    return ESP_OK;
}

static esp_err_t set_channel(ledc_channel_t channel, uint8_t value)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(RGB_LEDC_MODE, channel, value_to_duty(value)),
                        TAG, "set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(RGB_LEDC_MODE, channel),
                        TAG, "update duty failed");
    return ESP_OK;
}

static esp_err_t set_channel_duty(ledc_channel_t channel, uint32_t duty)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(RGB_LEDC_MODE, channel, duty),
                        TAG, "set precise duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(RGB_LEDC_MODE, channel),
                        TAG, "update precise duty failed");
    return ESP_OK;
}

esp_err_t status_light_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (red == s_last_red && green == s_last_green && blue == s_last_blue) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(set_channel(RGB_LEDC_RED_CHANNEL, red), TAG, "set red failed");
    ESP_RETURN_ON_ERROR(set_channel(RGB_LEDC_GREEN_CHANNEL, green), TAG, "set green failed");
    ESP_RETURN_ON_ERROR(set_channel(RGB_LEDC_BLUE_CHANNEL, blue), TAG, "set blue failed");
    s_ws2812_night_path_local_off = false;

    if (s_ws2812_enabled) {
        ESP_RETURN_ON_ERROR(ws2812_set_rgb(red, green, blue), TAG, "set ws2812 failed");
    }

    s_last_red = red;
    s_last_green = green;
    s_last_blue = blue;
    s_ws2812_night_path_last_tx_us = 0;
    ESP_LOGI(TAG, "rgb=%u,%u,%u", red, green, blue);
    return ESP_OK;
}

esp_err_t status_light_force_off(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    esp_err_t step_err = set_channel_duty(RGB_LEDC_RED_CHANNEL, value_to_duty(0));
    if (step_err != ESP_OK && err == ESP_OK) {
        err = step_err;
    }
    step_err = set_channel_duty(RGB_LEDC_GREEN_CHANNEL, value_to_duty(0));
    if (step_err != ESP_OK && err == ESP_OK) {
        err = step_err;
    }
    step_err = set_channel_duty(RGB_LEDC_BLUE_CHANNEL, value_to_duty(0));
    if (step_err != ESP_OK && err == ESP_OK) {
        err = step_err;
    }

    if (s_ws2812_enabled) {
        step_err = ws2812_force_off_frames(4);
        if (step_err != ESP_OK && err == ESP_OK) {
            err = step_err;
        }
    }

    s_last_red = 0;
    s_last_green = 0;
    s_last_blue = 0;
    s_ws2812_night_path_local_off = false;
    s_ws2812_sleep_assist_last_tx_us = 0;
    s_ws2812_night_path_last_tx_us = 0;
    printf("DG Light: force off err=%d ws2812_err=%d tx=%u\r\n",
           (int)err, (int)s_ws2812_last_error, (unsigned)s_ws2812_tx_count);
    return err;
}

esp_err_t status_light_init(const status_light_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "null config");

    s_active_low = config->active_low;

    ledc_timer_config_t timer_config = {
        .speed_mode = RGB_LEDC_MODE,
        .duty_resolution = RGB_LEDC_DUTY_RES,
        .timer_num = RGB_LEDC_TIMER,
        .freq_hz = RGB_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG, "timer config failed");
    ESP_RETURN_ON_ERROR(configure_channel(RGB_LEDC_RED_CHANNEL, config->red_gpio), TAG, "red channel failed");
    ESP_RETURN_ON_ERROR(configure_channel(RGB_LEDC_GREEN_CHANNEL, config->green_gpio), TAG, "green channel failed");
    ESP_RETURN_ON_ERROR(configure_channel(RGB_LEDC_BLUE_CHANNEL, config->blue_gpio), TAG, "blue channel failed");

    if (config->ws2812_enabled) {
        ESP_RETURN_ON_ERROR(ws2812_init(config->ws2812_gpio, config->ws2812_count),
                            TAG, "ws2812 init failed");
        s_ws2812_enabled = true;
    } else {
        s_ws2812_enabled = false;
    }

    s_ready = true;
    s_last_red = 1;
    s_last_green = 1;
    s_last_blue = 1;

    ESP_LOGI(TAG, "status light ready");
    return ESP_OK;
}


static esp_err_t sleep_assist_set_precise(uint8_t red, uint8_t green, uint8_t blue,
                                          float brightness, bool include_ws2812)
{
    if (brightness < 0.0f) {
        brightness = 0.0f;
    }
    if (brightness > 1.0f) {
        brightness = 1.0f;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(set_channel_duty(RGB_LEDC_RED_CHANNEL, value_brightness_to_duty(red, brightness)),
                        TAG, "set red precise failed");
    ESP_RETURN_ON_ERROR(set_channel_duty(RGB_LEDC_GREEN_CHANNEL, value_brightness_to_duty(green, brightness)),
                        TAG, "set green precise failed");
    ESP_RETURN_ON_ERROR(set_channel_duty(RGB_LEDC_BLUE_CHANNEL, value_brightness_to_duty(blue, brightness)),
                        TAG, "set blue precise failed");
    s_ws2812_night_path_local_off = false;

    uint8_t out_red = brightness_to_value(red, brightness);
    uint8_t out_green = brightness_to_value(green, brightness);
    uint8_t out_blue = brightness_to_value(blue, brightness);
    bool force_ws2812_off = include_ws2812 && s_ws2812_enabled &&
                            brightness <= 0.0f && red == 0 && green == 0 && blue == 0;
    if (include_ws2812 && s_ws2812_enabled) {
        if (force_ws2812_off) {
            ESP_RETURN_ON_ERROR(ws2812_set_rgb(0, 0, 0), TAG, "set ws2812 off failed");
        } else {
            ESP_RETURN_ON_ERROR(ws2812_set_rgb_smooth(red, green, blue, brightness),
                                TAG, "set ws2812 smooth precise failed");
        }
    }
    if (include_ws2812 || !s_ws2812_enabled) {
        s_last_red = out_red;
        s_last_green = out_green;
        s_last_blue = out_blue;
        s_ws2812_night_path_last_tx_us = 0;
        if (force_ws2812_off) {
            s_ws2812_sleep_assist_last_tx_us = 0;
        }
    }
    return ESP_OK;
}

esp_err_t status_light_sleep_assist_set(uint8_t red, uint8_t green, uint8_t blue, float brightness)
{
    return sleep_assist_set_precise(red, green, blue, brightness, true);
}

esp_err_t status_light_sleep_assist_set_local(uint8_t red, uint8_t green, uint8_t blue, float brightness)
{
    return sleep_assist_set_precise(red, green, blue, brightness, false);
}

esp_err_t status_light_ws2812_night_path(float progress, float max_brightness)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (progress < 0.0f) {
        progress = 0.0f;
    }
    if (progress > 1.0f) {
        progress = 1.0f;
    }
    if (max_brightness < 0.0f) {
        max_brightness = 0.0f;
    }
    if (max_brightness > 0.06f) {
        max_brightness = 0.06f;
    }

    if (!s_ws2812_night_path_local_off) {
        ESP_RETURN_ON_ERROR(set_channel_duty(RGB_LEDC_RED_CHANNEL, value_to_duty(0)),
                            TAG, "night path red off failed");
        ESP_RETURN_ON_ERROR(set_channel_duty(RGB_LEDC_GREEN_CHANNEL, value_to_duty(0)),
                            TAG, "night path green off failed");
        ESP_RETURN_ON_ERROR(set_channel_duty(RGB_LEDC_BLUE_CHANNEL, value_to_duty(0)),
                            TAG, "night path blue off failed");
        s_ws2812_night_path_local_off = true;
    }

    if (!s_ws2812_enabled || !s_ws2812_pixels || s_ws2812_count == 0) {
        return ESP_OK;
    }

    int64_t now_us = esp_timer_get_time();
    if (s_ws2812_night_path_last_tx_us > 0 &&
        now_us - s_ws2812_night_path_last_tx_us < WS2812_NIGHT_PATH_MIN_TX_INTERVAL_US) {
        return ESP_OK;
    }

    float lit = progress * ((float)s_ws2812_count + 3.0f);
    bool changed = false;
    uint8_t max_red = 0;
    uint8_t max_green = 0;
    for (size_t i = 0; i < s_ws2812_count; ++i) {
        float local = lit - (float)i;
        if (local < 0.0f) {
            local = 0.0f;
        } else if (local > 1.0f) {
            local = 1.0f;
        }
        float level = local * local * (3.0f - 2.0f * local);
        float brightness = max_brightness * level;
        uint8_t red = brightness_to_value(255, brightness);
        uint8_t green = brightness_to_value(120, brightness);
        uint8_t blue = brightness_to_value(24, brightness);
        size_t offset = i * 3;
        if (s_ws2812_pixels[offset + 0] != green ||
            s_ws2812_pixels[offset + 1] != red ||
            s_ws2812_pixels[offset + 2] != blue) {
            changed = true;
            s_ws2812_pixels[offset + 0] = green;
            s_ws2812_pixels[offset + 1] = red;
            s_ws2812_pixels[offset + 2] = blue;
        }
        if (red > max_red) {
            max_red = red;
        }
        if (green > max_green) {
            max_green = green;
        }
    }

    if (!changed) {
        return ESP_OK;
    }

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(s_ws2812_channel, s_ws2812_encoder,
                                 s_ws2812_pixels, s_ws2812_count * 3, &tx_config);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_ws2812_channel, 100);
    }
    s_ws2812_last_error = err;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "night path ws2812 tx failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ws2812_tx_count++;
    s_ws2812_night_path_last_tx_us = now_us;
    s_last_red = max_red;
    s_last_green = max_green;
    s_last_blue = 1;
    esp_rom_delay_us(80);
    return ESP_OK;
}

void status_light_get_diag(status_light_diag_t *diag)
{
    if (!diag) {
        return;
    }
    memset(diag, 0, sizeof(*diag));
    diag->ready = s_ready;
    diag->ws2812_enabled = s_ws2812_enabled;
    diag->ws2812_gpio = s_ws2812_gpio;
    diag->ws2812_count = s_ws2812_count;
    diag->ws2812_tx_count = s_ws2812_tx_count;
    diag->ws2812_last_error = s_ws2812_last_error;
    diag->last_red = s_last_red;
    diag->last_green = s_last_green;
    diag->last_blue = s_last_blue;
}

void status_light_self_test(void)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "self-test skipped: light not ready");
        return;
    }

    ESP_LOGI(TAG, "self-test: red/green/blue/white and brightness steps");
    const struct {
        uint8_t red;
        uint8_t green;
        uint8_t blue;
        const char *name;
    } colors[] = {
        {255, 0, 0, "red"},
        {0, 255, 0, "green"},
        {0, 0, 255, "blue"},
        {255, 255, 255, "white"},
    };

    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        ESP_LOGI(TAG, "self-test color=%s", colors[i].name);
        status_light_set_rgb(colors[i].red, colors[i].green, colors[i].blue);
        vTaskDelay(pdMS_TO_TICKS(450));
    }

    const uint8_t levels[] = {8, 25, 55, 100, 55, 25, 8};
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i) {
        uint8_t level = levels[i];
        ESP_LOGI(TAG, "self-test warm brightness=%u%%", level);
        status_light_set_rgb(scale(255, level), scale(120, level), scale(20, level));
        vTaskDelay(pdMS_TO_TICKS(280));
    }

    status_light_set_rgb(0, 0, 0);
}

void status_light_apply_decision(const intervention_decision_t *decision)
{
    if (!s_ready || decision == NULL) {
        return;
    }

    uint8_t brightness = decision->brightness_percent;
    if (brightness > 60) {
        brightness = 60;
    }

    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    switch (decision->action) {
    case INTERVENTION_ACTION_WARM_LIGHT:
    case INTERVENTION_ACTION_STEREO_BREATHING:
    case INTERVENTION_ACTION_PINK_NOISE:
    case INTERVENTION_ACTION_REDUCE_AROUSAL:
        red = scale(255, brightness);
        green = scale(120, brightness);
        blue = scale(20, brightness);
        break;
    case INTERVENTION_ACTION_NIGHT_PATH:
        red = scale(255, brightness);
        green = scale(80, brightness);
        blue = 0;
        break;
    case INTERVENTION_ACTION_WAKE_MUSIC:
        red = scale(255, brightness);
        green = scale(180, brightness);
        blue = scale(80, brightness);
        break;
    case INTERVENTION_ACTION_NONE:
    default:
        break;
    }

    esp_err_t err = status_light_set_rgb(red, green, blue);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rgb update failed: %s", esp_err_to_name(err));
    }
}
