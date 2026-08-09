/*
 * Slate — GT911 capacitive touch bring-up.
 *
 * The pin the Waveshare wiki and the ESPHome package S-2 cross-checked the RGB
 * map against agree on, and the one S-2 itself never needed: the touch
 * interrupt is GPIO4, and the reset is CH422G EXIO1. Everything else about this
 * controller is the address, and the address is the only part of GT911
 * bring-up that is not obvious.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "slate_touch.h"

static const char *TAG = "slate_touch";

#define SLATE_TOUCH_H_RES 800
#define SLATE_TOUCH_V_RES 480

#define SLATE_TOUCH_EXIO_RESET IO_EXPANDER_PIN_NUM_1
#define SLATE_TOUCH_INT_GPIO   GPIO_NUM_4

/* LVGL's own refresh period, so a coordinate is never the thing a frame is
 * waiting for. A poll is one 9-byte I²C transaction at 400 kHz — a few hundred
 * microseconds against the 26.4 ms the panel takes to scan (S-2) — and
 * sampling at half the refresh rate was visible as a marker trailing a finger
 * before the display's own latency was accounted for at all. */
#define SLATE_TOUCH_READ_PERIOD_MS 10

/* A controller that has stopped answering does so on every poll, fifty times a
 * second. The first line is the diagnosis and the rest are noise, so the rest
 * are a count. */
#define SLATE_TOUCH_FAULT_LOG_PERIOD_US (10LL * 1000000)

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_touch;
static lv_indev_t *s_indev;
static bool s_ready;
static bool s_pressed;
static uint16_t s_last_x;
static uint16_t s_last_y;
static uint32_t s_press_samples;
static int64_t s_press_started_us;
static uint32_t s_faults;
static int64_t s_fault_logged_at_us;

/*
 * The driver keeps the pointer rather than the value, so this outlives the call
 * even though nothing reads it after initialisation: with reset on the expander
 * the driver's own address-selection branch is unreachable (see below) and the
 * field is only ever inspected there.
 */
static esp_lcd_touch_io_gt911_config_t s_gt911_config = {
    .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
};

/*
 * GT911 latches its I²C address from the state of the interrupt line at the
 * moment reset is released: low selects 0x5D, high selects 0x14. The driver
 * performs that sequence itself, but only when reset and interrupt are both SoC
 * pins — on this board reset is CH422G EXIO1, so that branch cannot be reached
 * and the driver's fallback reset is a no-op with `rst_gpio_num` unset.
 *
 * Hence this. Drive the interrupt line low as an output, hold the controller in
 * reset long enough for it to see that, release, and keep holding while the
 * address is sampled. The line is handed to the driver immediately afterwards,
 * which reconfigures it as an input — leaving it an output would have the SoC
 * and the controller both driving one wire.
 */
static esp_err_t reset_controller(esp_io_expander_handle_t expander)
{
    const gpio_config_t int_as_output = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = BIT64(SLATE_TOUCH_INT_GPIO),
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_as_output), TAG, "interrupt line as output");
    ESP_RETURN_ON_ERROR(gpio_set_level(SLATE_TOUCH_INT_GPIO, 0), TAG, "interrupt line low");

    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(expander, SLATE_TOUCH_EXIO_RESET, IO_EXPANDER_OUTPUT),
        TAG, "reset direction");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, SLATE_TOUCH_EXIO_RESET, 0),
                        TAG, "reset low");
    vTaskDelay(pdMS_TO_TICKS(11));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, SLATE_TOUCH_EXIO_RESET, 1),
                        TAG, "reset high");
    vTaskDelay(pdMS_TO_TICKS(6));

    /* The controller runs its own firmware start-up before it will answer. */
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static esp_err_t attach_controller(int i2c_port)
{
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_config.dev_addr = s_gt911_config.dev_addr;
    /* The macro sets a per-device clock, which only the new I²C driver can
     * honour — the legacy one takes its speed from the bus and refuses a
     * configuration that says otherwise rather than ignoring it. The first
     * attempt at this returned ESP_ERR_INVALID_ARG for exactly that reason. */
    io_config.scl_speed_hz = 0;

    /* The `_v1` suffix names the legacy `driver/i2c.h` bus explicitly. It is
     * not a fallback: the CH422G package accepts nothing else, ESP-IDF aborts
     * at boot if both I²C stacks touch one bus (#7), and the panel and the
     * touch controller are on the same two wires. */
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c_v1((uint32_t) i2c_port, &io_config, &s_io),
                        TAG, "panel IO");

    const esp_lcd_touch_config_t config = {
        .x_max = SLATE_TOUCH_H_RES,
        .y_max = SLATE_TOUCH_V_RES,
        .rst_gpio_num = GPIO_NUM_NC, /* EXIO1; reset_controller() has done it */
        .int_gpio_num = SLATE_TOUCH_INT_GPIO,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
        .driver_data = &s_gt911_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(s_io, &config, &s_touch), TAG, "GT911");
    return ESP_OK;
}

static void note_fault(const char *what, esp_err_t err)
{
    s_faults++;

    const int64_t now = esp_timer_get_time();
    if (s_fault_logged_at_us != 0 && now - s_fault_logged_at_us < SLATE_TOUCH_FAULT_LOG_PERIOD_US) {
        return;
    }
    s_fault_logged_at_us = now;
    ESP_LOGW(TAG, "%s: %s (%" PRIu32 " so far)", what, esp_err_to_name(err), s_faults);
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void) indev;

    esp_err_t err = esp_lcd_touch_read_data(s_touch);
    if (err != ESP_OK) {
        note_fault("controller read", err);
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_lcd_touch_point_data_t point = {0};
    uint8_t points = 0;
    err = esp_lcd_touch_get_data(s_touch, &point, &points, 1);
    if (err != ESP_OK) {
        note_fault("coordinate read", err);
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    uint16_t x = point.x;
    uint16_t y = point.y;

    if (points == 0) {
        if (s_pressed) {
            /* The sample rate is reported rather than assumed, because it is
             * the number that separates a slow input device from a slow
             * display: this is how often a coordinate was available, and
             * anything the glass does later is downstream of it. */
            const int64_t held_us = esp_timer_get_time() - s_press_started_us;
            ESP_LOGI(TAG, "release at %u,%u after %lld ms, %" PRIu32 " samples (%.0f Hz)",
                     (unsigned) s_last_x, (unsigned) s_last_y, (long long) (held_us / 1000),
                     s_press_samples,
                     held_us > 0 ? (double) s_press_samples * 1000000.0 / (double) held_us : 0.0);
            s_pressed = false;
        }
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    /* Clamped rather than trusted, and reported rather than clamped quietly: a
     * coordinate outside the panel means the controller came up against a
     * configuration for a different resolution, which is a thing to fix and not
     * a thing to absorb. LVGL is handed something inside the display either
     * way, because a point outside it lands on no object at all. */
    if (x >= SLATE_TOUCH_H_RES || y >= SLATE_TOUCH_V_RES) {
        note_fault("coordinate outside the panel", ESP_ERR_INVALID_RESPONSE);
        x = x >= SLATE_TOUCH_H_RES ? SLATE_TOUCH_H_RES - 1 : x;
        y = y >= SLATE_TOUCH_V_RES ? SLATE_TOUCH_V_RES - 1 : y;
    }

    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
    s_last_x = x;
    s_last_y = y;

    if (!s_pressed) {
        ESP_LOGI(TAG, "press at %d,%d (%u point%s, strength %u)", (int) x, (int) y,
                 (unsigned) points, points == 1 ? "" : "s", (unsigned) point.strength);
        s_pressed = true;
        s_press_started_us = esp_timer_get_time();
        s_press_samples = 0;
    }
    s_press_samples++;
}

esp_err_t slate_touch_init(int i2c_port, esp_io_expander_handle_t expander)
{
    if (!expander) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(reset_controller(expander), TAG, "reset");
    ESP_RETURN_ON_ERROR(attach_controller(i2c_port), TAG, "attach");

    s_indev = lv_indev_create();
    if (!s_indev) {
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, indev_read_cb);
    lv_timer_set_period(lv_indev_get_read_timer(s_indev), SLATE_TOUCH_READ_PERIOD_MS);

    s_ready = true;
    ESP_LOGI(TAG, "GT911 at 0x%02X on I2C%d, interrupt GPIO%d, reset EXIO1; polled every %d ms",
             (unsigned) s_gt911_config.dev_addr, i2c_port, (int) SLATE_TOUCH_INT_GPIO,
             SLATE_TOUCH_READ_PERIOD_MS);
    return ESP_OK;
}

bool slate_touch_ready(void)
{
    return s_ready;
}
