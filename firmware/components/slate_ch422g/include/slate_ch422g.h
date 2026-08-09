/*
 * Slate — the CH422G outputs used by the Waveshare panel.
 *
 * CH422G encodes its register in the I2C address rather than in a first data
 * byte. The generic expander component only exposes that device through the
 * deprecated I2C driver, so this board-local driver keeps the small part Slate
 * needs on the same `i2c_master_bus_handle_t` as the GT911.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct slate_ch422g *slate_ch422g_handle_t;

#define SLATE_CH422G_PIN(number) ((uint8_t) (UINT8_C(1) << (number)))

/**
 * @brief Attach the CH422G to an existing I2C master bus.
 *
 * All eight general-purpose outputs start high, matching the chip's reset
 * state, and are enabled as push-pull outputs. The returned handle owns its I2C
 * device handles but borrows the bus.
 */
esp_err_t slate_ch422g_new(i2c_master_bus_handle_t bus, slate_ch422g_handle_t *out_handle);

/**
 * @brief Set one or more of EXIO0..EXIO7 high or low.
 *
 * The cached register update and the I2C write are serialized together. Calls
 * from the display and touch tasks therefore preserve one another's pins
 * without an external lock.
 */
esp_err_t slate_ch422g_set_level(slate_ch422g_handle_t handle, uint8_t pin_mask, bool high);

/** @brief Remove the CH422G device handles and release the driver. */
esp_err_t slate_ch422g_del(slate_ch422g_handle_t handle);

#ifdef __cplusplus
}
#endif
