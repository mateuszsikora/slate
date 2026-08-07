/*
 * Slate — render performance spike (S-2).
 *
 * design.md §13 states the experiment: 20 tiles including 4 animated, 10 entity
 * updates per second, measure frame time and page-switch smoothness. It
 * determines two things that cannot be walked back later — the maximum tile
 * count per page (§3.2 configurations depend on it) and whether one 750 KB
 * framebuffer suffices or tearing demands a second (§6.2).
 *
 * Two departures from the brief, both deliberate and both argued in the issue
 * and in docs/spikes/s2.md:
 *
 *   1. The verdict is measured at 12 tiles, not 20. §3.2's grid is 4 × 3 and the
 *      smallest tile is 1×1, so no configuration can express 20 tiles on a page.
 *      20 is measured as an overload on a relaxed 5 × 4 grid — useful for
 *      knowing whether a wider grid is on the table, useless as a "maximum".
 *   2. Frame samples are collected into PSRAM and printed after the run. A
 *      printf inside a frame-time measurement measures the UART.
 *
 * What this spike cannot do by itself is decide the pass criterion: §13 says
 * "no visible stutter", and stutter is a thing an eye judges. The numbers here
 * are the quantitative half; the maintainer watches the panel for the other.
 * The run therefore ends in a visual phase that repeats the same load forever,
 * so it can be watched and filmed after the numbers are in.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lvgl.h"

#include "s2_board.h"
#include "s2_lvgl_pool.h"
#include "s2_scene.h"

static const char *TAG = "s2";

/* -------------------------------------------------------------------------
 * Experiment parameters
 * ------------------------------------------------------------------------- */

#define S2_WARMUP_MS      3000  /* discarded: first render fills the glyph cache */
#define S2_DWELL_MS       3000  /* time on a page between switches */
#define S2_ROUNDS           16  /* page switches measured */
#define S2_UPDATE_PERIOD_MS 100 /* §13: 10 entity updates per second */

/* 16 rounds × 3 s at the panel's ~38 Hz, plus warmup and slack. */
#define S2_FRAME_CAPACITY 4096

/*
 * Radio load. design.md §6.2 says the bounce buffer is required because WiFi
 * activity otherwise causes visible artifacts, so a measurement with the radio
 * idle measures the easy case and proves nothing about the buffer.
 *
 * This runs without joining any network, because the artifact does not come
 * from being associated. It comes from the radio contending for the same memory
 * bus the panel's DMA is streaming from, and from the interrupt load of the
 * WiFi task — both of which are present the moment the PHY is on. So the stress
 * is built from three things that need no credentials:
 *
 *   - AP mode, beaconing every 100 ms (transmit path, PHY on continuously);
 *   - STA scanning in a loop (channel switching, calibration, receive path);
 *   - promiscuous receive, which counts every frame in the air on the current
 *     channel. This is what makes the load real rather than synthetic — the
 *     house's own traffic becomes the receive stream.
 *
 * On the promiscuous callback: it reads `sig_len` and nothing else. No payload
 * is inspected, copied, stored or transmitted, and nothing is written to flash.
 * The counter exists solely so the report can state how busy the radio actually
 * was, rather than asserting it.
 *
 * What this does NOT reproduce, stated in s2.md rather than glossed over: a
 * sustained high-throughput transfer, such as the §11.1 development OTA pushing
 * a 1.5 MB image over the same radio. That is a heavier bus load than beacons
 * and scan responses, and it remains unmeasured.
 */
#ifndef S2_RADIO
#define S2_RADIO 0
#endif

/*
 * Whether the visual phase after the measurement keeps switching pages. Set to
 * 0 to isolate a reported flicker from the page rebuild — see the visual phase
 * at the bottom of this file.
 */
#ifndef S2_VISUAL_SWITCH
#define S2_VISUAL_SWITCH 1
#endif

typedef struct {
    uint32_t round;
    uint32_t destroy_us;
    uint32_t build_us;
    uint32_t first_frame_us; /* switch complete → first frame fully flushed */
    uint32_t frame_index;    /* index into the frame log where the switch landed */
    uint32_t tiles;
} s2_switch_t;

static s2_frame_t  *s_frames;
static s2_switch_t  s_switches[S2_ROUNDS];
static uint32_t     s_update_seq;

/* -------------------------------------------------------------------------
 * LVGL heap in PSRAM (design.md §6.2)
 * ------------------------------------------------------------------------- */

void *s2_lvgl_pool_alloc(size_t size)
{
    void *pool = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    printf("s2: LVGL pool %u bytes at %p (PSRAM)\n", (unsigned)size, pool);
    return pool;
}

/* -------------------------------------------------------------------------
 * WiFi
 * ------------------------------------------------------------------------- */

#if S2_RADIO
static volatile uint32_t s_rx_frames;
static volatile uint32_t s_rx_bytes;
static volatile uint32_t s_scans;

static void s2_promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    s_rx_frames++;
    s_rx_bytes += pkt->rx_ctrl.sig_len; /* length only — see the header comment */
}

/* Scanning is what keeps the receive path and the channel switching busy. It
 * runs on its own task so the LVGL task is never blocked by it — the whole
 * point is contention underneath LVGL, not a stall inside it. */
static void s2_scan_task(void *arg)
{
    (void)arg;
    const wifi_scan_config_t scan = {
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };
    while (true) {
        if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
            uint16_t found = 0;
            esp_wifi_scan_get_ap_num(&found);
            esp_wifi_clear_ap_list();
            s_scans++;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void s2_radio_start(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Open, one client, and nothing listening behind it. It exists to make the
     * transmit path run, not to be connected to. */
    wifi_config_t ap = {
        .ap = {
            .ssid            = "slate-s2-probe",
            .ssid_len        = 14,
            .channel         = 1,
            .max_connection  = 1,
            .authmode        = WIFI_AUTH_OPEN,
            .beacon_interval = 100,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    const wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(s2_promiscuous_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    xTaskCreatePinnedToCore(s2_scan_task, "s2_scan", 4096, NULL, 3, NULL, 0);

    ESP_LOGI(TAG, "radio active: AP beacons + active scan + promiscuous RX, no association");
}
#endif

/* -------------------------------------------------------------------------
 * The load
 * ------------------------------------------------------------------------- */

static void s2_update_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s2_scene_update(s_update_seq++);
}

/* Runs LVGL for `ms`, which is what actually renders. Kept as tight as the real
 * runtime's loop: lv_timer_handler() reports when it next wants to run, and a
 * fixed 5 ms sleep would quantise every frame time to a multiple of the tick. */
static void s2_run(uint32_t ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < deadline) {
        uint32_t next = lv_timer_handler();
        if (next > 10) {
            next = 10;
        }
        vTaskDelay(pdMS_TO_TICKS(next ? next : 1));
    }
}

/* -------------------------------------------------------------------------
 * Reporting
 * ------------------------------------------------------------------------- */

static void s2_dump(void)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);

    printf("S2CFG,num_fbs,%d,bounce_px,%d,tiles_cfg,%d,tiles_built,%u,"
           "grid,%dx%d,draw_lines,%d,pclk_hz,%d,v_total,%d,refresh_hz_calc,%.2f,"
           "vsync_period_us,%lld,vsync_count,%u,"
           "lvgl_free,%u,lvgl_max_used,%u,int_free,%u,psram_free,%u\n",
           (int)S2_NUM_FBS, S2_BOUNCE ? S2_BOUNCE_PX : 0, (int)S2_TILES,
           (unsigned)s2_scene_tile_count(), S2_GRID_COLS, S2_GRID_ROWS,
           (S2_NUM_FBS >= 2) ? 0 : (int)S2_DRAW_LINES,
           (int)S2_PCLK_HZ, S2_V_TOTAL,
           (double)S2_PCLK_HZ /
               ((double)(S2_LCD_H_RES + S2_HSYNC_PULSE_WIDTH + S2_HSYNC_BACK_PORCH +
                         S2_HSYNC_FRONT_PORCH) * S2_V_TOTAL),
           (long long)s2_vsync_period_us(), (unsigned)s2_vsync_count(),
           (unsigned)mon.free_size, (unsigned)mon.max_used,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Evidence that the radio was busy, rather than an assertion that it was.
     * A run whose frame counter stayed near zero would mean the channel was
     * quiet and the control measured nothing. */
#if S2_RADIO
    printf("S2RADIO,frames,%u,bytes,%u,scans,%u\n",
           (unsigned)s_rx_frames, (unsigned)s_rx_bytes, (unsigned)s_scans);
#else
    printf("S2RADIO,frames,0,bytes,0,scans,0\n");
#endif

    printf("S2FH,idx,start_us,render_us,flush_us,flush_cnt,tear_cnt,px,wait_us\n");
    const uint32_t n = s2_frames_captured();
    for (uint32_t i = 0; i < n; i++) {
        const s2_frame_t *f = &s_frames[i];
        printf("S2F,%u,%lld,%u,%u,%u,%u,%u,%u\n",
               (unsigned)i, (long long)f->start_us, (unsigned)f->render_us,
               (unsigned)f->flush_us, (unsigned)f->flush_cnt, (unsigned)f->tear_cnt,
               (unsigned)f->px, (unsigned)f->wait_us);
    }

    printf("S2SWH,round,destroy_us,build_us,first_frame_us,frame_index,tiles\n");
    for (uint32_t i = 0; i < S2_ROUNDS; i++) {
        const s2_switch_t *s = &s_switches[i];
        printf("S2SW,%u,%u,%u,%u,%u,%u\n",
               (unsigned)s->round, (unsigned)s->destroy_us, (unsigned)s->build_us,
               (unsigned)s->first_frame_us, (unsigned)s->frame_index,
               (unsigned)s->tiles);
    }
}

/* -------------------------------------------------------------------------
 * Experiment
 * ------------------------------------------------------------------------- */

static void s2_task(void *arg)
{
    (void)arg;

    lv_init();
    s_frames = heap_caps_calloc(S2_FRAME_CAPACITY, sizeof(s2_frame_t), MALLOC_CAP_SPIRAM);
    assert(s_frames);
    lv_display_t *disp = s2_display_init(s_frames, S2_FRAME_CAPACITY);
    (void)disp;

    lv_obj_t *page = s2_scene_build(0);
    lv_timer_create(s2_update_timer_cb, S2_UPDATE_PERIOD_MS, NULL);

    /* Warmup: the first render of each glyph populates LVGL's cache and the
     * first frame after boot is not representative of anything. Charging that
     * to the measurement would report a stutter that happens once, ever. */
    s2_run(500);
    s2_backlight(true);
    s2_run(S2_WARMUP_MS);
    s2_frames_reset();

    ESP_LOGI(TAG, "measuring: %d rounds × %d ms, %u tiles, %d fb(s), bounce %s",
             S2_ROUNDS, S2_DWELL_MS, (unsigned)s2_scene_tile_count(),
             (int)S2_NUM_FBS, S2_BOUNCE ? "on" : "OFF");

    for (uint32_t round = 0; round < S2_ROUNDS; round++) {
        s2_run(S2_DWELL_MS);

        /*
         * The page switch — §6.4's destroy-then-build, which is what PUT /config
         * does and what the pass criterion is about. Timed in three parts,
         * because they fail differently: a slow destroy is a teardown problem, a
         * slow build is a parser or layout problem, and a slow first frame is a
         * render problem.
         */
        const uint32_t frame_index = s2_frames_captured();

        const int64_t t0 = esp_timer_get_time();
        s2_scene_destroy(page);
        const int64_t t1 = esp_timer_get_time();
        page = s2_scene_build((round + 1) % S2_PAGE_COUNT);
        const int64_t t2 = esp_timer_get_time();

        /* Run until the new page has actually been put on the panel — a build
         * that returns quickly and renders slowly is still a slow switch. */
        while (s2_frames_captured() == frame_index) {
            lv_timer_handler();
            vTaskDelay(1);
        }
        const int64_t t3 = esp_timer_get_time();

        s_switches[round] = (s2_switch_t){
            .round = round,
            .destroy_us = (uint32_t)(t1 - t0),
            .build_us = (uint32_t)(t2 - t1),
            .first_frame_us = (uint32_t)(t3 - t2),
            .frame_index = frame_index,
            .tiles = s2_scene_tile_count(),
        };
    }

    ESP_LOGI(TAG, "S-2 measurement complete — %u frames captured",
             (unsigned)s2_frames_captured());
    s2_dump();
    printf("S2END\n");

    /*
     * Visual phase. §13's criterion is what the panel looks like, so the load
     * keeps running after the numbers are printed: pages switch once a second
     * with the same 10 Hz update rate, which is the worst case for both stutter
     * and tearing. Nothing is measured from here on — this exists to be watched
     * and filmed.
     */
#if S2_VISUAL_SWITCH
    /* Six seconds a page, not one. The first attempt switched every second,
     * which is long enough to see a page but too short to judge whether a
     * marquee label scrolls smoothly — the maintainer said so, and a criterion
     * nobody can actually evaluate is not a criterion. */
    ESP_LOGI(TAG, "visual phase: switching pages every 6 s, watch the panel");
    for (uint32_t round = 0;; round++) {
        s2_run(6000);
        s2_scene_destroy(page);
        page = s2_scene_build(round % S2_PAGE_COUNT);
    }
#else
    /*
     * The same load with the page switch removed. This exists to answer one
     * question the numbers cannot: when a flicker is reported, is it the page
     * rebuild blanking the screen, or is it the double-buffered direct mode
     * failing to keep both framebuffers in sync? Only the second would be a
     * defect in the configuration this spike recommends. Same tiles, same 10 Hz
     * updates, same animations — the rebuild is the only thing missing.
     */
    ESP_LOGI(TAG, "visual phase: no page switching, animations and updates only");
    (void)page;
    while (true) {
        s2_run(1000);
    }
#endif
}

void app_main(void)
{
#if S2_RADIO
    s2_radio_start();
#else
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGW(TAG, "radio idle — §6.2's artifact case is NOT covered by this run");
#endif

    ESP_ERROR_CHECK(s2_board_init());

    /*
     * design.md §6.1: all LVGL access happens on one task. The stack is sized
     * deliberately rather than inherited — S-3 hit a stack overflow in lv_init()
     * on the 3.5 KB default and it printed as memory corruption.
     */
    xTaskCreatePinnedToCore(s2_task, "s2", 12288, NULL, 4, NULL, 1);
}
