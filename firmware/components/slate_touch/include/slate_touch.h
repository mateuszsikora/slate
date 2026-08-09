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

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "slate_ch422g.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
