/*
 * Slate — GT911 capacitive touch, as an LVGL input device.
 *
 * design.md §6.1. This component owns the two pins that belong to the touch
 * controller and nothing else on the board: the interrupt line on the SoC and
 * the reset line on the CH422G. The I²C bus and expander belong to
 * slate_display, which brought them up for the panel and lends them here rather
 * than have two owners for one bus.
 *
 * The input device is LVGL, so §6.1's single-owner rule applies with no
 * exception: slate_touch_init() runs on the display task and the read callback
 * it registers runs there too. Nothing in this component may be called from an
 * API handler or a provider.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "slate_ch422g.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Called on the LVGL task at the first sample of a physical press. Returning
 * true consumes that press through release, which lets a dark panel wake
 * without also operating the tile under the finger.
 */
typedef bool (*slate_touch_press_observer_t)(void *ctx);

/**
 * Called once when one uninterrupted physical press reaches its configured
 * duration. The triggering press is consumed through release before this is
 * called, so a recovery gesture cannot also activate the tile underneath it.
 * The callback runs on the LVGL task and must only hand work to another task.
 */
typedef void (*slate_touch_hold_observer_t)(void *ctx);

/**
 * @brief Reset the GT911, attach it to the bus and register an LVGL pointer.
 *
 * Must be called on the LVGL task, after lv_init() and after a display exists —
 * the panel resolution is read from that display rather than declared again
 * here. `i2c_bus` is an already-installed current-driver master bus and
 * `expander` is a live CH422G handle. Both are borrowed; neither is released by
 * this component.
 *
 * A failure releases everything it had taken, including the interrupt pin, and
 * leaves the panel without touch but with everything else intact. That is the
 * degradation the caller is expected to accept: a dashboard that cannot be
 * tapped is worth more than a boot loop.
 */
esp_err_t slate_touch_init(i2c_master_bus_handle_t i2c_bus, slate_ch422g_handle_t expander);

/** @brief Whether the controller answered and an LVGL input device exists. */
bool slate_touch_ready(void);

/**
 * @brief Replace the process-wide press observer.
 *
 * Safe from any task. Passing NULL clears the observer. Registration is valid
 * even when the controller is unavailable, so callers do not need a second
 * initialization order for headless degradation.
 */
esp_err_t slate_touch_set_press_observer(slate_touch_press_observer_t observer,
                                         void *ctx);

/**
 * @brief Replace the process-wide continuous-hold observer.
 *
 * Safe from any task. A non-NULL observer requires a non-zero duration. Passing
 * NULL clears the observer and requires duration_ms and ctx to be zero/NULL.
 * Registering a hold does not consume ordinary taps; only the press that
 * actually reaches the threshold is removed from LVGL through its release.
 */
esp_err_t slate_touch_set_hold_observer(uint32_t duration_ms,
                                        slate_touch_hold_observer_t observer,
                                        void *ctx);

#ifdef __cplusplus
}
#endif
