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

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "slate_ch422g.h"
#include "slate_display.h"
#include "slate_theme.h"
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

/* EXIO1 is the touch controller's reset and belongs to slate_touch, which has
 * to hold it while the controller's I²C address is selected. */
#define SLATE_EXIO_DISP    SLATE_CH422G_PIN(2)
#define SLATE_EXIO_LCD_RST SLATE_CH422G_PIN(3)

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
#define SLATE_DISPLAY_HEAP_METRICS_PERIOD_US (15LL * 1000000)
#define SLATE_IDENTIFY_PHASE_MS               180
#define SLATE_IDENTIFY_PHASES                 6
#define SLATE_SETUP_QR_PAYLOAD_MAX            224

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
/* The CH422G driver serializes its cached output update with the corresponding
 * bus write, so touch reset and #28's future backlight task preserve one
 * another's pins without a second lock here. */
static TaskHandle_t s_task;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_display;
static i2c_master_bus_handle_t s_i2c_bus;
static slate_ch422g_handle_t s_expander;
static void *s_lvgl_pool;
static esp_err_t s_init_result = ESP_ERR_INVALID_STATE;
static bool s_lvgl_initialized;
static bool s_ready;
static atomic_uchar s_backlight_level = ATOMIC_VAR_INIT(0);
static bool s_first_frame_shown;
static slate_display_heap_metrics_t s_heap_metrics;
static int64_t s_heap_metrics_at_us;
static lv_obj_t *s_setup_overlay;
static lv_obj_t *s_identify_overlay;
static lv_timer_t *s_identify_timer;
static unsigned s_identify_phase;
static atomic_bool s_setup_presentation_active = ATOMIC_VAR_INIT(false);
static atomic_bool s_setup_hide_pending = ATOMIC_VAR_INIT(false);

static void identify_cleanup(void);

/* --- Compact strings used by the setup QR ------------------------------ */

static bool qr_append(char *out, size_t capacity, size_t *used, char value)
{
    if (*used + 1 >= capacity) {
        return false;
    }
    out[(*used)++] = value;
    out[*used] = '\0';
    return true;
}

static bool qr_append_text(char *out, size_t capacity, size_t *used, const char *text,
                           bool escape)
{
    while (*text != '\0') {
        /* ZXing's Wi-Fi payload grammar treats these as syntax. Escaping the
         * full set accepted by common iOS/Android scanners also handles an AP
         * name chosen from a MAC today and a user-chosen name in the future. */
        if (escape && strchr("\\;,\":", *text) != NULL &&
            !qr_append(out, capacity, used, '\\')) {
            return false;
        }
        if (!qr_append(out, capacity, used, *text++)) {
            return false;
        }
    }
    return true;
}

static bool setup_qr_payload(const slate_display_setup_t *setup, char *out, size_t capacity)
{
    size_t used = 0;
    const bool secured = setup->passphrase[0] != '\0';

    out[0] = '\0';
    if (!qr_append_text(out, capacity, &used, secured ? "WIFI:T:WPA;S:" : "WIFI:T:nopass;S:",
                        false) ||
        !qr_append_text(out, capacity, &used, setup->network, true)) {
        return false;
    }
    if (secured &&
        (!qr_append_text(out, capacity, &used, ";P:", false) ||
         !qr_append_text(out, capacity, &used, setup->passphrase, true))) {
        return false;
    }
    return qr_append_text(out, capacity, &used, ";;", false);
}

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
    const i2c_master_bus_config_t config = {
        .i2c_port = SLATE_I2C_PORT,
        .sda_io_num = SLATE_I2C_SDA_GPIO,
        .scl_io_num = SLATE_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, &s_i2c_bus);
}

static esp_err_t expander_init(void)
{
    ESP_RETURN_ON_ERROR(slate_ch422g_new(s_i2c_bus, &s_expander), TAG, "CH422G");

    /* Keep the glass dark until a complete frame exists. */
    esp_err_t err = slate_ch422g_set_level(s_expander, SLATE_EXIO_DISP, false);
    if (err == ESP_OK) {
        err = slate_ch422g_set_level(s_expander, SLATE_EXIO_LCD_RST, false);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        err = slate_ch422g_set_level(s_expander, SLATE_EXIO_LCD_RST, true);
    }
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
        cleanup_error("backlight", slate_ch422g_set_level(s_expander, SLATE_EXIO_DISP, false));
        atomic_store_explicit(&s_backlight_level, 0, memory_order_release);
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
        cleanup_error("CH422G", slate_ch422g_del(s_expander));
        s_expander = NULL;
    }
    if (s_i2c_bus) {
        cleanup_error("I2C", i2c_del_master_bus(s_i2c_bus));
        s_i2c_bus = NULL;
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
    const slate_diagnostic_palette_t *diagnostics = slate_diagnostic_palette();
    for (size_t i = 0; i < 4; i++) {
        if (s_corner_hit[i] || !point_on_object(s_corner[i], point)) {
            continue;
        }
        s_corner_hit[i] = true;
        lv_obj_set_style_bg_color(s_corner[i], lv_color_hex(diagnostics->touch_hit),
                                  LV_PART_MAIN);
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
    const slate_theme_t *theme = slate_theme_default();
    const slate_diagnostic_palette_t *diagnostics = slate_diagnostic_palette();
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
                                 SLATE_BRINGUP_CORNER_SIZE, diagnostics->touch_idle);
    }

    lv_obj_t *tile = solid_rect(screen, SLATE_BRINGUP_BACKLIGHT_X, SLATE_BRINGUP_BACKLIGHT_Y,
                                SLATE_BRINGUP_BACKLIGHT_W, SLATE_BRINGUP_BACKLIGHT_H,
                                theme->surface_alt);
    s_backlight_tile = tile;
    lv_obj_set_style_border_color(tile, lv_color_hex(theme->warn), LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(tile, theme->radius, LV_PART_MAIN);
    lv_obj_t *tile_label = lv_label_create(tile);
    lv_label_set_text(tile_label, "BACKLIGHT OFF");
    lv_obj_set_style_text_color(tile_label, lv_color_hex(theme->text_hi), LV_PART_MAIN);
    lv_obj_set_style_text_font(tile_label, theme->body, LV_PART_MAIN);
    lv_obj_center(tile_label);

    s_touch_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_touch_label, lv_color_hex(diagnostics->touch_hit), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_touch_label, theme->caption, LV_PART_MAIN);
    lv_obj_align(s_touch_label, LV_ALIGN_BOTTOM_MID, 0, -22);

    const int32_t arm = 2 * SLATE_BRINGUP_CROSS_ARM + 1;
    s_crosshair_h = solid_rect(screen, -arm, -arm, arm, SLATE_BRINGUP_CROSS_THICK,
                               diagnostics->crosshair);
    s_crosshair_v = solid_rect(screen, -arm, -arm, SLATE_BRINGUP_CROSS_THICK, arm,
                               diagnostics->crosshair);

    update_touch_label();
}

static void bringup_screen_deleted(lv_event_t *event)
{
    (void) event;
    if (s_backlight_timer != NULL) {
        lv_timer_delete(s_backlight_timer);
        s_backlight_timer = NULL;
    }
    memset(s_corner, 0, sizeof(s_corner));
    s_backlight_tile = NULL;
    s_crosshair_h = NULL;
    s_crosshair_v = NULL;
    s_touch_label = NULL;
}

static void build_test_pattern(void *ctx)
{
    (void) ctx;
    const slate_theme_t *theme = slate_theme_default();
    const slate_diagnostic_palette_t *diagnostics = slate_diagnostic_palette();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_add_event_cb(screen, bringup_screen_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(theme->bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < SLATE_DIAGNOSTIC_SWATCH_COUNT; i++) {
        solid_rect(screen, (int32_t) i * 100, 0, 100, 180, diagnostics->rgb[i]);
        solid_rect(screen, (int32_t) i * 100, 180, 100, 55, diagnostics->greyscale[i]);
    }

    /* Fine lines and a centre cross make a swapped pin, crop or porch error
     * visible without needing any UI runtime or font assets. */
    for (int32_t x = 0; x < SLATE_LCD_H_RES; x += 20) {
        solid_rect(screen, x, 250, 1, 150,
                   x % 100 == 0 ? theme->accent : theme->surface_alt);
    }
    for (int32_t y = 250; y < 400; y += 20) {
        solid_rect(screen, 0, y, SLATE_LCD_H_RES, 1,
                   y % 100 == 50 ? theme->accent : theme->surface_alt);
    }

    /* Draw the physical edge coordinates explicitly. The regular 20 px grid
     * ends at x=780, which can look like a cropped right edge even when all
     * 800 columns are being scanned. These one-pixel rails make a real crop or
     * porch error unambiguous on the glass. */
    solid_rect(screen, 0, 0, SLATE_LCD_H_RES, 1, diagnostics->edge);
    solid_rect(screen, 0, SLATE_LCD_V_RES - 1, SLATE_LCD_H_RES, 1, diagnostics->edge);
    solid_rect(screen, 0, 0, 1, SLATE_LCD_V_RES, diagnostics->edge);
    solid_rect(screen, SLATE_LCD_H_RES - 1, 0, 1, SLATE_LCD_V_RES, diagnostics->edge);

    solid_rect(screen, SLATE_LCD_H_RES / 2 - 1, 245, 3, 165, theme->warn);
    solid_rect(screen, 0, 324, SLATE_LCD_H_RES, 3, theme->warn);

    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "SLATE DISPLAY  |  800x480 RGB565  |  2FB + VSYNC + BOUNCE");
    lv_obj_set_style_text_color(label, lv_color_hex(theme->text_hi), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, theme->body, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -42);

    /* Continuous motion is deliberate: a static pattern cannot reveal the
     * frame skipping/flicker that made S-2 require the VSYNC gate. Its travel
     * stops short of the corner targets below, which share this row. */
    lv_obj_t *marker = solid_rect(screen, 80, 458, 40, 12, theme->accent);
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
                             const lv_font_t *font, int32_t width)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    return label;
}

/* Hold a setup row to one line, so a value too long for it is truncated with an
 * ellipsis instead of pushing into the row below.
 *
 * LV_LABEL_LONG_DOT does not mean "one line". LVGL wraps the text to the
 * label's width first and only replaces the tail with dots once the wrapped
 * result overflows the label's *height* — and a label with no height set
 * reports the wrapped text as its own height, so it never overflows and never
 * truncates. §7.5's dashboard labels use slate_component_label_one_line() for
 * the same reason. This component cannot call it: slate_ui requires
 * slate_display and not the other way round, and that helper is private to it.
 */
static void setup_label_one_line(lv_obj_t *label)
{
    /* No NULL guard, unlike the slate_ui twin: there make_label() can return
     * NULL and its callers test for it, here every row is dereferenced by the
     * lv_obj_align() on the next line. A guard would only suggest a safety
     * this call site does not have. */
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    /* Against the content box, not the outer bounds: a row that later gains
     * padding must still have one full line inside it rather than become dots
     * while its text fits. */
    lv_obj_set_height(label, lv_font_get_line_height(font) +
                                 lv_obj_get_style_space_top(label, LV_PART_MAIN) +
                                 lv_obj_get_style_space_bottom(label, LV_PART_MAIN));
}

static lv_obj_t *setup_qr(lv_obj_t *parent, const slate_display_setup_t *setup,
                          int32_t size)
{
    char payload[SLATE_SETUP_QR_PAYLOAD_MAX] = {0};
    if (!setup_qr_payload(setup, payload, sizeof(payload))) {
        ESP_LOGE(TAG, "setup Wi-Fi QR payload does not fit");
        return NULL;
    }

    lv_obj_t *qr = lv_qrcode_create(parent);
    if (qr != NULL) {
        lv_qrcode_set_size(qr, size);
        /* QR contrast is a machine interface, not a theme surface. Several
         * phone cameras reject inverted symbols even when their ratio is good. */
        lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
        lv_qrcode_set_quiet_zone(qr, true);
        if (lv_qrcode_update(qr, payload, strlen(payload)) != LV_RESULT_OK) {
            lv_obj_delete(qr);
            qr = NULL;
            ESP_LOGE(TAG, "encoding setup Wi-Fi QR");
        }
    }
    explicit_bzero(payload, sizeof(payload));
    return qr;
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
    const slate_theme_t *theme = slate_theme_default();

    /* A newer show supersedes a hide requested before this work reached the
     * LVGL task. A hide requested after this store remains pending and wins at
     * the end of the task's iteration. */
    atomic_store_explicit(&s_setup_hide_pending, false, memory_order_release);
    hide_setup_overlay(NULL);

    lv_obj_t *screen = lv_screen_active();
    const int32_t width = setup->banner ? SLATE_LCD_H_RES - 2 * theme->pad : SLATE_LCD_H_RES;
    const int32_t height = setup->banner ? 164 : SLATE_LCD_V_RES;
    const int32_t x = setup->banner ? theme->pad : 0;
    const int32_t y = setup->banner ? theme->pad : 0;

    lv_obj_t *overlay = lv_obj_create(screen);
    s_setup_overlay = overlay;
    lv_obj_remove_style_all(overlay);
    lv_obj_set_pos(overlay, x, y);
    lv_obj_set_size(overlay, width, height);
    lv_obj_set_style_bg_color(
        overlay, lv_color_hex(setup->banner ? theme->surface_alt : theme->bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(overlay, lv_color_hex(theme->warn), LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, setup->banner ? 2 : 0, LV_PART_MAIN);
    lv_obj_set_style_radius(overlay, setup->banner ? theme->radius : 0, LV_PART_MAIN);
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
        lv_obj_set_size(card, 748, 404);
        lv_obj_center(card);
        lv_obj_set_style_bg_color(card, lv_color_hex(theme->surface), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, lv_color_hex(theme->surface_alt), LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(card, theme->radius, LV_PART_MAIN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    }

    const int32_t qr_size = setup->banner ? 116 : 248;
    lv_obj_t *qr = setup_qr(card, setup, qr_size);
    if (qr != NULL) {
        lv_obj_align(qr, setup->banner ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID,
                     setup->banner ? -18 : 24, 0);
    }

    if (setup->banner) {
        const int32_t text_width = qr != NULL ? width - qr_size - 64 : width - 40;
        lv_obj_t *title = setup_label(card, "NETWORK OFFLINE", theme->warn, theme->body,
                                      text_width);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 14);

        char connection[96];
        snprintf(connection, sizeof(connection), "Scan to join %s", setup->network);
        lv_obj_t *join = setup_label(card, connection, theme->text_hi, theme->body, text_width);
        lv_obj_align(join, LV_ALIGN_TOP_LEFT, 20, 43);

        char security[96];
        if (setup->passphrase[0] == '\0') {
            strlcpy(security, "No Wi-Fi password", sizeof(security));
        } else {
            snprintf(security, sizeof(security), "Password: %s", setup->passphrase);
        }
        lv_obj_t *password = setup_label(card, security, theme->text_hi, theme->caption,
                                         text_width);
        lv_obj_align(password, LV_ALIGN_TOP_LEFT, 20, 72);

        char fallback[64];
        snprintf(fallback, sizeof(fallback), "Or open http://%s", setup->address);
        lv_obj_t *address = setup_label(card, fallback, theme->text_hi, theme->caption,
                                        text_width);
        lv_obj_align(address, LV_ALIGN_TOP_LEFT, 20, 96);

        lv_obj_t *message = setup_label(card, setup->message, theme->text_lo, theme->caption,
                                        text_width);
        lv_obj_align(message, LV_ALIGN_TOP_LEFT, 20, 120);
        explicit_bzero(connection, sizeof(connection));
        explicit_bzero(security, sizeof(security));
        explicit_bzero(fallback, sizeof(fallback));
    } else {
        const int32_t text_x = qr != NULL ? 306 : 34;
        const int32_t text_width = qr != NULL ? 408 : 680;
        lv_obj_t *title = setup_label(card, "CONNECT THIS PANEL", theme->warn, theme->body,
                                      text_width);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, text_x, 26);

        char join[96];
        snprintf(join, sizeof(join), "1   Scan to join %s", setup->network);
        lv_obj_t *join_label = setup_label(card, join, theme->text_hi, theme->body, text_width);
        /* §9.2 allows the full 32-byte SSID, which is wider than these 408 px.
         * The security row below is where the passphrase is, so this row is
         * held to one line rather than allowed to grow over it. */
        setup_label_one_line(join_label);
        lv_obj_align(join_label, LV_ALIGN_TOP_LEFT, text_x, 74);

        char security[96];
        if (setup->passphrase[0] == '\0') {
            strlcpy(security, "No Wi-Fi password is required.", sizeof(security));
        } else {
            snprintf(security, sizeof(security), "Password   %s", setup->passphrase);
        }
        lv_obj_t *security_label = setup_label(card, security, theme->text_lo, theme->caption,
                                                text_width);
        lv_obj_align(security_label, LV_ALIGN_TOP_LEFT, text_x, 108);

        char open[64];
        snprintf(open, sizeof(open), "2   Open http://%s", setup->address);
        lv_obj_t *open_label = setup_label(card, open, theme->text_hi, theme->body, text_width);
        lv_obj_align(open_label, LV_ALIGN_TOP_LEFT, text_x, 154);

        lv_obj_t *fallback = setup_label(
            card, "If your phone closes its setup window, type that address in a browser.",
            theme->text_lo, theme->caption, text_width);
        lv_obj_align(fallback, LV_ALIGN_TOP_LEFT, text_x, 188);

        lv_obj_t *message = setup_label(card, setup->message, theme->warn, theme->caption,
                                        text_width);
        lv_obj_align(message, LV_ALIGN_TOP_LEFT, text_x, 248);

        lv_obj_t *recovery = setup_label(
            card, "Recovery: hold anywhere on the screen for 10 seconds to erase all settings.",
            theme->text_lo, theme->caption, text_width);
        lv_obj_align(recovery, LV_ALIGN_TOP_LEFT, text_x, 318);
        explicit_bzero(join, sizeof(join));
        explicit_bzero(security, sizeof(security));
        explicit_bzero(open, sizeof(open));
    }

    lv_obj_move_foreground(overlay);
    explicit_bzero(setup, sizeof(*setup));
    free(setup);
}

static void show_factory_reset(void *ctx)
{
    (void) ctx;
    const slate_theme_t *theme = slate_theme_default();
    hide_setup_overlay(NULL);
    identify_cleanup();

    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, SLATE_LCD_H_RES, SLATE_LCD_V_RES);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(theme->bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    atomic_store_explicit(&s_setup_presentation_active, true, memory_order_release);

    lv_obj_t *title = setup_label(overlay, "FACTORY RESET", theme->warn, theme->hero, 700);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -58);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *body = setup_label(
        overlay, "Erasing the dashboard, credentials and security settings.\n"
                 "The panel will restart in network setup mode.",
        theme->text_hi, theme->body, 700);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 44);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_move_foreground(overlay);
    backlight_on();
}

/* --- §4.1 panel identification ---------------------------------------- */

static void identify_cleanup(void)
{
    if (s_identify_timer != NULL) {
        lv_timer_delete(s_identify_timer);
        s_identify_timer = NULL;
    }
    if (s_identify_overlay != NULL) {
        lv_obj_delete(s_identify_overlay);
        s_identify_overlay = NULL;
    }
}

static void identify_tick(lv_timer_t *timer)
{
    (void) timer;
    s_identify_phase++;
    if (s_identify_phase >= SLATE_IDENTIFY_PHASES) {
        identify_cleanup();
        return;
    }
    lv_obj_set_style_bg_opa(s_identify_overlay,
                            (s_identify_phase & 1u) == 0 ? LV_OPA_80 : LV_OPA_TRANSP,
                            LV_PART_MAIN);
}

static void identify_start(void *ctx)
{
    (void) ctx;
    identify_cleanup();

    const slate_theme_t *theme = slate_theme_default();
    s_identify_overlay = lv_obj_create(lv_layer_top());
    if (s_identify_overlay == NULL) {
        return;
    }
    lv_obj_remove_style_all(s_identify_overlay);
    lv_obj_set_size(s_identify_overlay, SLATE_LCD_H_RES, SLATE_LCD_V_RES);
    lv_obj_set_pos(s_identify_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_identify_overlay, lv_color_hex(theme->accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_identify_overlay, LV_OPA_80, LV_PART_MAIN);
    lv_obj_add_flag(s_identify_overlay, LV_OBJ_FLAG_CLICKABLE);

    s_identify_phase = 0;
    s_identify_timer = lv_timer_create(identify_tick, SLATE_IDENTIFY_PHASE_MS, NULL);
    if (s_identify_timer == NULL) {
        identify_cleanup();
    }
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
        esp_err_t touch_err = slate_touch_init(s_i2c_bus, s_expander);
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

/* Every synchronisation object the task needs, released together. The call
 * sites below differ only in which of them exist yet, and a NULL handle is one
 * that has already gone. */
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
    if (!s_work_queue || !s_init_done) {
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

esp_err_t slate_display_identify(void)
{
    return slate_display_post(identify_start, NULL, 100);
}

esp_err_t slate_display_factory_reset_show(void)
{
    return slate_display_post(show_factory_reset, NULL, 100);
}

bool slate_display_ready(void)
{
    return s_ready;
}

esp_err_t slate_display_backlight_set(bool on)
{
    return slate_display_brightness_set(on ? 100 : 0);
}

esp_err_t slate_display_brightness_set(uint8_t percent)
{
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_expander) {
        return ESP_ERR_INVALID_STATE;
    }

    if (percent < 100 &&
        atomic_load_explicit(&s_setup_presentation_active, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* S-2 confirmed the installed path is a binary CH422G output. Keep the
     * percentage contract here so #41's configured LEDC variant does not leak
     * into the scheduler or its callers. */
    uint8_t actual = percent == 0 ? 0 : 100;
    esp_err_t err = slate_ch422g_set_level(s_expander, SLATE_EXIO_DISP, actual > 0);
    if (err == ESP_OK) {
        atomic_store_explicit(&s_backlight_level, actual, memory_order_release);
    }
    return err;
}

bool slate_display_backlight_is_on(void)
{
    return atomic_load_explicit(&s_backlight_level, memory_order_acquire) > 0;
}

uint8_t slate_display_brightness_level(void)
{
    return atomic_load_explicit(&s_backlight_level, memory_order_acquire);
}

bool slate_display_setup_active(void)
{
    return atomic_load_explicit(&s_setup_presentation_active, memory_order_acquire);
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

#ifdef SLATE_DISPLAY_SELFTEST

/* §9.2 lets the setup network be the full 32 bytes the standard allows, and
 * that is what the card has to survive — not the shorter name a particular
 * router happens to be called. Tied to the header constant, so shortening one
 * without the other is a compile error rather than a fixture that quietly
 * stops being the maximum. */
#define SETUP_TEST_SSID "Ridiculously-Long-Network-Name32"
_Static_assert(sizeof(SETUP_TEST_SSID) == SLATE_DISPLAY_SETUP_NETWORK_LEN,
               "the setup fixture must be a maximum-length SSID");

/* The passphrase row is the reachable half of the same question: §9.2's
 * SLATE_SETUP_AP_PASSWORD is a build-time knob, so a panel really can be
 * carrying one of these. It is not held to one line and must not be — a
 * truncated passphrase cannot be typed, which is the whole reason it is on the
 * screen. What is checked is that the rows below it stay where they are, and
 * the two presentations have different amounts of room for that: the card
 * leaves 46 px, two caption lines, which is what this value needs in its 408 px
 * column; the banner leaves 24 px, one line, and this value measures 18 px in
 * its wider 592 px column. Six pixels is exactly why that one is a check rather
 * than a hope. The fixture is the longest value the field can hold, so nothing
 * a panel can be provisioned with is longer than what is measured here. */
#define SETUP_TEST_PASSPHRASE \
    "correct-horse-battery-staple-correct-horse-battery-staple-abcdef"
_Static_assert(sizeof(SETUP_TEST_PASSPHRASE) == SLATE_DISPLAY_SETUP_PASSPHRASE_LEN,
               "the setup fixture must be a maximum-length passphrase");

typedef struct {
    SemaphoreHandle_t done;
    esp_err_t result;
} setup_selftest_request_t;

/* Rows are found by what they say rather than by their position among the
 * card's children, so reordering the presentation does not silently move a
 * check onto a different label. LV_LABEL_LONG_DOT rewrites the tail, never the
 * head, so a prefix still identifies a truncated row.
 *
 * A NULL prefix matches the first object of the class instead, which is how the
 * QR is located. A prefix is only meaningful for lv_label_class — it is read
 * with lv_label_get_text() — so any other class must pass NULL. */
static lv_obj_t *setup_find(lv_obj_t *parent, const lv_obj_class_t *class_p,
                            const char *prefix)
{
    const uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(child, class_p) &&
            (prefix == NULL ||
             strncmp(lv_label_get_text(child), prefix, strlen(prefix)) == 0)) {
            return child;
        }
        lv_obj_t *nested = setup_find(child, class_p, prefix);
        if (nested != NULL) {
            return nested;
        }
    }
    return NULL;
}

static lv_obj_t *setup_find_label(lv_obj_t *parent, const char *prefix)
{
    return setup_find(parent, &lv_label_class, prefix);
}

/* Built through the work function the queue would have called: this already
 * runs on the LVGL task, and §6.1 gives the presentation one path in, not a
 * privileged second one for the verifier. Ownership of the copy is the same as
 * slate_display_setup_show() hands over. */
static lv_obj_t *setup_selftest_build(const char *ssid, const char *passphrase, bool banner)
{
    slate_display_setup_t *copy = calloc(1, sizeof(*copy));
    if (copy == NULL) {
        return NULL;
    }
    copy->banner = banner;
    strlcpy(copy->network, ssid, sizeof(copy->network));
    strlcpy(copy->address, "192.168.4.1", sizeof(copy->address));
    strlcpy(copy->passphrase, passphrase, sizeof(copy->passphrase));
    strlcpy(copy->message, "Setup self-test", sizeof(copy->message));

    show_setup_overlay(copy);
    if (s_setup_overlay != NULL) {
        lv_obj_update_layout(s_setup_overlay);
    }
    return s_setup_overlay;
}

static bool setup_row_is_one_line(lv_obj_t *label)
{
    const lv_font_t *font =
        label != NULL ? lv_obj_get_style_text_font(label, LV_PART_MAIN) : NULL;
    return font != NULL &&
           lv_obj_get_height(label) == lv_font_get_line_height(font) +
                                           lv_obj_get_style_space_top(label, LV_PART_MAIN) +
                                           lv_obj_get_style_space_bottom(label, LV_PART_MAIN);
}

static bool setup_rows_clear(lv_obj_t *above, lv_obj_t *below)
{
    return above != NULL && below != NULL &&
           lv_obj_get_y(above) + lv_obj_get_height(above) <= lv_obj_get_y(below);
}

static esp_err_t setup_selftest_on_task(void)
{
    unsigned checks = 0;
    unsigned failures = 0;
#define SETUP_CHECK(condition, description)                                          \
    do {                                                                             \
        bool passed_ = (condition);                                                  \
        checks++;                                                                    \
        failures += !passed_;                                                        \
        ESP_LOGI(TAG, "selftest: %-52s %s", description, passed_ ? "PASS" : "FAIL");  \
    } while (0)

    /* Checked here rather than only where the work was requested. slate_setup
     * raises §9.4's card from a wifi event, seconds after start_network()
     * returned, so a card that appeared between the request and this task's
     * turn would be deleted by the fixtures below — and only a state
     * transition ever calls show, so nothing would put it back. */
    if (slate_display_setup_active()) {
        ESP_LOGW(TAG, "setup selftest skipped: a setup presentation is on screen");
        return ESP_ERR_INVALID_STATE;
    }

    lv_obj_t *overlay = setup_selftest_build(SETUP_TEST_SSID, "", false);
    lv_obj_t *join = overlay != NULL ? setup_find_label(overlay, "1   Scan to join") : NULL;
    lv_obj_t *security = overlay != NULL ? setup_find_label(overlay, "No Wi-Fi password") : NULL;
    const char *long_text = join != NULL ? lv_label_get_text(join) : "";
    const int32_t long_y = join != NULL ? lv_obj_get_y(join) : -1;

    SETUP_CHECK(join != NULL && lv_label_get_long_mode(join) == LV_LABEL_LONG_DOT &&
                    setup_row_is_one_line(join),
                "a maximum-length SSID leaves the join row one line high");
    SETUP_CHECK(setup_rows_clear(join, security),
                "the join row stays clear of the security row");
    /* The height is what makes the dots happen at all, so an SSID that is
     * merely narrow enough to fit would pass the two checks above without
     * proving anything about the constraint under test. */
    SETUP_CHECK(strstr(long_text, "...") != NULL,
                "an SSID wider than the card truncates with an ellipsis");
    hide_setup_overlay(NULL);

    overlay = setup_selftest_build("slate-a1b2c3", "panel-setup-key", false);
    join = overlay != NULL ? setup_find_label(overlay, "1   Scan to join") : NULL;
    security = overlay != NULL ? setup_find_label(overlay, "Password   ") : NULL;

    SETUP_CHECK(join != NULL &&
                    strcmp(lv_label_get_text(join), "1   Scan to join slate-a1b2c3") == 0 &&
                    setup_row_is_one_line(join) && lv_obj_get_y(join) == long_y,
                "a short SSID renders in full and does not move");
    SETUP_CHECK(setup_rows_clear(join, security) && security != NULL &&
                    strcmp(lv_label_get_text(security), "Password   panel-setup-key") == 0,
                "the security row still carries the provisioned passphrase");
    hide_setup_overlay(NULL);

    /* The card gives the passphrase 46 px before the "2  Open http://" row —
     * two caption lines, which is what a 64-byte value needs at 408 px. */
    overlay = setup_selftest_build("slate-a1b2c3", SETUP_TEST_PASSPHRASE, false);
    security = overlay != NULL ? setup_find_label(overlay, "Password   ") : NULL;
    lv_obj_t *open_row = overlay != NULL ? setup_find_label(overlay, "2   Open http://") : NULL;

    SETUP_CHECK(security != NULL &&
                    strcmp(lv_label_get_text(security),
                           "Password   " SETUP_TEST_PASSPHRASE) == 0,
                "a maximum-length passphrase is printed in full");
    /* The card has the same two geometries the banner does — 408 px with the
     * QR, 680 px without — and at 680 this passphrase fits on one line, so the
     * clearance below would pass without having been asked anything. The
     * banner's payload is longer and its symbol smaller, so its QR succeeding
     * does imply this one's; that implication is not written anywhere the
     * compiler can see, which is the reason this is a check and not a note. */
    SETUP_CHECK(overlay != NULL && setup_find(overlay, &lv_qrcode_class, NULL) != NULL,
                "the card fixture encoded its QR, so the column is narrow");
    SETUP_CHECK(setup_rows_clear(security, open_row),
                "the card's passphrase row clears the address row");
    hide_setup_overlay(NULL);

    /* §9.4's banner keeps LV_LABEL_LONG_WRAP: it is a different presentation
     * and changing it is not this issue's subject. What it does not get to be
     * is unwatched. A maximum-length SSID fits its wider column with 3 px to
     * spare, and a type step or a moved y constant would silently spend that
     * and put the join row back on the passphrase — so the margin is asserted
     * rather than logged, and the measurement goes out beside it. */
    overlay = setup_selftest_build(SETUP_TEST_SSID, SETUP_TEST_PASSPHRASE, true);
    lv_obj_t *banner_join = overlay != NULL ? setup_find_label(overlay, "Scan to join") : NULL;
    lv_obj_t *banner_security = overlay != NULL ? setup_find_label(overlay, "Password: ") : NULL;
    lv_obj_t *banner_address =
        overlay != NULL ? setup_find_label(overlay, "Or open http://") : NULL;
    ESP_LOGI(TAG,
             "selftest: recovery banner join %" PRId32 " px in %" PRId32 ", "
             "passphrase %" PRId32 " px in %" PRId32,
             banner_join != NULL ? lv_obj_get_height(banner_join) : -1,
             banner_join != NULL && banner_security != NULL
                 ? lv_obj_get_y(banner_security) - lv_obj_get_y(banner_join)
                 : -1,
             banner_security != NULL ? lv_obj_get_height(banner_security) : -1,
             banner_security != NULL && banner_address != NULL
                 ? lv_obj_get_y(banner_address) - lv_obj_get_y(banner_security)
                 : -1);
    /* The passphrase goes into the QR payload too. Had it failed to encode,
     * setup_qr() would have returned NULL and the column would be 732 px
     * instead of 592 — the measurement above would then be true of a layout
     * no panel ever shows. */
    SETUP_CHECK(overlay != NULL && setup_find(overlay, &lv_qrcode_class, NULL) != NULL,
                "the banner fixture encoded its QR, so the column is narrow");
    SETUP_CHECK(setup_rows_clear(banner_join, banner_security),
                "the recovery banner join row clears its security row");
    /* 24 px here against the card's 46 — the banner is the tighter of the two
     * presentations and takes the same 64-byte value. */
    SETUP_CHECK(setup_rows_clear(banner_security, banner_address),
                "the banner's passphrase row clears the address row");
    hide_setup_overlay(NULL);

    /* The fixtures leave the backlight on, which is what §9.4 asks of a real
     * card anyway. What must not survive is the presentation itself: while it
     * is active §3.3's dimming is refused. */
    SETUP_CHECK(!slate_display_setup_active(),
                "the fixtures leave no setup presentation behind");

    ESP_LOGI(TAG, "selftest: %u check(s), %u failure(s)", checks, failures);
#undef SETUP_CHECK
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

static void setup_selftest_work(void *ctx)
{
    setup_selftest_request_t *request = ctx;
    request->result = setup_selftest_on_task();
    xSemaphoreGive(request->done);
}

esp_err_t slate_display_selftest(void)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "setup selftest skipped: the panel did not initialise");
        return ESP_ERR_INVALID_STATE;
    }
    /* The two preconditions are checked in different places on purpose.
     * s_ready is written once during initialisation and never again, so
     * reading it here is as good as reading it there. Whether a setup card is
     * on screen is not: §9.4 raises it asynchronously, so that one is only
     * true where it is acted on, and the work is queued even when it will
     * immediately decline. */
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    setup_selftest_request_t request = {.done = done, .result = ESP_ERR_INVALID_STATE};
    esp_err_t err = slate_display_post(setup_selftest_work, &request, 1000);
    if (err == ESP_OK) {
        xSemaphoreTake(done, portMAX_DELAY);
        err = request.result;
    }
    vSemaphoreDelete(done);
    return err;
}

#endif /* SLATE_DISPLAY_SELFTEST */
