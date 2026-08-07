/*
 * Panel bring-up and render instrumentation for spike S-2.
 *
 * Everything hardware-facing lives here so s2_main.c reads as the experiment
 * rather than as a driver. The pin map and the timings are the part of this
 * file that outlives the spike — see docs/spikes/s2.md and issue #6.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

/* design.md §3.2: the display is 800×480. */
#define S2_LCD_H_RES 800
#define S2_LCD_V_RES 480

/*
 * Panel timings. Total line and frame lengths decide the refresh rate, and the
 * refresh rate is the ceiling on everything this spike measures:
 *
 *   h_total = 800 + 8 + 8 + 4 = 820 pclk
 *   v_total = 480 + 16 + 16 + 4 = 516 lines
 *   16 MHz / (820 × 516) = 37.8 Hz
 *
 * The measured VSYNC rate is reported alongside this arithmetic in s2.md; if
 * they disagree, the arithmetic is wrong and so is every derived number.
 */
#define S2_PCLK_HZ            (16 * 1000 * 1000)
#define S2_HSYNC_PULSE_WIDTH  4
#define S2_HSYNC_BACK_PORCH   8
#define S2_HSYNC_FRONT_PORCH  8
#define S2_VSYNC_PULSE_WIDTH  4
#define S2_VSYNC_BACK_PORCH   16
#define S2_VSYNC_FRONT_PORCH  16

#define S2_V_TOTAL (S2_LCD_V_RES + S2_VSYNC_PULSE_WIDTH + S2_VSYNC_BACK_PORCH + S2_VSYNC_FRONT_PORCH)

/*
 * Build-time configuration. Each combination is a separate build — see
 * main/CMakeLists.txt for why these are not runtime switches.
 */
#ifndef S2_NUM_FBS
#define S2_NUM_FBS 1
#endif
#ifndef S2_BOUNCE
#define S2_BOUNCE 1
#endif

/* design.md §6.2: bounce buffer in internal SRAM, sized in lines of pixels. */
#define S2_BOUNCE_LINES 10
#define S2_BOUNCE_PX    (S2_BOUNCE_LINES * S2_LCD_H_RES)

/*
 * Height of each LVGL draw buffer, in lines, for the single-framebuffer builds.
 * §6.2 says "~1/10 screen" — 48 lines — and LVGL wants two of them so rendering
 * and flushing can overlap, which is 153 600 B of internal DMA-capable SRAM.
 * That is the figure the radio runs turn out not to be able to afford, so the
 * height is a knob and every run reports which one it used. The double-buffered
 * builds ignore this entirely: they render straight into the PSRAM framebuffers
 * and allocate no internal draw buffer at all.
 */
#ifndef S2_DRAW_LINES
#define S2_DRAW_LINES 48
#endif

/*
 * Whether the flush waits for VSYNC before letting LVGL render the next frame.
 * Only meaningful with two framebuffers; with one there is nothing to swap.
 * Default on, because with it off the panel skips rendered frames — see the
 * flush callback in s2_board.c. Build with -DS2_VSYNC_GATE=0 to reproduce the
 * flicker deliberately.
 */
#ifndef S2_VSYNC_GATE
#if S2_NUM_FBS >= 2
#define S2_VSYNC_GATE 1
#else
#define S2_VSYNC_GATE 0
#endif
#endif

/* One rendered LVGL frame. Collected into PSRAM and dumped after the run, never
 * printed inside the measured window — a printf in the middle of a frame-time
 * measurement measures the UART. */
typedef struct {
    int64_t  start_us;    /* LV_EVENT_RENDER_START, monotonic */
    uint32_t render_us;   /* render start → render ready, LVGL's own work */
    uint32_t flush_us;    /* time inside flush_cb, summed over the frame */
    uint16_t flush_cnt;   /* flushes in this frame; >1 in partial render mode */
    uint16_t tear_cnt;    /* flushes that raced the scan-out — see s2_board.c */
    uint32_t px;          /* pixels flushed this frame */
    uint32_t wait_us;     /* idle time waiting for VSYNC — not rendering cost */
} s2_frame_t;

esp_err_t s2_board_init(void);
void      s2_backlight(bool on);

/* Creates the LVGL display bound to the panel and installs the instrumentation.
 * Frames are appended to `sink` until `capacity` is reached. */
lv_display_t *s2_display_init(s2_frame_t *sink, uint32_t capacity);

uint32_t s2_frames_captured(void);
void     s2_frames_reset(void);

uint32_t s2_vsync_count(void);
int64_t  s2_vsync_period_us(void);

/* True when the framebuffer LVGL renders into is the one being scanned out, so
 * a tear is physically possible. False for the double-buffered build, where the
 * driver switches buffers on VSYNC and tearing is excluded by construction. */
bool s2_tearing_possible(void);
