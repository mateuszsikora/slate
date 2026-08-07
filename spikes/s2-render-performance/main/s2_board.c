/*
 * Spike S-2 — panel bring-up and render instrumentation.
 *
 * Nothing had ever been drawn on this panel before this spike: S-1 ran on a
 * dummy display and S-3 linked the RGB driver with placeholder pins and never
 * executed it. So the first job here is to make the panel work, and the second
 * is to measure it. The pin map below is the part worth keeping — issue #6 owns
 * display bring-up properly, and it should inherit verified numbers.
 *
 * Sources for the pin map, cross-checked against each other rather than copied
 * from one place:
 *
 *   - Waveshare wiki, "ESP32-S3-Touch-LCD-7", LCD interface pinout table.
 *   - inytar/waveshare-esp32-s3-touch-lcd-7-esphome, an independent ESPHome
 *     package for this exact board.
 *
 * They agree pin for pin. Note that waveshareteam/ESP32-S3-Touch-LCD-7B is a
 * *different* board and was deliberately not used as a source; design.md §6.3
 * is a standing reminder that this board's documentation has been wrong before.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
/* Not at the top of the include path: S-3 found CH422G inside
 * espressif/esp32_io_expander rather than in a component of its own. See #7. */
#include "port/esp_io_expander_ch422g.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "s2_board.h"

static const char *TAG = "s2.board";

/* -------------------------------------------------------------------------
 * Pin map
 * ------------------------------------------------------------------------- */

/* Touch bus, shared with the IO expander (§6.1). GT911 itself is not used by
 * this spike — S-2 measures rendering, and touch is #7 — but the expander on
 * this bus holds panel reset and the backlight, so the bus is not optional. */
#define S2_I2C_PORT    I2C_NUM_0
#define S2_I2C_SDA_GPIO 8
#define S2_I2C_SCL_GPIO 9
#define S2_I2C_HZ      400000

/*
 * CH422G outputs. EXIO2 is the backlight enable ("DISP" in the wiki's table),
 * EXIO3 resets the panel, EXIO1 resets the touch controller. The backlight is
 * a binary output here — design.md §16 records that smooth dimming needs a pin
 * bridged to a GPIO, which is #41's question, not this spike's.
 */
#define S2_EXIO_TP_RST  IO_EXPANDER_PIN_NUM_1
#define S2_EXIO_DISP    IO_EXPANDER_PIN_NUM_2
#define S2_EXIO_LCD_RST IO_EXPANDER_PIN_NUM_3

#define S2_PIN_HSYNC 46
#define S2_PIN_VSYNC 3
#define S2_PIN_DE    5
#define S2_PIN_PCLK  7

/*
 * RGB565 data lines, in the order esp_lcd expects: the low five are blue, the
 * middle six green, the top five red. The panel is wired to the upper bits of
 * each channel (B3..B7, G2..G7, R3..R7), which is what makes 16 lines carry
 * 5-6-5 — the missing low bits are simply not connected.
 */
static const int S2_DATA_GPIOS[16] = {
    14, /* B3 */ 38, /* B4 */ 18, /* B5 */ 17, /* B6 */ 10, /* B7 */
    39, /* G2 */  0, /* G3 */ 45, /* G4 */ 48, /* G5 */ 47, /* G6 */ 21, /* G7 */
     1, /* R3 */  2, /* R4 */ 42, /* R5 */ 41, /* R6 */ 40, /* R7 */
};

/* -------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

static esp_lcd_panel_handle_t s_panel;
#if S2_VSYNC_GATE
/* Given by the VSYNC ISR, taken by the flush: one rendered frame per scan-out. */
static SemaphoreHandle_t s_vsync_sem;
#endif
static esp_io_expander_handle_t s_expander;

/* VSYNC bookkeeping, written from the ISR and read from the LVGL task. Both are
 * 64-bit, so they are read under a critical section rather than assumed atomic
 * on a 32-bit core. */
static portMUX_TYPE s_vsync_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t  s_vsync_last_us;
static volatile int64_t  s_vsync_period_us;
static volatile uint32_t s_vsync_count;

/* Frame capture */
static s2_frame_t *s_frames;
static uint32_t    s_frames_cap;
static uint32_t    s_frames_len;
static s2_frame_t  s_current;
static bool        s_frame_open;

/* -------------------------------------------------------------------------
 * Bring-up
 * ------------------------------------------------------------------------- */

static esp_err_t s2_i2c_init(void)
{
    /*
     * The legacy driver, deliberately. S-3 found there is no standalone CH422G
     * component and the one inside espressif/esp32_io_expander takes an
     * i2c_port_t rather than an i2c_master_bus_handle_t. ESP-IDF aborts at boot
     * if both stacks touch the same bus, so the whole bus is legacy here. The
     * choice between keeping this and writing a CH422G driver against the new
     * API belongs to #7; a spike should not settle driver architecture.
     */
    const i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = S2_I2C_SDA_GPIO,
        .scl_io_num = S2_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = S2_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(S2_I2C_PORT, &conf), TAG, "i2c config");
    return i2c_driver_install(S2_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

static esp_err_t s2_expander_init(void)
{
    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_ch422g(S2_I2C_PORT,
                                                       ESP_IO_EXPANDER_I2C_CH422G_ADDRESS,
                                                       &s_expander),
                        TAG, "ch422g");

    const uint32_t outputs = S2_EXIO_TP_RST | S2_EXIO_DISP | S2_EXIO_LCD_RST;
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander, outputs, IO_EXPANDER_OUTPUT),
                        TAG, "dir");

    /* Backlight stays off until the first frame is on the panel: powering it up
     * over an uninitialised framebuffer shows whatever PSRAM happened to hold. */
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, S2_EXIO_DISP, 0), TAG, "disp");

    /* Panel and touch out of reset. The touch controller samples its address
     * pins on the reset edge, so it is released even though this spike never
     * talks to it — leaving it held is a difference from the real firmware for
     * no benefit. */
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, S2_EXIO_LCD_RST, 0), TAG, "rst lo");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, S2_EXIO_TP_RST, 0), TAG, "tp lo");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, S2_EXIO_LCD_RST, 1), TAG, "rst hi");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, S2_EXIO_TP_RST, 1), TAG, "tp hi");
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static bool IRAM_ATTR s2_on_vsync(esp_lcd_panel_handle_t panel,
                                  const esp_lcd_rgb_panel_event_data_t *data,
                                  void *user_ctx)
{
    (void)panel;
    (void)data;
    (void)user_ctx;

    const int64_t now = esp_timer_get_time();
    const int64_t delta = now - s_vsync_last_us;

    /* An exponential average rather than the last delta: the tear test divides
     * by this, and a single late ISR would otherwise shift the computed scan
     * position for a whole frame. Deltas outside a plausible range are dropped
     * (the first one, and any the ISR was late for). */
    if (delta > 1000 && delta < 200000) {
        s_vsync_period_us = (s_vsync_period_us == 0)
                                ? delta
                                : (s_vsync_period_us * 7 + delta) / 8;
    }
    s_vsync_last_us = now;
    s_vsync_count++;
#if S2_VSYNC_GATE
    BaseType_t woken = pdFALSE;
    if (s_vsync_sem) {
        xSemaphoreGiveFromISR(s_vsync_sem, &woken);
    }
    return woken == pdTRUE;
#else
    return false;
#endif
}

esp_err_t s2_board_init(void)
{
    ESP_RETURN_ON_ERROR(s2_i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(s2_expander_init(), TAG, "expander");

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = S2_PCLK_HZ,
            .h_res = S2_LCD_H_RES,
            .v_res = S2_LCD_V_RES,
            .hsync_pulse_width = S2_HSYNC_PULSE_WIDTH,
            .hsync_back_porch  = S2_HSYNC_BACK_PORCH,
            .hsync_front_porch = S2_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = S2_VSYNC_PULSE_WIDTH,
            .vsync_back_porch  = S2_VSYNC_BACK_PORCH,
            .vsync_front_porch = S2_VSYNC_FRONT_PORCH,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = S2_NUM_FBS,
        /* design.md §6.2 calls the bounce buffer required: without it the DMA
         * reads the framebuffer straight out of PSRAM and loses the race
         * whenever something else wants the bus. S2_BOUNCE=0 is the negative
         * control that shows the instrument can see the artifact. */
        .bounce_buffer_size_px = S2_BOUNCE ? S2_BOUNCE_PX : 0,
        .dma_burst_size = 64,
        .hsync_gpio_num = S2_PIN_HSYNC,
        .vsync_gpio_num = S2_PIN_VSYNC,
        .de_gpio_num    = S2_PIN_DE,
        .pclk_gpio_num  = S2_PIN_PCLK,
        .disp_gpio_num  = -1, /* on the CH422G, not on a GPIO */
        .flags.fb_in_psram = true,
    };
    memcpy(cfg.data_gpio_nums, S2_DATA_GPIOS, sizeof(S2_DATA_GPIOS));

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &s_panel), TAG, "rgb panel");

    const esp_lcd_rgb_panel_event_callbacks_t cbs = { .on_vsync = s2_on_vsync };
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL),
                        TAG, "vsync cb");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");

    ESP_LOGI(TAG, "RGB panel up: %dx%d, %d MHz pclk, %d fb(s), bounce %d px",
             S2_LCD_H_RES, S2_LCD_V_RES, S2_PCLK_HZ / 1000000,
             (int)S2_NUM_FBS, S2_BOUNCE ? S2_BOUNCE_PX : 0);
    return ESP_OK;
}

void s2_backlight(bool on)
{
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_expander, S2_EXIO_DISP, on ? 1 : 0));
}

/* -------------------------------------------------------------------------
 * Tear detection
 *
 * With a single framebuffer LVGL renders into the same memory the DMA is
 * scanning out, so a flush that lands on rows the scan-out is currently passing
 * splits the displayed frame — one part old, one part new. That is a tear, and
 * it is what the second 750 KB framebuffer in §6.2 would buy.
 *
 * It is measurable without a camera. The VSYNC ISR gives the start of each
 * frame and its period; the scan-out advances at a constant rate through
 * S2_V_TOTAL lines. So for a flush covering rows [y1,y2] between t0 and t1, the
 * lines the scan crossed in that window are known, and the flush tore if that
 * set intersects the rows it wrote.
 *
 * Two honest caveats, both recorded in s2.md:
 *
 *   - With a bounce buffer the DMA reads ahead of the visible line by the
 *     bounce height (10 lines here), so the true race window is shifted a
 *     fraction of a millisecond earlier. Ten lines out of 516 is under 2 % of a
 *     frame; it cannot turn a torn frame into a clean one or the reverse at any
 *     rate this spike measures.
 *   - This counts *opportunities* visible to the instrument, not tears seen by
 *     an eye. A tear on two rows of a dark background is real and invisible.
 *     That is why the maintainer looks at the panel as well.
 * ------------------------------------------------------------------------- */

static uint32_t s2_scan_line_at(int64_t t, int64_t vsync_last, int64_t period)
{
    if (period <= 0) {
        return 0;
    }
    int64_t into = t - vsync_last;
    if (into < 0) {
        into = 0;
    }
    into %= period;
    return (uint32_t)((into * S2_V_TOTAL) / period);
}

static bool s2_flush_tore(int64_t t0, int64_t t1, int32_t y1, int32_t y2)
{
    int64_t vsync_last, period;
    portENTER_CRITICAL(&s_vsync_mux);
    vsync_last = s_vsync_last_us;
    period = s_vsync_period_us;
    portEXIT_CRITICAL(&s_vsync_mux);

    if (period <= 0) {
        return false; /* no VSYNC seen yet; nothing is being scanned out */
    }
    if ((t1 - t0) >= period) {
        return true; /* the write outlasted a whole frame — it cannot not tear */
    }

    const uint32_t l0 = s2_scan_line_at(t0, vsync_last, period);
    const uint32_t l1 = s2_scan_line_at(t1, vsync_last, period);

    if (l0 <= l1) {
        return !((int32_t)l1 < y1 || (int32_t)l0 > y2);
    }
    /* The window straddled a VSYNC: the scan swept [l0, V_TOTAL) and [0, l1]. */
    return ((int32_t)l0 <= y2) || ((int32_t)l1 >= y1);
}

bool s2_tearing_possible(void)
{
    return S2_NUM_FBS < 2;
}

/* -------------------------------------------------------------------------
 * LVGL display port
 * ------------------------------------------------------------------------- */

static void s2_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int64_t t0 = esp_timer_get_time();

#if S2_NUM_FBS >= 2
    /*
     * Direct render mode: LVGL draws into whichever framebuffer is currently the
     * back one, and only the last flush of a frame hands it to the driver, which
     * switches buffers at the next VSYNC. Nothing races the scan-out — the cost
     * is the second 750 KB in PSRAM.
     *
     * Keeping the two buffers in step is LVGL's own job and it does it:
     * refr_sync_areas() in lv_refr.c copies the previous frame's invalidated
     * areas into the buffer about to be rendered, for exactly this render mode.
     * Doing it again by hand here was tried and is pure cost — it moved
     * render+flush p50 from 5.6 ms to 16.1 ms — so it is not done.
     *
     * What does have to happen here is waiting for VSYNC, and leaving it out is
     * what produced a flicker on the moving parts of the screen. The reason is
     * in the driver: with a bounce buffer configured, a framebuffer switch does
     * not take effect when draw_bitmap is called. cur_fb_index is set at once,
     * but the bounce filler keeps reading bb_fb_index until the scan wraps at a
     * frame boundary (esp_lcd_panel_rgb.c):
     *
     *     if (panel->bounce_pos_px >= panel->fb_size / bytes_per_pixel) {
     *         panel->bounce_pos_px = 0;
     *         panel->bb_fb_index = panel->cur_fb_index;   // the switch lands here
     *
     * So the panel adopts whichever buffer is current at a frame boundary.
     * Rendering faster than the panel scans — 67 fps of LVGL against 37.8 Hz of
     * panel — submits about two buffers per displayed frame, and the skipped one
     * is a frame of motion that never reaches the glass. Moving content then
     * alternates between two versions of itself, which is the flicker. It is
     * absent at LVGL's default 33 ms refresh for the least satisfying of
     * reasons: 30.3 Hz is slower than the panel, so nothing is ever skipped.
     *
     * Waiting for the VSYNC after the switch makes LVGL produce exactly one
     * frame per scan-out. The wait is timed separately and reported in its own
     * column: it is idle time, and folding it into a frame-time percentile would
     * report waiting for the panel as if it were the cost of drawing.
     */
    if (lv_display_flush_is_last(disp)) {
#if S2_VSYNC_GATE
        const int64_t w0 = esp_timer_get_time();
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, S2_LCD_H_RES, S2_LCD_V_RES, px_map);
        xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(100));
        s_current.wait_us += (uint32_t)(esp_timer_get_time() - w0);
#else
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, S2_LCD_H_RES, S2_LCD_V_RES, px_map);
#endif
    }
#else
    /* Partial render mode: copy the rendered area into the live framebuffer. */
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
#endif

    const int64_t t1 = esp_timer_get_time();

    if (s_frame_open) {
        s_current.flush_us += (uint32_t)(t1 - t0);
        s_current.flush_cnt++;
        s_current.px += (uint32_t)((area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1));
        if (s2_tearing_possible() && s2_flush_tore(t0, t1, area->y1, area->y2)) {
            s_current.tear_cnt++;
        }
    }

    lv_display_flush_ready(disp);
}

static void s2_render_start_cb(lv_event_t *e)
{
    (void)e;
    memset(&s_current, 0, sizeof(s_current));
    s_current.start_us = esp_timer_get_time();
    s_frame_open = true;
}

static void s2_render_ready_cb(lv_event_t *e)
{
    (void)e;
    if (!s_frame_open) {
        return;
    }
    s_frame_open = false;
    s_current.render_us = (uint32_t)(esp_timer_get_time() - s_current.start_us);

    /* Dropped rather than overwritten: a run that outlives the buffer should
     * lose its tail, not silently recycle it into a shorter, prettier one. */
    if (s_frames && s_frames_len < s_frames_cap) {
        s_frames[s_frames_len++] = s_current;
    }
}

static uint32_t s2_tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

lv_display_t *s2_display_init(s2_frame_t *sink, uint32_t capacity)
{
    s_frames = sink;
    s_frames_cap = capacity;
    s_frames_len = 0;

#if S2_VSYNC_GATE
    s_vsync_sem = xSemaphoreCreateBinary();
    assert(s_vsync_sem != NULL);
#endif

    lv_display_t *disp = lv_display_create(S2_LCD_H_RES, S2_LCD_V_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, s2_flush_cb);

#if S2_NUM_FBS >= 2
    void *fb0 = NULL, *fb1 = NULL;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1));
    lv_display_set_buffers(disp, fb0, fb1, S2_LCD_H_RES * S2_LCD_V_RES * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    ESP_LOGI(TAG, "double framebuffer, direct render mode");
#else
    /*
     * design.md §6.2: draw buffer ~1/10 screen, internal SRAM. Sized in *bytes*
     * from the colour format rather than as an array of lv_color_t — in LVGL 9
     * that type is a 24-bit struct whatever the render depth, and sizing with it
     * costs 50 % more DIRAM than it uses. S-3 measured that mistake at 38 400 B
     * and it is recorded on #6; it is not repeated here.
     */
    const size_t buf_bytes = S2_LCD_H_RES * S2_DRAW_LINES * 2;
    void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    /*
     * Not an assert. Whether these two fit is one of this spike's answers, and a
     * failed allocation must be reported rather than turned into a boot loop:
     * with the radio on, two 1/10-screen buffers do not fit, which is exactly
     * the kind of thing #6 needs told rather than discovered.
     */
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "draw buffers (2 × %u B) do not fit: %u B internal free",
                 (unsigned)buf_bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        printf("S2FAIL,draw_buf_bytes,%u,int_dma_free,%u\n", (unsigned)buf_bytes,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        abort();
    }
    lv_display_set_buffers(disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    ESP_LOGI(TAG, "single framebuffer, partial render mode, 2 × %u B draw buffers (%d lines)",
             (unsigned)buf_bytes, (int)S2_DRAW_LINES);
#endif

    lv_display_add_event_cb(disp, s2_render_start_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, s2_render_ready_cb, LV_EVENT_RENDER_READY, NULL);
    lv_tick_set_cb(s2_tick_cb);

    return disp;
}

uint32_t s2_frames_captured(void) { return s_frames_len; }
void     s2_frames_reset(void)    { s_frames_len = 0; }

uint32_t s2_vsync_count(void)
{
    portENTER_CRITICAL(&s_vsync_mux);
    const uint32_t n = s_vsync_count;
    portEXIT_CRITICAL(&s_vsync_mux);
    return n;
}

int64_t s2_vsync_period_us(void)
{
    portENTER_CRITICAL(&s_vsync_mux);
    const int64_t p = s_vsync_period_us;
    portEXIT_CRITICAL(&s_vsync_mux);
    return p;
}
