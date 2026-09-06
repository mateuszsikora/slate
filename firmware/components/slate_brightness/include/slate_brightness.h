/*
 * Slate — configured backlight schedule and inactivity policy.
 *
 * DESIGN.md §3.3, §6.5 and §9.4. The policy owns no UI objects. It consumes
 * the active configuration, reads the clock through slate_time and drives the
 * board through slate_display's task-safe backlight boundary.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "slate_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Start the policy task and attach it to the touch input boundary. */
esp_err_t slate_brightness_init(void);

/**
 * @brief The level a boot should light the panel at, from 1 through 100.
 *
 * The level this policy last applied for a scheduled reason, kept in NVS so it
 * survives a reboot and an OTA, and 100 % on a panel that has never applied
 * one. It answers a question nobody else can at that point in the boot: the
 * panel is up before the configuration is read and long before the clock syncs,
 * and §6.2's first frame has to light at *something* (#183).
 *
 * Safe before slate_brightness_init(), which is where it is meant to be called
 * from — app_main pushes it into slate_display_set_first_frame_level() before
 * the panel comes up. It is a remembered fact rather than an inference about
 * the time: an unsynced clock still may not decide it is night.
 */
uint8_t slate_brightness_boot_level(void);

/**
 * @brief Replace the live brightness settings.
 *
 * The settings are copied synchronously. Missing values use conservative
 * defaults: full brightness, no night window, no inactivity blanking and
 * touch wake enabled. Invalid ranges are clamped or ignored because §4.1 has
 * no schema-1 validation code for settings values.
 */
esp_err_t slate_brightness_configure(const slate_config_settings_t *settings);

#ifdef SLATE_BRIGHTNESS_SELFTEST
/** @brief Run the deterministic policy verifier without changing the display. */
esp_err_t slate_brightness_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
