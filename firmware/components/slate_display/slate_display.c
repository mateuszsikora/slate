/*
 * Slate — Waveshare ESP32-S3-Touch-LCD-7 display bring-up.
 *
 * The pin map, timings and render configuration below were not copied forward
 * from an example. S-2 cross-checked them between two independent sources,
 * booted them on this exact N16R8 board and measured 26,441 us between VSYNCs.
 * Its recommended shape is implemented literally: two RGB565 framebuffers in
 * PSRAM, direct LVGL rendering, a ten-line internal bounce buffer and a flush
 * gate that permits exactly one rendered frame per panel scan.
 */

#include <stdbool.h>
#include <stdint.h>
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

#define SLATE_EXIO_TP_RST  IO_EXPANDER_PIN_NUM_1
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
static SemaphoreHandle_t s_vsync;
static TaskHandle_t s_task;
static esp_lcd_panel_handle_t s_panel;
static esp_io_expander_handle_t s_expander;
static esp_err_t s_init_result = ESP_ERR_INVALID_STATE;
static bool s_ready;
static bool s_backlight_on;

/* Written in the VSYNC ISR, read on the LVGL task. The values are 64-bit on a
 * 32-bit CPU, so the read is protected rather than assumed atomic. */
static portMUX_TYPE s_vsync_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t s_vsync_last_us;
static volatile int64_t s_vsync_period_us;
static volatile uint32_t s_vsync_count;

void *slate_display_lvgl_pool_alloc(size_t size)
{
    void *pool = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "LVGL pool %u B at %p in PSRAM", (unsigned) size, pool);
    return pool;
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
    return i2c_driver_install(SLATE_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

static esp_err_t expander_init(void)
{
    ESP_RETURN_ON_ERROR(
        esp_io_expander_new_i2c_ch422g(SLATE_I2C_PORT,
                                       ESP_IO_EXPANDER_I2C_CH422G_ADDRESS,
                                       &s_expander),
        TAG, "CH422G");

    const uint32_t outputs = SLATE_EXIO_TP_RST | SLATE_EXIO_DISP | SLATE_EXIO_LCD_RST;
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander, outputs, IO_EXPANDER_OUTPUT),
                        TAG, "CH422G direction");

    /* Keep the glass dark until a complete frame exists. */
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, SLATE_EXIO_DISP, 0),
                        TAG, "backlight off");

    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, SLATE_EXIO_LCD_RST, 0),
                        TAG, "panel reset low");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, SLATE_EXIO_TP_RST, 0),
                        TAG, "touch reset low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, SLATE_EXIO_LCD_RST, 1),
                        TAG, "panel reset high");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, SLATE_EXIO_TP_RST, 1),
                        TAG, "touch reset high");
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

    BaseType_t higher_priority_task_woken = pdFALSE;
    if (s_vsync) {
        xSemaphoreGiveFromISR(s_vsync, &higher_priority_task_woken);
    }
    return higher_priority_task_woken == pdTRUE;
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

static void backlight_enable(void)
{
    if (s_backlight_on) {
        return;
    }
    esp_err_t err = esp_io_expander_set_level(s_expander, SLATE_EXIO_DISP, 1);
    if (err == ESP_OK) {
        s_backlight_on = true;
        ESP_LOGI(TAG, "first frame displayed; backlight on");
    } else {
        ESP_LOGE(TAG, "backlight on: %s", esp_err_to_name(err));
    }
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void) area;

    if (lv_display_flush_is_last(display)) {
        /* A binary semaphore can hold an old VSYNC while LVGL is idle. Drain it
         * before requesting the buffer switch, otherwise the wait could return
         * for the boundary before the switch rather than the one after it. */
        xSemaphoreTake(s_vsync, 0);
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                                  SLATE_LCD_H_RES, SLATE_LCD_V_RES,
                                                  pixels);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "flush: %s", esp_err_to_name(err));
        } else if (xSemaphoreTake(s_vsync,
                                  pdMS_TO_TICKS(SLATE_DISPLAY_VSYNC_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "VSYNC timeout after framebuffer switch");
        } else {
            backlight_enable();
        }
    }
    lv_display_flush_ready(display);
}

static uint32_t tick_ms(void)
{
    return (uint32_t) (esp_timer_get_time() / 1000);
}

static esp_err_t lvgl_display_init(void)
{
    lv_init();

    lv_display_t *display = lv_display_create(SLATE_LCD_H_RES, SLATE_LCD_V_RES);
    if (!display) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flush_cb);

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
    return rect;
}

static void moving_marker_x(void *object, int32_t x)
{
    lv_obj_set_x((lv_obj_t *) object, x);
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
     * frame skipping/flicker that made S-2 require the VSYNC gate. */
    lv_obj_t *marker = solid_rect(screen, 20, 458, 40, 12, 0x6C8CFF);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, marker);
    lv_anim_set_exec_cb(&animation, moving_marker_x);
    lv_anim_set_values(&animation, 20, 740);
    lv_anim_set_duration(&animation, 2500);
    lv_anim_set_reverse_duration(&animation, 2500);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_start(&animation);

    ESP_LOGI(TAG, "bring-up test pattern built on LVGL task");
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
        ESP_LOGI(TAG, "display ready: %u B internal DMA-capable, %u B PSRAM free",
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    xSemaphoreGive(s_init_done);

    if (!s_ready) {
        s_task = NULL;
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
        log_vsync_once();
    }
}

esp_err_t slate_display_init(void)
{
    if (s_task || s_work_queue || s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    s_work_queue = xQueueCreate(SLATE_DISPLAY_QUEUE_LEN, sizeof(slate_display_work_t));
    s_init_done = xSemaphoreCreateBinary();
    s_vsync = xSemaphoreCreateBinary();
    if (!s_work_queue || !s_init_done || !s_vsync) {
        if (s_work_queue) {
            vQueueDelete(s_work_queue);
            s_work_queue = NULL;
        }
        if (s_init_done) {
            vSemaphoreDelete(s_init_done);
            s_init_done = NULL;
        }
        if (s_vsync) {
            vSemaphoreDelete(s_vsync);
            s_vsync = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(display_task, "slate_lvgl", SLATE_DISPLAY_TASK_STACK,
                                NULL, SLATE_DISPLAY_TASK_PRIORITY, &s_task,
                                SLATE_DISPLAY_TASK_CORE) != pdPASS) {
        s_task = NULL;
        vQueueDelete(s_work_queue);
        vSemaphoreDelete(s_init_done);
        vSemaphoreDelete(s_vsync);
        s_work_queue = NULL;
        s_init_done = NULL;
        s_vsync = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_init_done, pdMS_TO_TICKS(SLATE_DISPLAY_INIT_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_init_result != ESP_OK) {
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

bool slate_display_ready(void)
{
    return s_ready;
}
