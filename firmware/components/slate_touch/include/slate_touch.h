/*
 * Slate — GT911 capacitive touch, as an LVGL input device.
 *
 * design.md §6.1. This component owns the two pins that belong to the touch
 * controller and nothing else on the board: the interrupt line on the SoC and
 * the reset line on the CH422G. The I²C bus and the expander itself belong to
 * slate_display, which brought both up for the panel and lends them here rather
 * than have two owners for one bus.
 *
 * The input device is LVGL, so §6.1's single-owner rule applies with no
 * exception: slate_touch_init() runs on the display task and the read callback
 * it registers runs there too. Nothing in this component may be called from an
 * API handler or a provider.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
/* Same package and the same qualified path as the CH422G header slate_display
 * includes: `esp_io_expander.h` exists twice in `espressif/esp32_io_expander`,
 * and the copy under `base/` is the deprecated one. */
#include "port/esp_io_expander.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset the GT911, attach it to the bus and register an LVGL pointer.
 *
 * Must be called on the LVGL task, after lv_init() and after a display exists.
 * `i2c_port` is an already-installed legacy `driver/i2c.h` master port and
 * `expander` a live CH422G handle; both are borrowed and neither is released by
 * this component.
 *
 * A failure leaves the panel without touch but with everything else intact,
 * which is the degradation the caller is expected to accept: a dashboard that
 * cannot be tapped is worth more than a boot loop.
 */
esp_err_t slate_touch_init(int i2c_port, esp_io_expander_handle_t expander);

/** @brief Whether the controller answered and an LVGL input device exists. */
bool slate_touch_ready(void);

#ifdef __cplusplus
}
#endif
