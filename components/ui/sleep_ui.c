#include "sleep_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "time_sync.h"

#if CONFIG_DG_ENABLE_BOX_DISPLAY
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/i2c.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_tt21100.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#endif

static const char *TAG = "sleep_ui";

#if CONFIG_DG_ENABLE_BOX_DISPLAY
static bool s_display_ready;
static bool s_dashboard_visible;
static bool s_assist_visible;
static bool s_clock_mode_visible;
static bool s_sleep_locked_clock_visible;
static bool s_solid_color_visible;
static uint32_t s_solid_color_rgb;
static sleep_ui_command_cb_t s_command_cb;
static void *s_command_ctx;
static lv_disp_t *s_display;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_touch_handle_t s_touch;
static lv_indev_t *s_touch_indev;
static lv_obj_t *s_title;
static lv_obj_t *s_clock;
static lv_obj_t *s_breath;
static lv_obj_t *s_heart;
static lv_obj_t *s_range;
static lv_obj_t *s_state;
static lv_obj_t *s_score;
static lv_obj_t *s_radar;
static lv_obj_t *s_voice;
static lv_obj_t *s_decision;
static lv_obj_t *s_assist_orb;
static lv_obj_t *s_assist_ring;
static lv_obj_t *s_assist_text;
static lv_obj_t *s_clock_big;
static lv_obj_t *s_clock_date;
static lv_obj_t *s_clock_status;
static bool s_touch_ready;
static int s_last_error;
static uint32_t s_touch_press_count;
static uint32_t s_touch_command_count;
static int16_t s_touch_last_x = -1;
static int16_t s_touch_last_y = -1;
static mobile_app_command_t s_touch_last_command;
static char s_touch_last_target[16] = "none";
static uint32_t s_touch_last_command_ms;
static bool s_sleep_mode_active;
static uint32_t s_touch_last_tap_ms;
static int16_t s_touch_last_tap_x = -1;
static int16_t s_touch_last_tap_y = -1;
static int64_t s_last_assist_brightness_log_us;
static char s_solid_label[24];
static uint32_t s_scene_bg = 0x050201;
static uint32_t s_scene_orb = 0xB03A00;
static uint32_t s_scene_ring = 0xE07018;
static uint32_t s_scene_text = 0xD89048;

#define TOUCH_COMMAND_DEBOUNCE_MS 900
#define TOUCH_DOUBLE_TAP_MS 550
#define TOUCH_DOUBLE_TAP_MAX_DELTA 55
#define SENSOR_STATUS_TOUCH_HOLD_MS 1500

static lv_color_t ui_color(uint32_t rgb)
{
    return lv_color_hex(rgb);
}
static const lv_font_t *font_value(void)
{
#if LV_FONT_MONTSERRAT_32
    return &lv_font_montserrat_32;
#elif LV_FONT_MONTSERRAT_24
    return &lv_font_montserrat_24;
#else
    return &lv_font_montserrat_14;
#endif
}

static const lv_font_t *font_ui(void)
{
#if LV_FONT_SIMSUN_16_CJK
    return &lv_font_simsun_16_cjk;
#elif LV_FONT_UNSCII_16
    return &lv_font_unscii_16;
#elif LV_FONT_MONTSERRAT_16
    return &lv_font_montserrat_16;
#else
    return &lv_font_montserrat_14;
#endif
}

static const lv_font_t *font_button(void)
{
#if LV_FONT_MONTSERRAT_24
    return &lv_font_montserrat_24;
#elif LV_FONT_MONTSERRAT_20
    return &lv_font_montserrat_20;
#else
    return font_ui();
#endif
}

static const lv_font_t *font_clock_date(void)
{
#if LV_FONT_MONTSERRAT_20
    return &lv_font_montserrat_20;
#else
    return font_ui();
#endif
}

static void reset_screen_object_refs(void)
{
    s_title = NULL;
    s_clock = NULL;
    s_breath = NULL;
    s_heart = NULL;
    s_range = NULL;
    s_state = NULL;
    s_score = NULL;
    s_radar = NULL;
    s_voice = NULL;
    s_decision = NULL;
    s_assist_orb = NULL;
    s_assist_ring = NULL;
    s_assist_text = NULL;
    s_clock_big = NULL;
    s_clock_date = NULL;
    s_clock_status = NULL;
}

static void clean_screen(lv_obj_t *scr)
{
    lv_obj_clean(scr);
    reset_screen_object_refs();
}

static esp_err_t probe_touch_i2c_addr(uint8_t addr)
{
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    if (i2c_master_cmd_begin(BSP_I2C_NUM, cmd, pdMS_TO_TICKS(1000)) == ESP_OK) {
        ret = ESP_OK;
    }
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t touch_new_legacy_i2c_compat(esp_lcd_touch_handle_t *ret_touch)
{
    if (!ret_touch) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_touch = NULL;
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C init failed");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    bool tt21100 = false;
    esp_lcd_panel_io_i2c_config_t tp_io_config;
    if (probe_touch_i2c_addr(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS) == ESP_OK) {
        esp_lcd_panel_io_i2c_config_t config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        memcpy(&tp_io_config, &config, sizeof(config));
    } else if (probe_touch_i2c_addr(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP) == ESP_OK) {
        esp_lcd_panel_io_i2c_config_t config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        memcpy(&tp_io_config, &config, sizeof(config));
    } else if (probe_touch_i2c_addr(ESP_LCD_TOUCH_IO_I2C_TT21100_ADDRESS) == ESP_OK) {
        esp_lcd_panel_io_i2c_config_t config = ESP_LCD_TOUCH_IO_I2C_TT21100_CONFIG();
        memcpy(&tp_io_config, &config, sizeof(config));
        tp_cfg.flags.mirror_x = 1;
        tt21100 = true;
    } else {
        return ESP_ERR_NOT_FOUND;
    }

    /* IDF 5.5 legacy I2C panel IO rejects a nonzero scl_speed_hz. */
    tp_io_config.scl_speed_hz = 0;

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)BSP_I2C_NUM,
                                                 &tp_io_config, &tp_io_handle),
                        TAG, "touch panel IO failed");
    if (tt21100) {
        return esp_lcd_touch_new_i2c_tt21100(tp_io_handle, &tp_cfg, ret_touch);
    }
    return esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, ret_touch);
}

static esp_err_t init_display_with_touch(void)
{
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    bsp_display_config_t display_cfg = {
        .max_transfer_sz = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(bsp_display_new(&display_cfg, &s_panel, &s_panel_io),
                        TAG, "LCD panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true),
                        TAG, "LCD panel on failed");

    const lvgl_port_display_cfg_t port_display_cfg = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
#if CONFIG_BSP_LCD_DRAW_BUF_DOUBLE
        .double_buffer = true,
#else
        .double_buffer = false,
#endif
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        },
    };

    s_display = lvgl_port_add_disp(&port_display_cfg);
    ESP_RETURN_ON_FALSE(s_display != NULL, ESP_FAIL, TAG, "LVGL display add failed");

    esp_err_t touch_err = touch_new_legacy_i2c_compat(&s_touch);
    if (touch_err == ESP_OK && s_touch) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_display,
            .handle = s_touch,
        };
        s_touch_indev = lvgl_port_add_touch(&touch_cfg);
        if (!s_touch_indev) {
            ESP_LOGW(TAG, "LVGL touch input add failed");
        }
        s_touch_ready = s_touch_indev != NULL;
        (void)esp_lcd_touch_set_swap_xy(s_touch, false);
        (void)esp_lcd_touch_set_mirror_x(s_touch, true);
        (void)esp_lcd_touch_set_mirror_y(s_touch, true);
    } else {
        ESP_LOGW(TAG, "BOX touch init skipped: %s", esp_err_to_name(touch_err));
        s_last_error = touch_err;
    }

    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(100), TAG, "LCD brightness failed");
    return ESP_OK;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                            lv_coord_t width, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, ui_color(color), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(label, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(label, ui_color(0x050201), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    lv_label_set_text(label, text);
    return label;
}

static void record_touch_event(lv_event_t *event, const char *target,
                               mobile_app_command_t command, bool command_sent)
{
    lv_point_t point = {
        .x = -1,
        .y = -1,
    };
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev) {
        lv_indev_get_point(indev, &point);
    }
    s_touch_press_count++;
    if (command_sent) {
        s_touch_command_count++;
    }
    s_touch_last_x = (int16_t)point.x;
    s_touch_last_y = (int16_t)point.y;
    s_touch_last_command = command;
    snprintf(s_touch_last_target, sizeof(s_touch_last_target), "%s", target ? target : "unknown");
}

static void record_touch_point(const lv_point_t *point, const char *target,
                               mobile_app_command_t command, bool command_sent)
{
    s_touch_press_count++;
    if (command_sent) {
        s_touch_command_count++;
    }
    s_touch_last_x = point ? (int16_t)point->x : -1;
    s_touch_last_y = point ? (int16_t)point->y : -1;
    s_touch_last_command = command;
    snprintf(s_touch_last_target, sizeof(s_touch_last_target), "%s", target ? target : "unknown");
}

static void dispatch_sleep_command(const lv_point_t *point, const char *target,
                                   mobile_app_command_t command)
{
    uint32_t now_ms = lv_tick_get();
    if (s_touch_last_command_ms != 0 &&
        (uint32_t)(now_ms - s_touch_last_command_ms) < TOUCH_COMMAND_DEBOUNCE_MS) {
        record_touch_point(point, target, command, false);
        return;
    }

    s_touch_last_command_ms = now_ms;
    record_touch_point(point, target, command, true);
    printf("DG Touch: %s command=%d at %d,%d\r\n",
           target ? target : "sleep_tap",
           (int)command,
           point ? (int)point->x : -1,
           point ? (int)point->y : -1);
    if (s_command_cb) {
        (void)s_command_cb(command, s_command_ctx);
    }
}

static void dispatch_sleep_tap_stop(const lv_point_t *point, const char *target)
{
    dispatch_sleep_command(point, target, MOBILE_APP_COMMAND_STOP);
}

static bool dispatch_sleep_double_tap(lv_event_t *event, const lv_point_t *point)
{
    if (!s_sleep_mode_active || !point || !s_command_cb) {
        return false;
    }
    uint32_t now_ms = lv_tick_get();
    bool close_enough = false;
    if (s_touch_last_tap_ms != 0 &&
        (uint32_t)(now_ms - s_touch_last_tap_ms) <= TOUCH_DOUBLE_TAP_MS) {
        int dx = (int)point->x - (int)s_touch_last_tap_x;
        int dy = (int)point->y - (int)s_touch_last_tap_y;
        close_enough = dx * dx + dy * dy <= TOUCH_DOUBLE_TAP_MAX_DELTA * TOUCH_DOUBLE_TAP_MAX_DELTA;
    }

    s_touch_last_tap_ms = now_ms;
    s_touch_last_tap_x = (int16_t)point->x;
    s_touch_last_tap_y = (int16_t)point->y;

    if (!close_enough) {
        return false;
    }

    record_touch_point(point, "double_tap_stop", MOBILE_APP_COMMAND_STOP, true);
    s_touch_last_command_ms = now_ms;
    printf("DG Touch: double tap exit sleep at %d,%d\r\n", (int)point->x, (int)point->y);
    (void)s_command_cb(MOBILE_APP_COMMAND_STOP, s_command_ctx);
    s_touch_last_tap_ms = 0;
    return true;
}

static void screen_touch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) {
        return;
    }

    lv_point_t point = {
        .x = -1,
        .y = -1,
    };
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev) {
        lv_indev_get_point(indev, &point);
    }
    if (s_sleep_mode_active && s_assist_visible) {
        /* BOX-3 reports the visible left half as high X. The assist screen
         * therefore maps high X to the left EXIT button and low X to the
         * right DEEP SLEEP button. */
        if (point.x >= BSP_LCD_H_RES / 2) {
            dispatch_sleep_command(&point, "assist_exit", MOBILE_APP_COMMAND_STOP);
        } else {
            dispatch_sleep_command(&point, "assist_deep", MOBILE_APP_COMMAND_SLEPT);
        }
        return;
    }
    if (s_sleep_mode_active) {
        dispatch_sleep_tap_stop(&point, "sleep_tap_stop");
        return;
    }
    if (s_solid_color_visible) {
        record_touch_point(&point, "solid_tap_ignored", MOBILE_APP_COMMAND_NONE, false);
        printf("DG Touch: solid screen tap ignored at %d,%d\r\n", (int)point.x, (int)point.y);
        return;
    }

    mobile_app_command_t command = MOBILE_APP_COMMAND_NONE;
    const char *status = "TOUCH";
    const char *target = "screen";
    /* BOX-3 touch coordinates are mirrored against the visible landscape UI:
       touching the left button reports a high X value. Keep the visual layout
       left-to-right, but dispatch zones using the observed coordinate space. */
    if (point.x >= (BSP_LCD_H_RES * 2) / 3) {
        command = MOBILE_APP_COMMAND_SLEEP;
        status = "SLEEP";
        target = "zone_sleep";
    } else if (point.x >= BSP_LCD_H_RES / 3) {
        command = MOBILE_APP_COMMAND_STOP;
        status = "STOP";
        target = "zone_stop";
    } else {
        command = MOBILE_APP_COMMAND_SELF_TEST;
        status = "TEST";
        target = "zone_test";
    }

    record_touch_event(event, target, command, command != MOBILE_APP_COMMAND_NONE);
    if (s_clock_mode_visible && s_clock_status) {
        lv_label_set_text_fmt(s_clock_status, "%s %d,%d",
                              status, (int)s_touch_last_x, (int)s_touch_last_y);
    }
    if (command != MOBILE_APP_COMMAND_NONE && s_command_cb) {
        uint32_t now_ms = lv_tick_get();
        if (s_touch_last_command_ms != 0 &&
            (uint32_t)(now_ms - s_touch_last_command_ms) < TOUCH_COMMAND_DEBOUNCE_MS) {
            return;
        }
        s_touch_last_command_ms = now_ms;
        (void)s_command_cb(command, s_command_ctx);
    }
}

static void touch_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) {
        return;
    }

    lv_point_t point = {
        .x = -1,
        .y = -1,
    };
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev) {
        lv_indev_get_point(indev, &point);
    }
    mobile_app_command_t command = (mobile_app_command_t)(uintptr_t)lv_event_get_user_data(event);
    if (s_sleep_mode_active && s_assist_visible &&
        (command == MOBILE_APP_COMMAND_STOP || command == MOBILE_APP_COMMAND_SLEPT)) {
        dispatch_sleep_command(&point,
                               command == MOBILE_APP_COMMAND_SLEPT ? "assist_deep" : "assist_exit",
                               command);
        return;
    }
    if (s_sleep_mode_active) {
        dispatch_sleep_tap_stop(&point, "sleep_button_tap_stop");
        return;
    }
    if (dispatch_sleep_double_tap(event, &point)) {
        return;
    }

    const char *status = "SENT";
    const char *target = "button";
    if (command == MOBILE_APP_COMMAND_SLEEP) {
        status = "SENT: SLEEP";
        target = "sleep";
    } else if (command == MOBILE_APP_COMMAND_STOP) {
        status = "SENT: STOP";
        target = "stop";
    } else if (command == MOBILE_APP_COMMAND_SELF_TEST) {
        status = "SENT: TEST";
        target = "test";
    } else if (command == MOBILE_APP_COMMAND_SLEPT) {
        status = "SENT: DEEP";
        target = "deep";
    }
    record_touch_event(event, target, command, true);

    if (s_clock_mode_visible && s_clock_status) {
        lv_label_set_text(s_clock_status, status);
    }
    if (s_command_cb) {
        uint32_t now_ms = lv_tick_get();
        if (s_touch_last_command_ms != 0 &&
            (uint32_t)(now_ms - s_touch_last_command_ms) < TOUCH_COMMAND_DEBOUNCE_MS) {
            return;
        }
        s_touch_last_command_ms = now_ms;
        (void)s_command_cb(command, s_command_ctx);
    }
}

static lv_obj_t *make_touch_button(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                                   lv_coord_t width, uint32_t bg, uint32_t fg,
                                   mobile_app_command_t command)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 66);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(button, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, ui_color(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, ui_color(fg), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(button, touch_button_event_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)command);

    lv_obj_t *label = lv_label_create(button);
    bool cjk_text = false;
    for (const unsigned char *p = (const unsigned char *)text; p && *p; ++p) {
        if (*p >= 0x80) {
            cjk_text = true;
            break;
        }
    }
    lv_obj_set_style_text_font(label, cjk_text ? font_ui() : font_button(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, ui_color(fg), LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static void create_dashboard(void)
{
    lv_obj_t *scr = lv_scr_act();
    clean_screen(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, ui_color(0x050201), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    s_title = make_label(scr, "DREAMGUARDIAN", 8, 6, 190, font_ui(), 0xFFD0A0);
    s_clock = make_label(scr, "--:--", 250, 6, 64, font_ui(), 0xFFD0A0);

    s_breath = make_label(scr, "B ---", 8, 36, 100, font_value(), 0xFFD0A0);
    s_heart = make_label(scr, "H ---", 112, 36, 100, font_value(), 0xFFD0A0);
    s_range = make_label(scr, "R ---", 216, 36, 100, font_value(), 0xFFD0A0);

    s_state = make_label(scr, "STATE ----------", 8, 88, 304, font_ui(), 0xFF8A30);
    s_score = make_label(scr, "SCORE ---  RISK ---  SSI ---", 8, 116, 304, font_ui(), 0xFFD0A0);
    s_radar = make_label(scr, "RX ----- RAW ----- OK -----", 8, 144, 304, font_ui(), 0xFFD0A0);
    s_decision = make_label(scr, "SKILL --------------------", 8, 172, 304, font_ui(), 0xFFD0A0);
    s_voice = make_label(scr, "MODE ---------- MOT --.--", 8, 204, 304, font_ui(), 0xFFD0A0);

    s_assist_visible = false;
    s_clock_mode_visible = false;
    s_sleep_locked_clock_visible = false;
    s_solid_color_visible = false;
    s_dashboard_visible = true;
}

static lv_obj_t *make_diag_rect(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                lv_coord_t w, lv_coord_t h, uint32_t color)
{
    lv_obj_t *rect = lv_obj_create(parent);
    lv_obj_clear_flag(rect, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(rect, x, y);
    lv_obj_set_size(rect, w, h);
    lv_obj_set_style_bg_color(rect, ui_color(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(rect, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(rect, 0, LV_PART_MAIN);
    return rect;
}

static void create_display_diagnostic(void)
{
    lv_obj_t *scr = lv_scr_act();
    clean_screen(scr);
    lv_obj_set_style_bg_color(scr, ui_color(0x050201), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    make_diag_rect(scr, 0, 0, 80, 60, 0x4A1204);
    make_diag_rect(scr, 80, 0, 80, 60, 0xA83A08);
    make_diag_rect(scr, 160, 0, 80, 60, 0xFF6A00);
    make_diag_rect(scr, 240, 0, 80, 60, 0xFFD0A0);

    for (int x = 0; x < BSP_LCD_H_RES; x += 20) {
        make_diag_rect(scr, x, 70, 1, 118, 0xFFD0A0);
    }
    for (int y = 70; y < 188; y += 20) {
        make_diag_rect(scr, 0, y, BSP_LCD_H_RES, 1, 0xFFD0A0);
    }

    make_label(scr, "DISPLAY MODE", 64, 196, 210, font_value(), 0xFFD0A0);
    s_dashboard_visible = false;
    s_assist_visible = false;
    s_clock_mode_visible = false;
    s_sleep_locked_clock_visible = false;
    s_solid_color_visible = false;
}
#endif

void sleep_ui_init(void)
{
#if CONFIG_DG_ENABLE_BOX_DISPLAY
    esp_err_t err = init_display_with_touch();
    printf("DG LCD: init err=%s panel=%d touch=%d\r\n",
           esp_err_to_name(err), s_panel != NULL ? 1 : 0, s_touch != NULL ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BOX display init failed: %s; serial UI remains active", esp_err_to_name(err));
        s_display_ready = false;
        return;
    }

    if (bsp_display_lock(0)) {
        s_display_ready = true;
        bsp_display_unlock();
        esp_err_t clock_err = sleep_ui_show_clock_mode();
        printf("DG LCD: dashboard ready=1 clock_err=%s\r\n", esp_err_to_name(clock_err));
        ESP_LOGI(TAG, "BOX display dashboard initialized with touch input");
    } else {
        printf("DG LCD: dashboard ready=0 lock_failed\r\n");
        ESP_LOGE(TAG, "BOX display lock failed during init");
    }
#else
    ESP_LOGI(TAG, "UI placeholder initialized; enable CONFIG_DG_ENABLE_BOX_DISPLAY for screen dashboard");
#endif
}

void sleep_ui_register_command_callback(sleep_ui_command_cb_t cb, void *ctx)
{
#if CONFIG_DG_ENABLE_BOX_DISPLAY
    s_command_cb = cb;
    s_command_ctx = ctx;
#else
    (void)cb;
    (void)ctx;
#endif
}

void sleep_ui_set_sleep_mode_active(bool active)
{
#if CONFIG_DG_ENABLE_BOX_DISPLAY
    if (s_sleep_mode_active != active) {
        s_touch_last_tap_ms = 0;
    }
    s_sleep_mode_active = active;
#else
    (void)active;
#endif
}

void sleep_ui_get_status(sleep_ui_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
#if CONFIG_DG_ENABLE_BOX_DISPLAY
    status->display_ready = s_display_ready;
    status->touch_ready = s_touch_ready;
    status->touch_indev_ready = s_touch_indev != NULL;
    status->last_error = s_last_error;
    status->press_count = s_touch_press_count;
    status->command_count = s_touch_command_count;
    status->last_x = s_touch_last_x;
    status->last_y = s_touch_last_y;
    status->last_command = s_touch_last_command;
    snprintf(status->last_target, sizeof(status->last_target), "%s", s_touch_last_target);
#else
    status->last_error = ESP_ERR_NOT_SUPPORTED;
    snprintf(status->last_target, sizeof(status->last_target), "disabled");
#endif
}

void sleep_ui_self_test(void)
{
#if CONFIG_DG_ENABLE_BOX_DISPLAY
    if (!s_display_ready) {
        ESP_LOGW(TAG, "display self-test skipped: display not ready");
        return;
    }
    if (bsp_display_lock(0)) {
        create_display_diagnostic();
        bsp_display_unlock();
        ESP_LOGI(TAG, "display self-test ready: color blocks and 1px grid");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
#else
    ESP_LOGI(TAG, "display self-test skipped: CONFIG_DG_ENABLE_BOX_DISPLAY is off");
#endif
}

esp_err_t sleep_ui_set_scene_preset(const char *preset)
{
#if CONFIG_DG_ENABLE_BOX_DISPLAY
    if (preset && strcmp(preset, "empty") == 0) {
        s_scene_bg = 0x020202;
        s_scene_orb = 0x2A2A2A;
        s_scene_ring = 0x666666;
        s_scene_text = 0x999999;
    } else {
        s_scene_bg = 0x050201;
        s_scene_orb = 0xB03A00;
        s_scene_ring = 0xE07018;
        s_scene_text = 0xD89048;
    }
    s_assist_visible = false;
    s_sleep_locked_clock_visible = false;
    ESP_LOGI(TAG, "scene preset=%s", preset ? preset : "ppm");
    return ESP_OK;
#else
    (void)preset;
    return ESP_OK;
#endif
}


esp_err_t sleep_ui_sleep_assist_show(float phase, float brightness, bool text_visible)
{
#if CONFIG_DG_ENABLE_BOX_DISPLAY
    if (!s_display_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness < 0.0f) {
        brightness = 0.0f;
    }
    if (brightness > 1.0f) {
        brightness = 1.0f;
    }
    int backlight = (int)(32.0f + brightness * 68.0f);
    if (backlight < 0) {
        backlight = 0;
    }
    if (backlight > 100) {
        backlight = 100;
    }
    (void)bsp_display_brightness_set(backlight);
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_assist_brightness_log_us > 3000000LL) {
        s_last_assist_brightness_log_us = now_us;
        printf("DG LCD: sleep assist brightness=%.3f backlight=%d\r\n", brightness, backlight);
    }

    if (!bsp_display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *scr = lv_scr_act();
    if (!s_assist_visible) {
        clean_screen(scr);
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(scr, screen_touch_event_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_set_style_bg_color(scr, ui_color(s_scene_bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
        s_assist_ring = lv_obj_create(scr);
        lv_obj_clear_flag(s_assist_ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_assist_ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_assist_ring, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_assist_ring, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_assist_ring, ui_color(s_scene_ring), LV_PART_MAIN);
        lv_obj_set_style_border_opa(s_assist_ring, LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_radius(s_assist_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        s_assist_orb = lv_obj_create(scr);
        lv_obj_clear_flag(s_assist_orb, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_assist_orb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(s_assist_orb, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_assist_orb, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_assist_orb, ui_color(s_scene_orb), LV_PART_MAIN);
        s_assist_text = lv_label_create(scr);
        lv_obj_clear_flag(s_assist_text, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_font(s_assist_text, font_ui(), LV_PART_MAIN);
        lv_obj_set_style_text_color(s_assist_text, ui_color(s_scene_text), LV_PART_MAIN);
        lv_label_set_text(s_assist_text, "SLEEP ASSIST");
        lv_obj_align(s_assist_text, LV_ALIGN_TOP_MID, 0, 18);
        make_touch_button(scr, "退出", 0, 174, 158,
                          0x5A1010, 0xFFFFFF, MOBILE_APP_COMMAND_STOP);
        make_touch_button(scr, "深睡眠", 162, 174, 158,
                          0x004A78, 0xFFFFFF, MOBILE_APP_COMMAND_SLEPT);
        s_clock_mode_visible = false;
        s_sleep_locked_clock_visible = false;
        s_assist_visible = true;
        s_dashboard_visible = false;
        s_solid_color_visible = false;
    }

    float curve = 0.5f * (1.0f - cosf(2.0f * 3.14159265358979323846f * phase));
    lv_coord_t size = (lv_coord_t)(64.0f + 42.0f * curve + brightness * 105.0f);
    if (size < 32) {
        size = 32;
    }
    if (size > 120) {
        size = 120;
    }
    lv_obj_set_size(s_assist_orb, size, size);
    lv_obj_align(s_assist_orb, LV_ALIGN_CENTER, 0, 10);
    if (s_assist_ring) {
        lv_coord_t ring_size = size + 24;
        lv_obj_set_size(s_assist_ring, ring_size, ring_size);
        lv_obj_align(s_assist_ring, LV_ALIGN_CENTER, 0, 10);
        lv_obj_set_style_border_opa(s_assist_ring, (lv_opa_t)(42 + (int)(curve * 62.0f)), LV_PART_MAIN);
    }
    int orb_opa = 70 + (int)(brightness * 520.0f);
    if (orb_opa > 190) {
        orb_opa = 190;
    }
    lv_obj_set_style_bg_opa(s_assist_orb, (lv_opa_t)orb_opa, LV_PART_MAIN);

    if (s_assist_text) {
        if (text_visible) {
            lv_obj_clear_flag(s_assist_text, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_assist_text, LV_OBJ_FLAG_HIDDEN);
        }
    }

    bsp_display_unlock();
    return ESP_OK;
#else
    (void)phase;
    (void)brightness;
    (void)text_visible;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t sleep_ui_show_solid_color(uint32_t rgb, uint8_t brightness_percent, const char *label)
{
#if CONFIG_DG_ENABLE_BOX_DISPLAY
    if (!s_display_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    (void)bsp_display_brightness_set(brightness_percent);

    if (!bsp_display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    const char *label_text = label ? label : "";
    if (!s_solid_color_visible || s_solid_color_rgb != rgb ||
        strcmp(s_solid_label, label_text) != 0) {
        lv_obj_t *scr = lv_scr_act();
        clean_screen(scr);
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(scr, screen_touch_event_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_set_style_bg_color(scr, ui_color(rgb), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

        if (label_text[0]) {
            lv_obj_t *text = lv_label_create(scr);
            lv_obj_set_style_text_font(text, font_value(), LV_PART_MAIN);
            lv_obj_set_style_text_color(text, ui_color(0xFFFFFF), LV_PART_MAIN);
            lv_label_set_text(text, label_text);
            lv_obj_align(text, LV_ALIGN_CENTER, 0, 0);
        }

        s_solid_color_rgb = rgb;
        snprintf(s_solid_label, sizeof(s_solid_label), "%s", label_text);
        s_dashboard_visible = false;
        s_assist_visible = false;
        s_clock_mode_visible = false;
        s_sleep_locked_clock_visible = false;
        s_solid_color_visible = true;
    }

    bsp_display_unlock();
    return ESP_OK;
#else
    (void)rgb;
    (void)brightness_percent;
    (void)label;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t sleep_ui_show_clock_mode(void)
{
#if CONFIG_DG_ENABLE_BOX_DISPLAY
    if (!s_display_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(50))) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_clock_mode_visible || s_sleep_locked_clock_visible) {
        lv_obj_t *scr = lv_scr_act();
        clean_screen(scr);
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(scr, screen_touch_event_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_set_style_bg_color(scr, ui_color(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

        lv_obj_t *brand_zh = lv_label_create(scr);
        lv_obj_set_pos(brand_zh, 8, 8);
        lv_obj_set_size(brand_zh, 304, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(brand_zh, font_value(), LV_PART_MAIN);
        lv_obj_set_style_text_color(brand_zh, ui_color(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_align(brand_zh, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(brand_zh, 0, LV_PART_MAIN);
        lv_label_set_long_mode(brand_zh, LV_LABEL_LONG_CLIP);
        lv_label_set_text(brand_zh, "DreamGuardian");

        lv_obj_t *brand_en = lv_label_create(scr);
        lv_obj_set_pos(brand_en, 8, 42);
        lv_obj_set_size(brand_en, 304, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(brand_en, font_button(), LV_PART_MAIN);
        lv_obj_set_style_text_color(brand_en, ui_color(0xFFFF40), LV_PART_MAIN);
        lv_obj_set_style_text_align(brand_en, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_opa(brand_en, LV_OPA_COVER, LV_PART_MAIN);
        lv_label_set_text(brand_en, "GO");

        s_clock_big = lv_label_create(scr);
        lv_obj_set_pos(s_clock_big, 8, 70);
        lv_obj_set_size(s_clock_big, 304, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(s_clock_big, font_value(), LV_PART_MAIN);
        lv_obj_set_style_text_color(s_clock_big, ui_color(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_clock_big, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(s_clock_big, 0, LV_PART_MAIN);
        lv_label_set_long_mode(s_clock_big, LV_LABEL_LONG_CLIP);

        s_clock_date = lv_label_create(scr);
        lv_obj_set_pos(s_clock_date, 0, 110);
        lv_obj_set_size(s_clock_date, 320, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(s_clock_date, font_clock_date(), LV_PART_MAIN);
        lv_obj_set_style_text_color(s_clock_date, ui_color(0x40FFFF), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_clock_date, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(s_clock_date, 0, LV_PART_MAIN);
        lv_label_set_long_mode(s_clock_date, LV_LABEL_LONG_CLIP);

        make_touch_button(scr, "SLEEP", 0, 174, 106, 0x005A28, 0xFFFFFF, MOBILE_APP_COMMAND_SLEEP);
        make_touch_button(scr, "STOP", 107, 174, 106, 0x8A0018, 0xFFFFFF, MOBILE_APP_COMMAND_STOP);
        make_touch_button(scr, "TEST", 214, 174, 106, 0x003A98, 0xFFFFFF, MOBILE_APP_COMMAND_SELF_TEST);

        s_clock_status = lv_label_create(scr);
        lv_obj_set_pos(s_clock_status, 8, 140);
        lv_obj_set_size(s_clock_status, 304, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(s_clock_status, font_button(), LV_PART_MAIN);
        lv_obj_set_style_text_color(s_clock_status, ui_color(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_clock_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(s_clock_status, 0, LV_PART_MAIN);
        lv_label_set_long_mode(s_clock_status, LV_LABEL_LONG_CLIP);
        lv_label_set_text(s_clock_status, "TOUCH READY");

        s_dashboard_visible = false;
        s_assist_visible = false;
        s_clock_mode_visible = true;
        s_sleep_locked_clock_visible = false;
        s_solid_color_visible = false;
    }

    struct tm timeinfo = {0};
    if (time_sync_get_local_time(&timeinfo, NULL)) {
        lv_label_set_text_fmt(s_clock_big, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        if (s_clock_date) {
            lv_label_set_text_fmt(s_clock_date, "%04d-%02d-%02d  %s",
                                  timeinfo.tm_year + 1900,
                                  timeinfo.tm_mon + 1,
                                  timeinfo.tm_mday,
                                  time_sync_weekday_name(timeinfo.tm_wday));
        }
    } else {
        voice_control_status_t voice = {0};
        voice_control_get_status(&voice);
        if (voice.clock_valid) {
            lv_label_set_text_fmt(s_clock_big, "%02u:%02u:--", voice.current_hour, voice.current_minute);
            if (s_clock_date) {
                lv_label_set_text(s_clock_date, "WAIT NET TIME");
            }
        } else {
            lv_label_set_text(s_clock_big, "--:--:--");
            if (s_clock_date) {
                lv_label_set_text(s_clock_date, "WAIT NET TIME");
            }
        }
    }
    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(70), TAG, "clock brightness failed");
    lvgl_port_unlock();
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t sleep_ui_show_sleep_locked_clock(void)
{
#if CONFIG_DG_ENABLE_BOX_DISPLAY
    if (!s_display_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(50))) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_sleep_locked_clock_visible) {
        lv_obj_t *scr = lv_scr_act();
        clean_screen(scr);
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(scr, screen_touch_event_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_set_style_bg_color(scr, ui_color(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

        lv_obj_t *state = lv_label_create(scr);
        lv_obj_set_pos(state, 8, 20);
        lv_obj_set_size(state, 304, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(state, font_button(), LV_PART_MAIN);
        lv_obj_set_style_text_color(state, ui_color(0xA06024), LV_PART_MAIN);
        lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(state, 0, LV_PART_MAIN);
        lv_label_set_text(state, "SLEEP LOCK");

        s_clock_big = lv_label_create(scr);
        lv_obj_set_pos(s_clock_big, 8, 62);
        lv_obj_set_size(s_clock_big, 304, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(s_clock_big, font_value(), LV_PART_MAIN);
        lv_obj_set_style_text_color(s_clock_big, ui_color(0xC08038), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_clock_big, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(s_clock_big, 0, LV_PART_MAIN);
        lv_label_set_long_mode(s_clock_big, LV_LABEL_LONG_CLIP);

        s_clock_date = lv_label_create(scr);
        lv_obj_set_pos(s_clock_date, 8, 106);
        lv_obj_set_size(s_clock_date, 304, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(s_clock_date, font_ui(), LV_PART_MAIN);
        lv_obj_set_style_text_color(s_clock_date, ui_color(0x705030), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_clock_date, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(s_clock_date, 0, LV_PART_MAIN);
        lv_label_set_long_mode(s_clock_date, LV_LABEL_LONG_CLIP);

        s_clock_status = lv_label_create(scr);
        lv_obj_set_pos(s_clock_status, 8, 146);
        lv_obj_set_size(s_clock_status, 304, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(s_clock_status, font_ui(), LV_PART_MAIN);
        lv_obj_set_style_text_color(s_clock_status, ui_color(0x806040), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_clock_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(s_clock_status, 0, LV_PART_MAIN);
        lv_label_set_long_mode(s_clock_status, LV_LABEL_LONG_CLIP);
        lv_label_set_text(s_clock_status, "MONITORING");

        s_dashboard_visible = false;
        s_assist_visible = false;
        s_clock_mode_visible = true;
        s_sleep_locked_clock_visible = true;
        s_solid_color_visible = false;
    }

    struct tm timeinfo = {0};
    if (time_sync_get_local_time(&timeinfo, NULL)) {
        lv_label_set_text_fmt(s_clock_big, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        if (s_clock_date) {
            lv_label_set_text_fmt(s_clock_date, "%04d-%02d-%02d  ASLEEP",
                                  timeinfo.tm_year + 1900,
                                  timeinfo.tm_mon + 1,
                                  timeinfo.tm_mday);
        }
    } else {
        voice_control_status_t voice = {0};
        voice_control_get_status(&voice);
        if (voice.clock_valid) {
            lv_label_set_text_fmt(s_clock_big, "%02u:%02u", voice.current_hour, voice.current_minute);
            if (s_clock_date) {
                lv_label_set_text(s_clock_date, "ASLEEP");
            }
        } else {
            lv_label_set_text(s_clock_big, "--:--");
            if (s_clock_date) {
                lv_label_set_text(s_clock_date, "ASLEEP");
            }
        }
    }
    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(8), TAG, "sleep locked brightness failed");
    lvgl_port_unlock();
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static const char *sensor_age_state(int64_t now_us, int64_t update_us)
{
    if (update_us <= 0) {
        return "WAIT";
    }
    int64_t age_us = now_us - update_us;
    if (age_us < 0) {
        age_us = 0;
    }
    return age_us <= 5000000LL ? "OK" : "OLD";
}

static unsigned sensor_bpm_to_uint(float bpm)
{
    if (!isfinite(bpm) || bpm < 0.0f) {
        return 0;
    }
    if (bpm > 999.0f) {
        return 999;
    }
    return (unsigned)(bpm + 0.5f);
}

static unsigned sensor_ratio_to_percent(float value)
{
    if (!isfinite(value) || value < 0.0f) {
        return 0;
    }
    if (value > 1.0f) {
        value = 1.0f;
    }
    return (unsigned)(value * 99.0f + 0.5f);
}

static unsigned long sensor_packet_count_short(uint32_t count)
{
    return count > 99999U ? 99999UL : (unsigned long)count;
}

void sleep_ui_update_sensor_status(const ld6002_snapshot_t *radar,
                                   const csi_monitor_status_t *csi)
{
#if CONFIG_DG_ENABLE_BOX_DISPLAY
    if (!s_display_ready || !s_clock_mode_visible || !s_clock_status) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    uint32_t now_ms = (uint32_t)(now_us / 1000LL);
    if (s_touch_last_command_ms > 0 &&
        now_ms - s_touch_last_command_ms < SENSOR_STATUS_TOUCH_HOLD_MS) {
        return;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(20))) {
        return;
    }

    bool show_radar = ((now_ms / 2000U) % 3U) != 2U;
    if (show_radar && radar) {
        if (radar->frames > 0) {
            const char *state = sensor_age_state(now_us, radar->last_update_us);
            lv_label_set_text_fmt(s_clock_status, "RAD %s P%u B%u H%u",
                                  state,
                                  radar->human_present ? 1U : 0U,
                                  sensor_bpm_to_uint(radar->breath_rate_bpm),
                                  sensor_bpm_to_uint(radar->heart_rate_bpm));
        } else if (radar->raw_frames > 0) {
            lv_label_set_text_fmt(s_clock_status, "RAD RAW %lu T%04X",
                                  sensor_packet_count_short(radar->raw_frames),
                                  radar->last_type);
        } else if (radar->uart_bytes > 0) {
            const char *state = sensor_age_state(now_us, radar->last_byte_update_us);
            lv_label_set_text_fmt(s_clock_status, "RAD RX %s L%02X",
                                  state,
                                  radar->last_byte);
        } else {
            lv_label_set_text(s_clock_status, "RAD WAIT");
        }
    } else if (csi) {
        const char *state = !csi->enabled ? "OFF" : (csi->packet_count > 0 ? "ON" : "WAIT");
        lv_label_set_text_fmt(s_clock_status, "CSI %s P%lu M%02u",
                              state,
                              sensor_packet_count_short(csi->packet_count),
                              sensor_ratio_to_percent(csi->motion_index));
    }
    lvgl_port_unlock();
#else
    (void)radar;
    (void)csi;
#endif
}

void sleep_ui_update(const ld6002_snapshot_t *radar,
                     const sleep_features_t *features,
                     const sleep_assessment_t *assessment,
                     const intervention_decision_t *decision)
{
    voice_control_status_t voice = {0};
    voice_control_get_status(&voice);

    ESP_LOGI(TAG,
             "human=%d breath=%.1f heart=%.1f range=%.1f bytes=%u last_byte=0x%02x raw=%u valid=%u unk=%u err=%u/%u last=0x%04x len=%u voice=%s alarm=%u %02u:%02u score=%u risk=%u ssi=%u state=%s skill=%s",
             radar->human_present,
             radar->breath_rate_bpm,
             radar->heart_rate_bpm,
             radar->range_cm,
             radar->uart_bytes,
             radar->last_byte,
             radar->raw_frames,
             radar->frames,
             radar->unknown_frames,
             radar->checksum_errors,
             radar->parse_errors,
             radar->last_type,
             radar->last_len,
             voice_mode_to_name(voice.mode),
             voice.alarm_enabled,
             voice.alarm_hour,
             voice.alarm_minute,
             assessment->sleep_score,
             assessment->wake_risk,
             assessment->stable_sleep_index,
             sleep_state_to_name(assessment->state),
             decision->skill_name);

#if CONFIG_DG_ENABLE_BOX_DISPLAY
    if (!s_display_ready) {
        return;
    }

    if (bsp_display_lock(0)) {
        if (!s_dashboard_visible) {
            create_dashboard();
            s_solid_color_visible = false;
        }
        if (voice.clock_valid) {
            lv_label_set_text_fmt(s_clock, "%02u:%02u", voice.current_hour, voice.current_minute);
        } else {
            lv_label_set_text(s_clock, "--:--");
        }
        lv_label_set_text_fmt(s_breath, "B %03.0f", radar->breath_rate_bpm);
        lv_label_set_text_fmt(s_heart, "H %03.0f", radar->heart_rate_bpm);
        lv_label_set_text_fmt(s_range, "R %03.0f", radar->range_cm);
        lv_label_set_text_fmt(s_state, "STATE %-12s", sleep_state_to_name(assessment->state));
        lv_label_set_text_fmt(s_score, "SCORE %03u  RISK %03u  SSI %03u",
                              assessment->sleep_score,
                              assessment->wake_risk,
                              assessment->stable_sleep_index);
        lv_label_set_text_fmt(s_radar, "RX%05lu RAW%05lu OK%05lu",
                              sensor_packet_count_short(radar->uart_bytes),
                              sensor_packet_count_short(radar->raw_frames),
                              sensor_packet_count_short(radar->frames));
        lv_label_set_text_fmt(s_decision, "SKILL %-21s",
                              decision->skill_name[0] ? decision->skill_name : "none");
        lv_label_set_text_fmt(s_voice, "MODE %-10s MOT %04.2f",
                              voice_mode_to_name(voice.mode),
                              features ? features->motion_energy : 0.0f);
        bsp_display_unlock();
    }
#else
    (void)features;
#endif
}
