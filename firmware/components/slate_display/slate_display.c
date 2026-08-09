/*
 * Slate — Waveshare ESP32-S3-Touch-LCD-7 display bring-up.
 *
 * The pin map, timings and render configuration below were not copied forward
 * from an example. S-2 cross-checked them between two independent sources,
 * booted them on this exact N16R8 board and measured 26,441 us between VSYNCs.
 * Its recommended shape is implemented literally: two RGB565 framebuffers in
 * PSRAM, direct LVGL rendering, a ten-line internal bounce buffer and a flush
 * gate that permits exactly one rendered frame per panel scan. The gate is a
 * deferral rather than a wait: §6.1 gives LVGL one task, so blocking it until
 * VSYNC would stop every lv_timer and not merely the renderer.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
/* S-3 found CH422G here rather than in a component of its own. */
#include "port/esp_io_expander_ch422g.h"

#include "slate_display.h"
#include "slate_touch.h"

static const char *TAG = "slate_display";

#define SLATE_LCD_H_RES 800
#define SLATE_LCD_V_RES 480

#define SLATE_LCD_PCLK_HZ           (16 * 1000 * 1000)
#define SLATE_LCD_HSYNC_PULSE_WIDTH 4
#define SLATE_LCD_HSYNC_BACK_PORCH  8
#define SLATE_LCD_HSYNC_FRONT_PORCH 8
#define SLATE_LCD_VSYNC_PULSE_WIDTH 4
#define SLATE_LCD_VSYNC_BACK_PORCH  16
#define SLATE_LCD_VSYNC_FRONT_PORCH 16

#define SLATE_LCD_BOUNCE_LINES 10
#define SLATE_LCD_BOUNCE_PIXELS (SLATE_LCD_H_RES * SLATE_LCD_BOUNCE_LINES)
#define SLATE_LCD_FRAME_BYTES ((size_t) SLATE_LCD_H_RES * SLATE_LCD_V_RES * 2)

#define SLATE_I2C_PORT     I2C_NUM_0
#define SLATE_I2C_SDA_GPIO 8
#define SLATE_I2C_SCL_GPIO 9
#define SLATE_I2C_HZ       400000

/* EXIO1 is the touch controller's reset and belongs to slate_touch, which has
 * to hold it while the controller's I²C address is selected. */
#define SLATE_EXIO_DISP    IO_EXPANDER_PIN_NUM_2
#define SLATE_EXIO_LCD_RST IO_EXPANDER_PIN_NUM_3

#define SLATE_PIN_HSYNC 46
#define SLATE_PIN_VSYNC 3
#define SLATE_PIN_DE    5
#define SLATE_PIN_PCLK  7

#define SLATE_DISPLAY_TASK_STACK 12288
#define SLATE_DISPLAY_TASK_PRIORITY 4
#define SLATE_DISPLAY_TASK_CORE 1
#define SLATE_DISPLAY_QUEUE_LEN 16
#define SLATE_DISPLAY_INIT_TIMEOUT_MS 10000
#define SLATE_DISPLAY_VSYNC_TIMEOUT_MS 100
#define SLATE_DISPLAY_EXPANDER_TIMEOUT_MS 100
#define SLATE_DISPLAY_HEAP_METRICS_PERIOD_US (15LL * 1000000)

static const int SLATE_DATA_GPIOS[16] = {
    14, /* B3 */ 38, /* B4 */ 18, /* B5 */ 17, /* B6 */ 10, /* B7 */
    39, /* G2 */  0, /* G3 */ 45, /* G4 */ 48, /* G5 */ 47, /* G6 */ 21, /* G7 */
     1, /* R3 */  2, /* R4 */ 42, /* R5 */ 41, /* R6 */ 40, /* R7 */
};

typedef struct {
    slate_display_work_fn fn;
    void *ctx;
} slate_display_work_t;

static QueueHandle_t s_work_queue;
static SemaphoreHandle_t s_init_done;
/* The CH422G's output register is read-modify-written by the driver, so two
 * tasks changing two different pins can lose one of them. Only the backlight
 * changes after start-up, but #28's schedule is a second task and this is
 * cheaper than remembering that. */
static SemaphoreHandle_t s_expander_lock;
static TaskHandle_t s_task;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_display;
static esp_io_expander_handle_t s_expander;
static void *s_lvgl_pool;
static esp_err_t s_init_result = ESP_ERR_INVALID_STATE;
static bool s_i2c_installed;
static bool s_lvgl_initialized;
static bool s_ready;
static bool s_backlight_on;
static bool s_first_frame_shown;
static slate_display_heap_metrics_t s_heap_metrics;
static int64_t s_heap_metrics_at_us;
static lv_obj_t *s_setup_overlay;
static atomic_bool s_setup_presentation_active = ATOMIC_VAR_INIT(false);
static atomic_bool s_setup_hide_pending = ATOMIC_VAR_INIT(false);

/* The flush gate. `s_flush_pending` is armed on the LVGL task once a
 * framebuffer has been submitted and disarmed by whichever of the VSYNC ISR and
 * the task's stall check completes the flush first, so exactly one of them
 * calls lv_display_flush_ready(). `s_frame_presented` carries the ISR's "the
 * switch has taken effect" back to the task, which is the only place allowed to
 * take the expander mutex and drive I²C. */
static atomic_bool s_flush_pending = ATOMIC_VAR_INIT(false);
static atomic_bool s_frame_presented = ATOMIC_VAR_INIT(false);
/* Written and read on the LVGL task only, always before `s_flush_pending` is
 * armed, so an observed pending flush is the one this timestamp belongs to. */
static int64_t s_flush_submitted_us;

/* Written in the VSYNC ISR, read on the LVGL task. The values are 64-bit on a
 * 32-bit CPU, so the read is protected rather than assumed atomic. */
static portMUX_TYPE s_vsync_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_heap_metrics_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t s_vsync_last_us;
static volatile int64_t s_vsync_period_us;
static volatile uint32_t s_vsync_count;

static void update_heap_metrics(void)
{
    int64_t now = esp_timer_get_time();
    if (s_heap_metrics.available &&
        now - s_heap_metrics_at_us < SLATE_DISPLAY_HEAP_METRICS_PERIOD_US) {
        return;
    }

    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);

    portENTER_CRITICAL(&s_heap_metrics_lock);
    s_heap_metrics.available = true;
    s_heap_metrics.free_size = monitor.free_size;
    s_heap_metrics.total_size = monitor.total_size;
    s_heap_metrics.frag_pct = monitor.frag_pct;
    s_heap_metrics_at_us = now;
    portEXIT_CRITICAL(&s_heap_metrics_lock);
}

void *slate_display_lvgl_pool_alloc(size_t size)
{
    ESP_LOGI(TAG, "LVGL pool %u B at %p in PSRAM", (unsigned) size, s_lvgl_pool);
    return s_lvgl_pool;
}

static esp_err_t i2c_init(void)
{
    /* CH422G 1.1.1 accepts an i2c_port_t. ESP-IDF aborts if the new and legacy
     * drivers touch the same bus, so #7 must keep using this installed legacy
     * bus or replace the expander driver for both panel and touch together. */
    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SLATE_I2C_SDA_GPIO,
        .scl_io_num = SLATE_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = SLATE_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(SLATE_I2C_PORT, &config), TAG, "I2C config");
    esp_err_t err = i2c_driver_install(SLATE_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err == ESP_OK) {
        s_i2c_installed = true;
    }
    return err;
}

static esp_err_t expander_init(void)
{
    ESP_RETURN_ON_ERROR(
        esp_io_expander_new_i2c_ch422g(SLATE_I2C_PORT,
                                       ESP_IO_EXPANDER_I2C_CH422G_ADDRESS,
                                       &s_expander),
        TAG, "CH422G");

    /* From the moment the handle exists, s_expander_lock is what makes the
     * output register single-writer, and that includes this function: the
     * backlight is reachable through the public API the instant `s_expander` is
     * non-NULL, which is now. */
    if (xSemaphoreTake(s_expander_lock, pdMS_TO_TICKS(SLATE_DISPLAY_EXPANDER_TIMEOUT_MS)) !=
        pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint32_t outputs = SLATE_EXIO_DISP | SLATE_EXIO_LCD_RST;
    esp_err_t err = esp_io_expander_set_dir(s_expander, outputs, IO_EXPANDER_OUTPUT);
    if (err == ESP_OK) {
        /* Keep the glass dark until a complete frame exists. */
        err = esp_io_expander_set_level(s_expander, SLATE_EXIO_DISP, 0);
    }
    if (err == ESP_OK) {
        err = esp_io_expander_set_level(s_expander, SLATE_EXIO_LCD_RST, 0);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        err = esp_io_expander_set_level(s_expander, SLATE_EXIO_LCD_RST, 1);
    }
    xSemaphoreGive(s_expander_lock);
    ESP_RETURN_ON_ERROR(err, TAG, "CH422G bring-up");

    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel,
                               const esp_lcd_rgb_panel_event_data_t *event,
                               void *ctx)
{
    (void) panel;
    (void) event;
    (void) ctx;

    const int64_t now = esp_timer_get_time();
    const int64_t delta = now - s_vsync_last_us;

    portENTER_CRITICAL_ISR(&s_vsync_lock);
    if (delta > 1000 && delta < 200000) {
        s_vsync_period_us = s_vsync_period_us == 0
                                ? delta
                                : (s_vsync_period_us * 7 + delta) / 8;
    }
    s_vsync_last_us = now;
    s_vsync_count++;
    portEXIT_CRITICAL_ISR(&s_vsync_lock);

    /* This is the gate. Releasing LVGL here rather than on the task keeps the
     * promise — no next render before the panel has begun scanning this frame —
     * while leaving the task free to run its other timers meanwhile. */
    if (atomic_exchange_explicit(&s_flush_pending, false, memory_order_acq_rel)) {
        atomic_store_explicit(&s_frame_presented, true, memory_order_release);
        lv_display_flush_ready(s_display);
    }
    return false;
}

static esp_err_t panel_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "I2C bring-up");
    ESP_RETURN_ON_ERROR(expander_init(), TAG, "expander bring-up");

    esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = SLATE_LCD_PCLK_HZ,
            .h_res = SLATE_LCD_H_RES,
            .v_res = SLATE_LCD_V_RES,
            .hsync_pulse_width = SLATE_LCD_HSYNC_PULSE_WIDTH,
            .hsync_back_porch = SLATE_LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = SLATE_LCD_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = SLATE_LCD_VSYNC_PULSE_WIDTH,
            .vsync_back_porch = SLATE_LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch = SLATE_LCD_VSYNC_FRONT_PORCH,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        .bounce_buffer_size_px = SLATE_LCD_BOUNCE_PIXELS,
        .dma_burst_size = 64,
        .hsync_gpio_num = SLATE_PIN_HSYNC,
        .vsync_gpio_num = SLATE_PIN_VSYNC,
        .de_gpio_num = SLATE_PIN_DE,
        .pclk_gpio_num = SLATE_PIN_PCLK,
        .disp_gpio_num = -1,
        .flags.fb_in_psram = true,
    };
    memcpy(config.data_gpio_nums, SLATE_DATA_GPIOS, sizeof(SLATE_DATA_GPIOS));

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&config, &s_panel), TAG, "RGB panel");

    const esp_lcd_rgb_panel_event_callbacks_t callbacks = {.on_vsync = on_vsync};
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &callbacks, NULL),
                        TAG, "VSYNC callback");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");

    ESP_LOGI(TAG, "RGB panel up: %dx%d RGB565, 16 MHz, 2 PSRAM framebuffers, bounce %d px",
             SLATE_LCD_H_RES, SLATE_LCD_V_RES, SLATE_LCD_BOUNCE_PIXELS);
    return ESP_OK;
}

/*
 * Latched on the frame rather than on the backlight's own state. Once the
 * backlight can be switched from outside, "is it off" stops answering "has
 * anything been drawn yet" — and a flush arriving while §3.3's screen-off is in
 * force must not turn the panel back on by itself.
 *
 * The latch is set only once the glass is actually lit. The expander shares its
 * bus with the touch controller now, so a single lost arbitration here is a
 * thing that happens; latching before the write would turn one NACK into a
 * panel that renders perfectly and is black for the rest of the boot. Retried
 * every frame, complained about once.
 */
static void backlight_enable_first_frame(void)
{
    static bool failure_logged;

    if (s_first_frame_shown) {
        return;
    }

    esp_err_t err = slate_display_backlight_set(true);
    if (err != ESP_OK) {
        if (!failure_logged) {
            failure_logged = true;
            ESP_LOGE(TAG, "backlight on: %s — retrying on each frame", esp_err_to_name(err));
        }
        return;
    }

    s_first_frame_shown = true;
    ESP_LOGI(TAG, "first frame displayed; backlight on");
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void) area;

    /* Nothing was submitted, so there is no switch to wait for. */
    if (!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
        return;
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                              SLATE_LCD_H_RES, SLATE_LCD_V_RES,
                                              pixels);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "flush: %s", esp_err_to_name(err));
        lv_display_flush_ready(display);
        return;
    }

    /* Armed after draw_bitmap has selected the new framebuffer, for the reason
     * the generation snapshot it replaces was taken there: a VSYNC raised
     * before the panel had this frame must not be the one that releases LVGL.
     * Losing the race to the ISR here costs the frame already being scanned and
     * can never release LVGL early, which is the direction that matters. */
    s_flush_submitted_us = esp_timer_get_time();
    atomic_store_explicit(&s_flush_pending, true, memory_order_release);
}

/*
 * Deferring the completion is only half of the gate. LVGL waits for a flush by
 * spinning on `disp->flushing` (lv_refr.c, wait_for_flushing()), and in double
 * buffered mode it does that at the top of every flush — so the renderer would
 * take the block straight back onto the task, as a busy-wait rather than as a
 * sleep, and the timers this change exists to free would be no better off.
 *
 * The wait is avoided by not refreshing at all while the panel still holds the
 * last frame. Wrapping the refresh timer's callback is how LVGL's own drivers
 * reach into that decision; pausing the timer does not work, because any
 * invalidation sends LV_EVENT_REFR_REQUEST and lv_display.c resumes it again.
 */
static void refresh_timer_cb(lv_timer_t *timer)
{
    if (atomic_load_explicit(&s_flush_pending, memory_order_acquire)) {
        return;
    }

    /* Animations advance on their own timer, which no longer shares a pass with
     * the renderer now that the renderer waits for the panel. A frame would
     * otherwise sample them at a quantised time up to a refresh period stale,
     * and uneven steps read as judder on anything moving. This is what the old
     * lock-step gave for free. */
    lv_anim_refr_now();
    lv_display_refr_timer(timer);
}

/* The gate completes in an interrupt now, so a panel that stopped raising VSYNC
 * would leave the renderer waiting for ever rather than for one frame. This is
 * the old blocking wait's timeout, moved to where the wait went. */
static void service_flush_gate(void)
{
    if (atomic_load_explicit(&s_flush_pending, memory_order_acquire) &&
        esp_timer_get_time() - s_flush_submitted_us >=
            SLATE_DISPLAY_VSYNC_TIMEOUT_MS * 1000LL &&
        atomic_exchange_explicit(&s_flush_pending, false, memory_order_acq_rel)) {
        ESP_LOGE(TAG, "VSYNC timeout after framebuffer switch");
        lv_display_flush_ready(s_display);
    }

    if (atomic_exchange_explicit(&s_frame_presented, false, memory_order_acq_rel)) {
        /* The scan has just begun, so this is the point in the frame with the
         * most room before the next one. Making the renderer due now rather
         * than on its own grid re-locks it to the panel, and consecutive frames
         * are then a scan apart, which is what keeps motion even. It cannot run
         * more often than the panel presents. */
        lv_timer_ready(lv_display_get_refr_timer(s_display));

        /* The ISR cannot do this itself: it takes the expander mutex and talks
         * I²C. Retried on every presented frame, which is what the latch inside
         * it is for. */
        backlight_enable_first_frame();
    }
}

static uint32_t tick_ms(void)
{
    return (uint32_t) (esp_timer_get_time() / 1000);
}

static esp_err_t lvgl_display_init(void)
{
    /* Allocate before lv_init() calls the configured pool hook. A failed 2 MiB
     * reservation can then degrade to headless operation instead of entering
     * TLSF with a null pool and aborting the firmware. */
    s_lvgl_pool = heap_caps_malloc(LV_MEM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lvgl_pool) {
        return ESP_ERR_NO_MEM;
    }

    lv_init();
    s_lvgl_initialized = true;

    lv_display_t *display = lv_display_create(SLATE_LCD_H_RES, SLATE_LCD_V_RES);
    if (!display) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flush_cb);
    /* Published before the first flush can arm the gate, because the VSYNC ISR
     * completes that flush through this handle. */
    s_display = display;
    lv_timer_set_cb(lv_display_get_refr_timer(display), refresh_timer_cb);

    void *framebuffer_0 = NULL;
    void *framebuffer_1 = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &framebuffer_0, &framebuffer_1),
        TAG, "framebuffers");
    if (!framebuffer_0 || !framebuffer_1) {
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_buffers(display, framebuffer_0, framebuffer_1,
                           SLATE_LCD_FRAME_BYTES, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_tick_set_cb(tick_ms);
    return ESP_OK;
}

static void cleanup_error(const char *resource, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cleanup %s: %s", resource, esp_err_to_name(err));
    }
}

static void display_resources_deinit(void)
{
    /* The expander must still have its bus while the glass is made dark. */
    if (s_expander) {
        cleanup_error("backlight", esp_io_expander_set_level(s_expander, SLATE_EXIO_DISP, 0));
        s_backlight_on = false;
        s_first_frame_shown = false;
    }

    /* Before LVGL goes, because the VSYNC ISR completes flushes through
     * s_display. This runs on the display task, which is pinned to the core the
     * panel's interrupt was allocated on, so once the callbacks are gone no
     * invocation of on_vsync is either in flight or still to come. */
    if (s_panel) {
        const esp_lcd_rgb_panel_event_callbacks_t no_callbacks = {0};
        cleanup_error("RGB callbacks",
                      esp_lcd_rgb_panel_register_event_callbacks(s_panel, &no_callbacks, NULL));
    }
    atomic_store_explicit(&s_flush_pending, false, memory_order_release);
    s_display = NULL;

    if (s_lvgl_initialized) {
        lv_deinit();
        s_lvgl_initialized = false;
    }
    if (s_lvgl_pool) {
        heap_caps_free(s_lvgl_pool);
        s_lvgl_pool = NULL;
    }

    /* Deleting the RGB panel stops its DMA and releases the framebuffers LVGL
     * was rendering into, which is why it follows lv_deinit(). */
    if (s_panel) {
        cleanup_error("RGB panel", esp_lcd_panel_del(s_panel));
        s_panel = NULL;
    }
    if (s_expander) {
        cleanup_error("CH422G", esp_io_expander_del(s_expander));
        s_expander = NULL;
    }
    if (s_i2c_installed) {
        cleanup_error("I2C", i2c_driver_delete(SLATE_I2C_PORT));
        s_i2c_installed = false;
    }
}

static lv_obj_t *solid_rect(lv_obj_t *parent, int32_t x, int32_t y,
                            int32_t width, int32_t height, uint32_t color)
{
    lv_obj_t *rect = lv_obj_create(parent);
    lv_obj_remove_style_all(rect);
    lv_obj_set_pos(rect, x, y);
    lv_obj_set_size(rect, width, height);
    lv_obj_set_style_bg_color(rect, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(rect, LV_OBJ_FLAG_SCROLLABLE);
    /* Every rectangle in this pattern is decoration. Taking the click flag off
     * here means the screen receives every press wherever it lands, so the
     * pattern below can hit-test the whole panel itself instead of depending on
     * which widget happens to be topmost. */
    lv_obj_remove_flag(rect, LV_OBJ_FLAG_CLICKABLE);
    return rect;
}

static void moving_marker_x(void *object, int32_t x)
{
    lv_obj_set_x((lv_obj_t *) object, x);
}

/*
 * The touch half of the bring-up pattern. §7.5 puts the real minimum touch
 * target at 48 px, and everything here is larger, because what is under test is
 * whether a coordinate is true rather than whether a person can hit a button.
 */
#define SLATE_BRINGUP_CORNER_SIZE 60
#define SLATE_BRINGUP_CORNER_INSET 2
#define SLATE_BRINGUP_BACKLIGHT_X 300
#define SLATE_BRINGUP_BACKLIGHT_Y 60
#define SLATE_BRINGUP_BACKLIGHT_W 200
#define SLATE_BRINGUP_BACKLIGHT_H 80
#define SLATE_BRINGUP_BACKLIGHT_WAKE_MS 5000

/*
 * Half the length of a crosshair arm, and the reason it is a number at all.
 *
 * The first version drew the crosshair as two lines spanning the whole panel,
 * and following a finger with it was visibly slow. The cause is not the input
 * device: LVGL joins invalidated areas that intersect into their bounding box,
 * and a full-width line always intersects a full-height one, so the bounding
 * box was the entire 800×480 screen — every touch sample repainting all 130-odd
 * objects of this pattern, twice over, because direct render mode has to keep
 * the second framebuffer consistent as well.
 *
 * A bounded cross costs its own bounding box and nothing else. The lesson
 * belongs to #20 rather than to this pattern: a widget that spans the screen
 * turns every update anywhere near it into a full-screen repaint.
 */
#define SLATE_BRINGUP_CROSS_ARM 60
#define SLATE_BRINGUP_CROSS_THICK 2

/* The readout is for reading, and a hundred repaints a second of a line of text
 * nobody can follow is the same full-screen-repaint mistake in miniature. */
#define SLATE_BRINGUP_LABEL_PERIOD_MS 100

#define SLATE_BRINGUP_IDLE      0x3A3F4A
#define SLATE_BRINGUP_HIT       0x2FBF71
#define SLATE_BRINGUP_CROSSHAIR 0x00E5FF

static lv_obj_t *s_corner[4];
static lv_obj_t *s_backlight_tile;
static lv_obj_t *s_crosshair_h;
static lv_obj_t *s_crosshair_v;
static lv_obj_t *s_touch_label;
static lv_timer_t *s_backlight_timer;
static bool s_corner_hit[4];
static uint32_t s_press_count;
static uint32_t s_label_at_ms;
static lv_point_t s_last_point = {-1, -1};

/*
 * The widget is asked where it is rather than told. `solid_rect()` takes the
 * click flag off everything so that the screen sees every press wherever it
 * lands, which leaves this pattern doing its own hit-testing — but a second
 * copy of each rectangle's geometry is how a target ends up visibly in one
 * place and touchable in another, and this harness exists to rule out exactly
 * that class of mismatch rather than to introduce one.
 */
static bool point_on_object(lv_obj_t *object, const lv_point_t *point)
{
    if (!object) {
        return false;
    }

    /* Compared here rather than through LVGL's own predicate: 9.5 exposes that
     * one as `_lv_area_is_point_on`, and a leading underscore is a version this
     * firmware does not get to depend on. The coordinates are still the
     * widget's. */
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    return point->x >= area.x1 && point->x <= area.x2 && point->y >= area.y1 &&
           point->y <= area.y2;
}

static void update_touch_label(void)
{
    if (!s_touch_label) {
        return;
    }

    int corners = 0;
    for (size_t i = 0; i < 4; i++) {
        corners += s_corner_hit[i] ? 1 : 0;
    }

    if (s_last_point.x < 0) {
        lv_label_set_text_fmt(s_touch_label, "TOUCH  waiting  |  corners 0/4  |  backlight %s",
                              slate_display_backlight_is_on() ? "on" : "off");
        return;
    }
    lv_label_set_text_fmt(s_touch_label,
                          "TOUCH  %d,%d  |  presses %" PRIu32 "  |  corners %d/4  |  backlight %s",
                          (int) s_last_point.x, (int) s_last_point.y, s_press_count, corners,
                          slate_display_backlight_is_on() ? "on" : "off");
}

static void backlight_on(void)
{
    esp_err_t err = slate_display_backlight_set(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight on: %s", esp_err_to_name(err));
    }
    update_touch_label();
}

/*
 * The reason the tile below is safe to press. A dark panel whose touch
 * controller has just failed is exactly the state this milestone is trying to
 * make impossible to reach without a cable, so the way back does not depend on
 * the thing being tested.
 */
static void backlight_wake_timer_cb(lv_timer_t *timer)
{
    (void) timer;
    s_backlight_timer = NULL; /* one-shot: LVGL frees it after this callback */
    ESP_LOGW(TAG, "backlight restored by the safety timer rather than by a touch");
    backlight_on();
}

static void backlight_wake_now(void)
{
    if (s_backlight_timer) {
        lv_timer_delete(s_backlight_timer);
        s_backlight_timer = NULL;
    }
    ESP_LOGI(TAG, "backlight restored by a touch");
    backlight_on();
}

static void backlight_sleep(void)
{
    esp_err_t err = slate_display_backlight_set(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight off: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "backlight off from firmware; a touch or %d s restores it",
             SLATE_BRINGUP_BACKLIGHT_WAKE_MS / 1000);

    if (!s_backlight_timer) {
        s_backlight_timer =
            lv_timer_create(backlight_wake_timer_cb, SLATE_BRINGUP_BACKLIGHT_WAKE_MS, NULL);
        if (s_backlight_timer) {
            lv_timer_set_repeat_count(s_backlight_timer, 1);
        }
    }
    update_touch_label();
}

static void latch_corners(const lv_point_t *point)
{
    for (size_t i = 0; i < 4; i++) {
        if (s_corner_hit[i] || !point_on_object(s_corner[i], point)) {
            continue;
        }
        s_corner_hit[i] = true;
        lv_obj_set_style_bg_color(s_corner[i], lv_color_hex(SLATE_BRINGUP_HIT), LV_PART_MAIN);
    }
}

/*
 * One handler on the screen rather than a callback per widget. The pattern is
 * asking "is this coordinate the one I touched", and that question is about the
 * whole panel — including the parts of it no widget covers.
 */
static void screen_input_event(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    s_last_point = point;

    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        s_press_count++;
        if (!slate_display_backlight_is_on()) {
            backlight_wake_now();
        } else if (point_on_object(s_backlight_tile, &point)) {
            backlight_sleep();
        }
    }

    /* The crosshair is left where the finger lifted rather than hidden on
     * release: judging a coordinate against the grid behind it is easier with
     * nothing on the glass. */
    if (s_crosshair_h && s_crosshair_v) {
        lv_obj_set_pos(s_crosshair_h, point.x - SLATE_BRINGUP_CROSS_ARM,
                       point.y - SLATE_BRINGUP_CROSS_THICK / 2);
        lv_obj_set_pos(s_crosshair_v, point.x - SLATE_BRINGUP_CROSS_THICK / 2,
                       point.y - SLATE_BRINGUP_CROSS_ARM);
    }
    latch_corners(&point);

    const uint32_t now_ms = lv_tick_get();
    if (lv_event_get_code(event) != LV_EVENT_PRESSING ||
        now_ms - s_label_at_ms >= SLATE_BRINGUP_LABEL_PERIOD_MS) {
        s_label_at_ms = now_ms;
        update_touch_label();
    }
}

static void build_touch_pattern(lv_obj_t *screen)
{
    s_press_count = 0;
    s_last_point.x = -1;
    s_last_point.y = -1;

    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, screen_input_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(screen, screen_input_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(screen, screen_input_event, LV_EVENT_RELEASED, NULL);

    /* Four targets at the physical corners. An axis that is mirrored lights the
     * wrong one, and an edge the controller cannot reach never lights at all —
     * both are the failure this issue has to rule out, and neither needs a
     * ruler to see. */
    const int32_t far_x = SLATE_LCD_H_RES - SLATE_BRINGUP_CORNER_INSET - SLATE_BRINGUP_CORNER_SIZE;
    const int32_t far_y = SLATE_LCD_V_RES - SLATE_BRINGUP_CORNER_INSET - SLATE_BRINGUP_CORNER_SIZE;
    const int32_t corner_x[4] = {SLATE_BRINGUP_CORNER_INSET, far_x, SLATE_BRINGUP_CORNER_INSET,
                                 far_x};
    const int32_t corner_y[4] = {SLATE_BRINGUP_CORNER_INSET, SLATE_BRINGUP_CORNER_INSET, far_y,
                                 far_y};
    for (size_t i = 0; i < 4; i++) {
        s_corner_hit[i] = false;
        s_corner[i] = solid_rect(screen, corner_x[i], corner_y[i], SLATE_BRINGUP_CORNER_SIZE,
                                 SLATE_BRINGUP_CORNER_SIZE, SLATE_BRINGUP_IDLE);
    }

    lv_obj_t *tile = solid_rect(screen, SLATE_BRINGUP_BACKLIGHT_X, SLATE_BRINGUP_BACKLIGHT_Y,
                                SLATE_BRINGUP_BACKLIGHT_W, SLATE_BRINGUP_BACKLIGHT_H, 0x22252B);
    s_backlight_tile = tile;
    lv_obj_set_style_border_color(tile, lv_color_hex(0xF5A524), LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 2, LV_PART_MAIN);
    lv_obj_t *tile_label = lv_label_create(tile);
    lv_label_set_text(tile_label, "BACKLIGHT OFF");
    lv_obj_set_style_text_color(tile_label, lv_color_hex(0xF2F5F9), LV_PART_MAIN);
    lv_obj_center(tile_label);

    s_touch_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_touch_label, lv_color_hex(0x2FBF71), LV_PART_MAIN);
    lv_obj_align(s_touch_label, LV_ALIGN_BOTTOM_MID, 0, -22);

    const int32_t arm = 2 * SLATE_BRINGUP_CROSS_ARM + 1;
    s_crosshair_h = solid_rect(screen, -arm, -arm, arm, SLATE_BRINGUP_CROSS_THICK,
                               SLATE_BRINGUP_CROSSHAIR);
    s_crosshair_v = solid_rect(screen, -arm, -arm, SLATE_BRINGUP_CROSS_THICK, arm,
                               SLATE_BRINGUP_CROSSHAIR);

    update_touch_label();
}

static void build_test_pattern(void *ctx)
{
    (void) ctx;

    static const uint32_t bars[] = {
        0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
        0xFF00FF, 0xFF0000, 0x0000FF, 0x000000,
    };
    static const uint32_t greys[] = {
        0xFFFFFF, 0xDADADA, 0xB6B6B6, 0x919191,
        0x6D6D6D, 0x494949, 0x242424, 0x000000,
    };

    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101114), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < sizeof(bars) / sizeof(bars[0]); i++) {
        solid_rect(screen, (int32_t) i * 100, 0, 100, 180, bars[i]);
        solid_rect(screen, (int32_t) i * 100, 180, 100, 55, greys[i]);
    }

    /* Fine lines and a centre cross make a swapped pin, crop or porch error
     * visible without needing any UI runtime or font assets. */
    for (int32_t x = 0; x < SLATE_LCD_H_RES; x += 20) {
        solid_rect(screen, x, 250, 1, 150, x % 100 == 0 ? 0x6C8CFF : 0x343841);
    }
    for (int32_t y = 250; y < 400; y += 20) {
        solid_rect(screen, 0, y, SLATE_LCD_H_RES, 1,
                   y % 100 == 50 ? 0x6C8CFF : 0x343841);
    }

    /* Draw the physical edge coordinates explicitly. The regular 20 px grid
     * ends at x=780, which can look like a cropped right edge even when all
     * 800 columns are being scanned. These one-pixel rails make a real crop or
     * porch error unambiguous on the glass. */
    solid_rect(screen, 0, 0, SLATE_LCD_H_RES, 1, 0xFFFFFF);
    solid_rect(screen, 0, SLATE_LCD_V_RES - 1, SLATE_LCD_H_RES, 1, 0xFFFFFF);
    solid_rect(screen, 0, 0, 1, SLATE_LCD_V_RES, 0xFFFFFF);
    solid_rect(screen, SLATE_LCD_H_RES - 1, 0, 1, SLATE_LCD_V_RES, 0xFFFFFF);

    solid_rect(screen, SLATE_LCD_H_RES / 2 - 1, 245, 3, 165, 0xF5A524);
    solid_rect(screen, 0, 324, SLATE_LCD_H_RES, 3, 0xF5A524);

    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "SLATE DISPLAY  |  800x480 RGB565  |  2FB + VSYNC + BOUNCE");
    lv_obj_set_style_text_color(label, lv_color_hex(0xF2F5F9), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -42);

    /* Continuous motion is deliberate: a static pattern cannot reveal the
     * frame skipping/flicker that made S-2 require the VSYNC gate. Its travel
     * stops short of the corner targets below, which share this row. */
    lv_obj_t *marker = solid_rect(screen, 80, 458, 40, 12, 0x6C8CFF);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, marker);
    lv_anim_set_exec_cb(&animation, moving_marker_x);
    lv_anim_set_values(&animation, 80, 680);
    lv_anim_set_duration(&animation, 2500);
    lv_anim_set_reverse_duration(&animation, 2500);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_start(&animation);

    build_touch_pattern(screen);

    ESP_LOGI(TAG, "bring-up test pattern built on LVGL task");
}

/* --- §9 recovery presentation ----------------------------------------- */

static lv_obj_t *setup_label(lv_obj_t *parent, const char *text, uint32_t color,
                             int32_t width)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    return label;
}

static void setup_overlay_deleted(lv_event_t *event)
{
    if (lv_event_get_target(event) == s_setup_overlay) {
        s_setup_overlay = NULL;
        atomic_store_explicit(&s_setup_presentation_active, false, memory_order_release);
    }
}

static void hide_setup_overlay(void *ctx)
{
    (void) ctx;
    if (s_setup_overlay) {
        lv_obj_delete(s_setup_overlay);
    }
}

static void show_setup_overlay(void *ctx)
{
    slate_display_setup_t *setup = ctx;

    /* A newer show supersedes a hide requested before this work reached the
     * LVGL task. A hide requested after this store remains pending and wins at
     * the end of the task's iteration. */
    atomic_store_explicit(&s_setup_hide_pending, false, memory_order_release);
    hide_setup_overlay(NULL);

    lv_obj_t *screen = lv_screen_active();
    const int32_t width = setup->banner ? 772 : SLATE_LCD_H_RES;
    const int32_t height = setup->banner ? 132 : SLATE_LCD_V_RES;
    const int32_t x = setup->banner ? 14 : 0;
    const int32_t y = setup->banner ? 14 : 0;

    lv_obj_t *overlay = lv_obj_create(screen);
    s_setup_overlay = overlay;
    lv_obj_remove_style_all(overlay);
    lv_obj_set_pos(overlay, x, y);
    lv_obj_set_size(overlay, width, height);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(setup->banner ? 0x22252B : 0x101114),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(overlay, lv_color_hex(0xF5A524), LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, setup->banner ? 2 : 0, LV_PART_MAIN);
    lv_obj_set_style_radius(overlay, setup->banner ? 18 : 0, LV_PART_MAIN);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, setup_overlay_deleted, LV_EVENT_DELETE, NULL);
    atomic_store_explicit(&s_setup_presentation_active, true, memory_order_release);

    /* §9.4 makes the address on this presentation a recovery mechanism, so it
     * cannot be left behind a dark backlight. While the presentation is active
     * slate_display_backlight_set(false) refuses later dimming as well. */
    backlight_on();

    lv_obj_t *card = overlay;
    if (!setup->banner) {
        card = lv_obj_create(overlay);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, 650, 330);
        lv_obj_center(card);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1C21), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, lv_color_hex(0x343841), LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    }

    const int32_t text_width = setup->banner ? width - 40 : 590;
    lv_obj_t *title = setup_label(card, setup->banner ? "NETWORK OFFLINE" : "SET UP NETWORK",
                                  0xF5A524, text_width);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, setup->banner ? 20 : 30, setup->banner ? 16 : 28);

    char connection[96];
    snprintf(connection, sizeof(connection), "Join %s  |  Open http://%s", setup->network,
             setup->address);
    lv_obj_t *join = setup_label(card, connection, 0xF2F5F9, text_width);
    lv_obj_align(join, LV_ALIGN_TOP_LEFT, setup->banner ? 20 : 30, setup->banner ? 43 : 76);

    if (setup->passphrase[0] != '\0') {
        char password[96];
        snprintf(password, sizeof(password), "WiFi password: %s", setup->passphrase);
        lv_obj_t *pass = setup_label(card, password, 0xF2F5F9, text_width);
        lv_obj_align(pass, LV_ALIGN_TOP_LEFT, setup->banner ? 20 : 30,
                     setup->banner ? 68 : 116);
        explicit_bzero(password, sizeof(password));
    }

    const int32_t message_y = setup->banner ? (setup->passphrase[0] ? 93 : 72)
                                             : (setup->passphrase[0] ? 170 : 140);
    lv_obj_t *message = setup_label(card, setup->message, 0x8A94A6, text_width);
    lv_obj_align(message, LV_ALIGN_TOP_LEFT, setup->banner ? 20 : 30, message_y);

    lv_obj_move_foreground(overlay);
    explicit_bzero(connection, sizeof(connection));
    explicit_bzero(setup, sizeof(*setup));
    free(setup);
}

static void log_vsync_once(void)
{
    static bool logged;
    if (logged) {
        return;
    }

    int64_t period;
    uint32_t count;
    portENTER_CRITICAL(&s_vsync_lock);
    period = s_vsync_period_us;
    count = s_vsync_count;
    portEXIT_CRITICAL(&s_vsync_lock);

    if (count >= 8 && period > 0) {
        ESP_LOGI(TAG, "VSYNC period %lld us (%.2f Hz), expected about 26441 us",
                 (long long) period, 1000000.0 / (double) period);
        logged = true;
    }
}

static void display_task(void *ctx)
{
    (void) ctx;

    s_init_result = panel_init();
    if (s_init_result == ESP_OK) {
        s_init_result = lvgl_display_init();
    }
    s_ready = s_init_result == ESP_OK;
    if (s_ready) {
        /* Here and not in app_main: lv_indev_create() is LVGL, and §6.1 gives
         * LVGL exactly one task. A controller that will not answer costs the
         * panel its input and nothing else — the display, the API and the setup
         * access point are all still worth having. */
        esp_err_t touch_err = slate_touch_init(SLATE_I2C_PORT, s_expander, s_expander_lock);
        if (touch_err != ESP_OK) {
            ESP_LOGE(TAG, "touch unavailable: %s — continuing without input",
                     esp_err_to_name(touch_err));
        }

        update_heap_metrics();
        ESP_LOGI(TAG, "display ready: %u B internal DMA-capable, %u B PSRAM free, touch %s",
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 slate_touch_ready() ? "up" : "absent");
        xSemaphoreGive(s_init_done);
    } else {
        display_resources_deinit();
        s_task = NULL;
        xSemaphoreGive(s_init_done);
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms == 0 || wait_ms > 10) {
            wait_ms = wait_ms == 0 ? 1 : 10;
        }

        slate_display_work_t work;
        if (xQueueReceive(s_work_queue, &work, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
            work.fn(work.ctx);
        }
        /* Hide is desired state rather than ordinary queued work. A full work
         * queue must not leave the recovery card over a dashboard after the
         * station has returned. Checking after work also preserves show/hide
         * ordering when a show was already queued. */
        if (atomic_exchange_explicit(&s_setup_hide_pending, false, memory_order_acq_rel)) {
            hide_setup_overlay(NULL);
        }
        service_flush_gate();
        log_vsync_once();
        update_heap_metrics();
    }
}

/* Every synchronisation object the task needs, released together. The three
 * call sites below differ only in which of them exist yet, and a NULL handle is
 * one that has already gone. */
static void primitives_free(void)
{
    if (s_work_queue) {
        vQueueDelete(s_work_queue);
        s_work_queue = NULL;
    }
    if (s_init_done) {
        vSemaphoreDelete(s_init_done);
        s_init_done = NULL;
    }
    if (s_expander_lock) {
        vSemaphoreDelete(s_expander_lock);
        s_expander_lock = NULL;
    }
}

esp_err_t slate_display_init(void)
{
    if (s_task || s_work_queue || s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    s_init_result = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_heap_metrics_lock);
    memset(&s_heap_metrics, 0, sizeof(s_heap_metrics));
    s_heap_metrics_at_us = 0;
    portEXIT_CRITICAL(&s_heap_metrics_lock);
    portENTER_CRITICAL(&s_vsync_lock);
    s_vsync_last_us = 0;
    s_vsync_period_us = 0;
    s_vsync_count = 0;
    portEXIT_CRITICAL(&s_vsync_lock);
    atomic_store_explicit(&s_flush_pending, false, memory_order_relaxed);
    atomic_store_explicit(&s_frame_presented, false, memory_order_relaxed);

    s_work_queue = xQueueCreate(SLATE_DISPLAY_QUEUE_LEN, sizeof(slate_display_work_t));
    s_init_done = xSemaphoreCreateBinary();
    s_expander_lock = xSemaphoreCreateMutex();
    if (!s_work_queue || !s_init_done || !s_expander_lock) {
        primitives_free();
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(display_task, "slate_lvgl", SLATE_DISPLAY_TASK_STACK,
                                NULL, SLATE_DISPLAY_TASK_PRIORITY, &s_task,
                                SLATE_DISPLAY_TASK_CORE) != pdPASS) {
        s_task = NULL;
        primitives_free();
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_init_done, pdMS_TO_TICKS(SLATE_DISPLAY_INIT_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(s_init_done);
    s_init_done = NULL;
    if (s_init_result != ESP_OK) {
        primitives_free();
        return s_init_result;
    }

    /* The first real tree crosses the same queue boundary every later UI
     * rebuild uses; bring-up therefore exercises the architecture, not a
     * privileged init-only path. */
    return slate_display_post(build_test_pattern, NULL, 1000);
}

esp_err_t slate_display_post(slate_display_work_fn fn, void *ctx, uint32_t timeout_ms)
{
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || !s_work_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    const slate_display_work_t work = {.fn = fn, .ctx = ctx};
    return xQueueSend(s_work_queue, &work, pdMS_TO_TICKS(timeout_ms)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t slate_display_setup_show(const slate_display_setup_t *setup)
{
    if (!setup || setup->network[0] == '\0' || setup->address[0] == '\0' ||
        setup->message[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    slate_display_setup_t *copy = malloc(sizeof(*copy));
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    *copy = *setup;
    copy->network[sizeof(copy->network) - 1] = '\0';
    copy->address[sizeof(copy->address) - 1] = '\0';
    copy->passphrase[sizeof(copy->passphrase) - 1] = '\0';
    copy->message[sizeof(copy->message) - 1] = '\0';

    esp_err_t err = slate_display_post(show_setup_overlay, copy, 1000);
    if (err != ESP_OK) {
        explicit_bzero(copy, sizeof(*copy));
        free(copy);
    }
    return err;
}

esp_err_t slate_display_setup_hide(void)
{
    if (!s_ready || !s_work_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    atomic_store_explicit(&s_setup_hide_pending, true, memory_order_release);
    return ESP_OK;
}

bool slate_display_ready(void)
{
    return s_ready;
}

esp_err_t slate_display_backlight_set(bool on)
{
    if (!on && atomic_load_explicit(&s_setup_presentation_active, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_expander || !s_expander_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_expander_lock, pdMS_TO_TICKS(SLATE_DISPLAY_EXPANDER_TIMEOUT_MS)) !=
        pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = esp_io_expander_set_level(s_expander, SLATE_EXIO_DISP, on ? 1 : 0);
    if (err == ESP_OK) {
        s_backlight_on = on;
    }
    xSemaphoreGive(s_expander_lock);
    return err;
}

bool slate_display_backlight_is_on(void)
{
    return s_backlight_on;
}

slate_display_backlight_mode_t slate_display_backlight_mode(void)
{
    /* S-2 confirmed EXIO2 on this board is a binary output with no PWM path.
     * #41 owns the soldered variant and the pin it would need. */
    return SLATE_DISPLAY_BACKLIGHT_ON_OFF;
}

void slate_display_heap_metrics(slate_display_heap_metrics_t *out)
{
    if (!out) {
        return;
    }

    portENTER_CRITICAL(&s_heap_metrics_lock);
    *out = s_heap_metrics;
    portEXIT_CRITICAL(&s_heap_metrics_lock);
}
