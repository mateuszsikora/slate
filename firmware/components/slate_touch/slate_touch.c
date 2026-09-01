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

#define SLATE_TOUCH_EXIO_RESET SLATE_CH422G_PIN(1)
#define SLATE_TOUCH_INT_GPIO   GPIO_NUM_4

/* Selected by the reset sequence below rather than by this constant — the
 * controller latches the address from a pin level, so changing it here alone
 * would produce a driver talking confidently to nothing. Both halves are in
 * reset_controller(), which is where the two have to agree. */
#define SLATE_TOUCH_ADDRESS ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS

/* LVGL's own refresh period. A poll is one 9-byte I²C transaction at 400 kHz,
 * a few hundred microseconds against the 26.4 ms the panel takes to scan (S-2),
 * so the input device should never be the thing a frame is waiting for.
 *
 * It is not quite what it achieves. #81 removed the ceiling that made this
 * number decorative — the flush gate used to wait for VSYNC on the one task
 * §6.1 allows LVGL, which pinned every lv_timer to one run per frame and a drag
 * to 38 Hz — and a drag now measures around 62 Hz. The remainder is the
 * renderer, which shares this task and takes it for the length of a frame's
 * drawing, so a poll due during one waits for it.
 */
#define SLATE_TOUCH_READ_PERIOD_MS 10

/* Keep the measured bus speed from #7, now expressed per device. Unlike the
 * legacy panel IO, the v2 path also honours the finite transfer timeout. */
#define SLATE_TOUCH_I2C_HZ         400000
#define SLATE_TOUCH_I2C_TIMEOUT_MS 10

/* A controller that has stopped answering does so on every poll, fifty times a
 * second. The first line is the diagnosis and the rest are noise, so the rest
 * are a count. */
#define SLATE_TOUCH_FAULT_LOG_PERIOD_US (10LL * 1000000)

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_touch;
static lv_indev_t *s_indev;
static bool s_ready;
static bool s_pressed;
static bool s_press_consumed;
static bool s_block_until_lift;
static int32_t s_h_res;
static int32_t s_v_res;
static uint16_t s_last_x;
static uint16_t s_last_y;
static uint32_t s_press_samples;
static int64_t s_press_started_us;
static bool s_hold_fired;
static uint32_t s_faults;
static int64_t s_fault_logged_at_us;
static portMUX_TYPE s_observer_lock = portMUX_INITIALIZER_UNLOCKED;
static slate_touch_press_observer_t s_press_observer;
static void *s_press_observer_ctx;
static slate_touch_hold_observer_t s_hold_observer;
static void *s_hold_observer_ctx;
static uint32_t s_hold_duration_ms;

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
 *
 * The board driver serializes every cached output update with its write. A
 * backlight change between reset low and high therefore preserves the reset
 * bit, and the release preserves the changed backlight bit in turn.
 */
static esp_err_t reset_controller(slate_ch422g_handle_t expander)
{
    const gpio_config_t int_as_output = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = BIT64(SLATE_TOUCH_INT_GPIO),
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_as_output), TAG, "interrupt line as output");
    ESP_RETURN_ON_ERROR(gpio_set_level(SLATE_TOUCH_INT_GPIO, 0), TAG, "interrupt line low");

    esp_err_t err = slate_ch422g_set_level(expander, SLATE_TOUCH_EXIO_RESET, false);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(11));
        err = slate_ch422g_set_level(expander, SLATE_TOUCH_EXIO_RESET, true);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "reset line");

    vTaskDelay(pdMS_TO_TICKS(6));

    /* The controller runs its own firmware start-up before it will answer. */
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static esp_err_t attach_controller(i2c_master_bus_handle_t i2c_bus)
{
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_config.dev_addr = SLATE_TOUCH_ADDRESS;
    io_config.scl_speed_hz = SLATE_TOUCH_I2C_HZ;
    io_config.transaction_timeout_ms = SLATE_TOUCH_I2C_TIMEOUT_MS;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c_v2(i2c_bus, &io_config, &s_io),
                        TAG, "panel IO");

    /* `driver_data` is deliberately absent. The driver reads it only inside the
     * address-selection branch that an expander-borne reset line makes
     * unreachable, so supplying it would advertise a knob that changes a log
     * line and nothing on the wire. */
    const esp_lcd_touch_config_t config = {
        .x_max = (uint16_t) s_h_res,
        .y_max = (uint16_t) s_v_res,
        .rst_gpio_num = GPIO_NUM_NC, /* EXIO1; reset_controller() has done it */
        .int_gpio_num = SLATE_TOUCH_INT_GPIO,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(s_io, &config, &s_touch), TAG, "GT911");
    return ESP_OK;
}

/*
 * Everything reset_controller() and attach_controller() may have taken, in the
 * reverse order, and callable at any point in either. The interrupt pin is the
 * one that matters: reset_controller() leaves it an output driving low, and the
 * driver only turns it back into an input on the path where it succeeds. A
 * failure between those two points would otherwise leave the SoC holding a line
 * the controller — released from reset a few milliseconds earlier — drives for
 * itself on every touch.
 */
static void release_controller(void)
{
    if (s_touch) {
        esp_lcd_touch_del(s_touch); /* also restores the interrupt pin */
        s_touch = NULL;
    } else {
        gpio_reset_pin(SLATE_TOUCH_INT_GPIO);
    }
    if (s_io) {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
    }
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

/*
 * Reporting a release is the only safe answer to a failed read — a coordinate
 * that could not be fetched is not a coordinate — but it has to be a release
 * this component agrees with. Leaving `s_pressed` set would make LVGL and
 * slate_touch disagree about the finger: LVGL would deliver a release and then
 * a fresh press on the next good poll, while the sample counter went on
 * accumulating into the previous press and the eventual real release logged
 * nothing at all. A consumed wake press is the exception: a transient failed
 * read may release it from LVGL's point of view, but only a real zero-point
 * sample clears the block. Otherwise the same finger could become a fresh,
 * visible press after the backlight came on and operate the tile underneath.
 */
static void report_released(lv_indev_data_t *data, bool physical_release)
{
    if (s_pressed) {
        const int64_t held_us = esp_timer_get_time() - s_press_started_us;
        /* The sample rate is reported rather than assumed, because it is the
         * number that separates a slow input device from a slow display: this
         * is how often a coordinate was available, and anything the glass does
         * later is downstream of it. */
        ESP_LOGI(TAG, "release at %u,%u after %lld ms, %" PRIu32 " samples (%.0f Hz)",
                 (unsigned) s_last_x, (unsigned) s_last_y, (long long) (held_us / 1000),
                 s_press_samples,
                 held_us > 0 ? (double) s_press_samples * 1000000.0 / (double) held_us : 0.0);
        s_pressed = false;
        s_press_consumed = false;
        s_hold_fired = false;
    }
    if (physical_release) {
        s_block_until_lift = false;
    }
    data->state = LV_INDEV_STATE_RELEASED;
}

static bool observe_press(void)
{
    slate_touch_press_observer_t observer;
    void *ctx;
    portENTER_CRITICAL(&s_observer_lock);
    observer = s_press_observer;
    ctx = s_press_observer_ctx;
    portEXIT_CRITICAL(&s_observer_lock);
    return observer != NULL && observer(ctx);
}

static void observe_hold(int64_t held_us)
{
    slate_touch_hold_observer_t observer;
    void *ctx;
    uint32_t duration_ms;

    portENTER_CRITICAL(&s_observer_lock);
    observer = s_hold_observer;
    ctx = s_hold_observer_ctx;
    duration_ms = s_hold_duration_ms;
    portEXIT_CRITICAL(&s_observer_lock);

    if (s_hold_fired || observer == NULL || duration_ms == 0 ||
        held_us < (int64_t) duration_ms * 1000) {
        return;
    }

    /* Set the input state before handing control away. The observer may wake a
     * higher-priority reset task immediately; no later path may reinterpret
     * this same finger as a release/click on the dashboard underneath it. */
    s_hold_fired = true;
    s_press_consumed = true;
    s_block_until_lift = true;
    observer(ctx);
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void) indev;

    esp_err_t err = esp_lcd_touch_read_data(s_touch);
    if (err != ESP_OK) {
        note_fault("controller read", err);
        report_released(data, false);
        return;
    }

    esp_lcd_touch_point_data_t point = {0};
    uint8_t points = 0;
    err = esp_lcd_touch_get_data(s_touch, &point, &points, 1);
    if (err != ESP_OK) {
        note_fault("coordinate read", err);
        report_released(data, false);
        return;
    }

    if (points == 0) {
        report_released(data, true);
        return;
    }

    uint16_t x = point.x;
    uint16_t y = point.y;

    /* Clamped rather than trusted, and reported rather than clamped quietly: a
     * coordinate outside the panel means the controller came up against a
     * configuration for a different resolution, which is a thing to fix and not
     * a thing to absorb. LVGL is handed something inside the display either
     * way, because a point outside it lands on no object at all. */
    if (x >= s_h_res || y >= s_v_res) {
        note_fault("coordinate outside the panel", ESP_ERR_INVALID_RESPONSE);
        x = x >= s_h_res ? (uint16_t) (s_h_res - 1) : x;
        y = y >= s_v_res ? (uint16_t) (s_v_res - 1) : y;
    }

    s_last_x = x;
    s_last_y = y;

    if (!s_pressed) {
        ESP_LOGI(TAG, "press at %d,%d (%u point%s, strength %u)", (int) x, (int) y,
                 (unsigned) points, points == 1 ? "" : "s", (unsigned) point.strength);
        s_pressed = true;
        s_press_consumed = s_block_until_lift || observe_press();
        s_block_until_lift = s_press_consumed;
        s_press_started_us = esp_timer_get_time();
        s_press_samples = 0;
        s_hold_fired = false;
    }
    s_press_samples++;

    observe_hold(esp_timer_get_time() - s_press_started_us);

    data->point.x = x;
    data->point.y = y;
    data->state = s_press_consumed ? LV_INDEV_STATE_RELEASED : LV_INDEV_STATE_PRESSED;
}

esp_err_t slate_touch_init(i2c_master_bus_handle_t i2c_bus, slate_ch422g_handle_t expander)
{
    if (!i2c_bus || !expander) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* One source of truth for the panel size, and it is the display that has
     * already been told what the panel is. A private copy here would be a third
     * place to change for a board variant and the first to be forgotten. */
    lv_display_t *display = lv_display_get_default();
    if (!display) {
        ESP_LOGE(TAG, "no LVGL display; touch has nothing to report coordinates against");
        return ESP_ERR_INVALID_STATE;
    }
    s_h_res = lv_display_get_horizontal_resolution(display);
    s_v_res = lv_display_get_vertical_resolution(display);
    if (s_h_res <= 0 || s_v_res <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = reset_controller(expander);
    if (err == ESP_OK) {
        err = attach_controller(i2c_bus);
    }
    if (err != ESP_OK) {
        release_controller();
        return err;
    }

    s_indev = lv_indev_create();
    if (!s_indev) {
        release_controller();
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, indev_read_cb);
    lv_timer_set_period(lv_indev_get_read_timer(s_indev), SLATE_TOUCH_READ_PERIOD_MS);

    s_ready = true;
    ESP_LOGI(TAG,
             "GT911 at 0x%02X on shared I2C bus, interrupt GPIO%d, reset EXIO1; "
             "%" PRId32 "x%" PRId32 ", polled every %d ms",
             (unsigned) SLATE_TOUCH_ADDRESS, (int) SLATE_TOUCH_INT_GPIO, s_h_res,
             s_v_res, SLATE_TOUCH_READ_PERIOD_MS);
    return ESP_OK;
}

bool slate_touch_ready(void)
{
    return s_ready;
}

esp_err_t slate_touch_set_press_observer(slate_touch_press_observer_t observer,
                                         void *ctx)
{
    if (observer == NULL && ctx != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_observer_lock);
    s_press_observer = observer;
    s_press_observer_ctx = ctx;
    portEXIT_CRITICAL(&s_observer_lock);
    return ESP_OK;
}

esp_err_t slate_touch_set_hold_observer(uint32_t duration_ms,
                                        slate_touch_hold_observer_t observer,
                                        void *ctx)
{
    bool clearing = observer == NULL;
    if ((clearing && (duration_ms != 0 || ctx != NULL)) ||
        (!clearing && duration_ms == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_observer_lock);
    s_hold_observer = observer;
    s_hold_observer_ctx = ctx;
    s_hold_duration_ms = duration_ms;
    portEXIT_CRITICAL(&s_observer_lock);
    return ESP_OK;
}
