/*
 * Slate — RGB panel and the single-owner LVGL task.
 *
 * design.md §6.1–§6.2. The task created by slate_display_init() is the only
 * task allowed to call LVGL. API handlers, the Home Assistant client and later
 * UI components cross that boundary through slate_display_post(); none of them
 * receives an lv_obj_t or a display handle.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*slate_display_work_fn)(void *ctx);

/**
 * @brief Which backlight variant this board is running.
 *
 * §16 asks the firmware to distinguish the two. On an unmodified board the
 * CH422G drives the backlight enable as a plain output and `ON_OFF` is the only
 * answer there is; `PWM` names the variant that appears once the testpoint
 * described in #41 has been bridged to a GPIO. That bridge cannot be probed —
 * the installer chooses the pin, and a pin nobody soldered reads the same as
 * one somebody did — so it will arrive as configuration rather than detection.
 * #28's night schedule branches here rather than assuming either.
 */
typedef enum {
    SLATE_DISPLAY_BACKLIGHT_ON_OFF,
    SLATE_DISPLAY_BACKLIGHT_PWM,
} slate_display_backlight_mode_t;

typedef struct {
    bool available;
    size_t free_size;
    size_t total_size;
    uint8_t frag_pct;
} slate_display_heap_metrics_t;

/* §9's recovery presentation is intentionally narrower than the WiFi state:
 * the network component decides what happened and gives the display only the
 * text a person in front of the panel needs. */
#define SLATE_DISPLAY_SETUP_NETWORK_LEN    33
#define SLATE_DISPLAY_SETUP_ADDRESS_LEN    16
#define SLATE_DISPLAY_SETUP_PASSPHRASE_LEN 65
#define SLATE_DISPLAY_SETUP_MESSAGE_LEN    128

typedef struct {
    /** Keep the current screen and place a recovery banner over it (§9.4). */
    bool banner;
    char network[SLATE_DISPLAY_SETUP_NETWORK_LEN];
    char address[SLATE_DISPLAY_SETUP_ADDRESS_LEN];
    char passphrase[SLATE_DISPLAY_SETUP_PASSPHRASE_LEN];
    char message[SLATE_DISPLAY_SETUP_MESSAGE_LEN];
} slate_display_setup_t;

/**
 * @brief Bring up the panel and start its LVGL owner task.
 *
 * The call waits until the task has initialised the RGB driver and LVGL, then
 * queues the bring-up test pattern. The backlight stays off until LVGL's first
 * complete frame has reached a VSYNC boundary, so uninitialised PSRAM is never
 * shown. Call once, before starting subsystems that post display work.
 */
esp_err_t slate_display_init(void);

/**
 * @brief Queue work that will execute on the LVGL task.
 *
 * `fn` must not block and must not call slate_display_post() recursively. `ctx`
 * is borrowed rather than copied and must remain valid until `fn` runs.
 * `timeout_ms == 0` is non-blocking. A full queue returns ESP_ERR_TIMEOUT.
 */
esp_err_t slate_display_post(slate_display_work_fn fn, void *ctx, uint32_t timeout_ms);

/**
 * @brief Show §9's setup card or runtime-loss banner.
 *
 * The request is copied before this function returns; the caller may clear or
 * release it immediately. In particular, the setup access-point passphrase
 * must not be kept on another task's stack while LVGL catches up.
 */
esp_err_t slate_display_setup_show(const slate_display_setup_t *setup);

/**
 * @brief Dismiss §9's setup presentation after the station reconnects.
 *
 * The request is desired state rather than ordinary queued work, so a full
 * LVGL work queue cannot leave a stale recovery card on screen.
 */
esp_err_t slate_display_setup_hide(void);

/**
 * @brief Flash a short accent overlay without replacing the active screen.
 *
 * The asynchronous primitive behind authenticated `POST /identify`. Repeated
 * calls restart the sequence, so identifying one panel cannot leave a timer or
 * overlay behind.
 */
esp_err_t slate_display_identify(void);

/**
 * @brief Replace the active presentation with factory-reset progress.
 *
 * Used by both authenticated browser reset and the ten-second panel gesture.
 * The call is asynchronous and keeps the backlight on; the reset path reboots
 * even if the display queue is unavailable.
 */
esp_err_t slate_display_factory_reset_show(void);

/** @brief Whether panel and LVGL initialisation completed successfully. */
bool slate_display_ready(void);

/**
 * @brief Switch the backlight.
 *
 * Safe from any task. The expander driver serialises its cached register update
 * with the bus write rather than leaving it to whichever of #28's schedule,
 * the LVGL task and a future API handler arrives second.
 *
 * Turning the backlight off does not stop the panel, the renderer or touch;
 * §3.3's `screen_off_after` is a dark screen that still responds, not a
 * suspended one. An active setup presentation refuses `off` with
 * ESP_ERR_INVALID_STATE because §9.4 requires its recovery address to remain
 * readable.
 */
esp_err_t slate_display_backlight_set(bool on);

/** @brief Whether the backlight is currently on. */
bool slate_display_backlight_is_on(void);

/**
 * @brief Set the requested backlight level from 0 through 100 percent.
 *
 * The unmodified CH422G path quantizes zero to off and every non-zero value to
 * on. A future PWM implementation behind the same boundary can preserve the
 * requested level. While a setup card or recovery banner is active, values
 * below 100 are refused with ESP_ERR_INVALID_STATE so §9.4's address remains
 * readable.
 */
esp_err_t slate_display_brightness_set(uint8_t percent);

/** @brief Actual level after hardware quantization and setup protection. */
uint8_t slate_display_brightness_level(void);

/** @brief Whether a full setup card or runtime-recovery banner is visible. */
bool slate_display_setup_active(void);

/** @brief Which backlight variant is live on this board. */
slate_display_backlight_mode_t slate_display_backlight_mode(void);

#ifdef SLATE_DISPLAY_SELFTEST
/**
 * @brief Check §9's setup card against the worst input §9.2 allows.
 *
 * Builds the card on the LVGL task from a maximum-length SSID and asserts the
 * join row stays one line high and clear of the security row under it. It
 * refuses while a real setup presentation is on screen rather than replacing
 * §9.4's recovery address with a fixture.
 */
esp_err_t slate_display_selftest(void);
#endif

/**
 * @brief Copy the latest LVGL allocator snapshot.
 *
 * The snapshot is collected on LVGL's owner task at the diagnostics heartbeat
 * interval. Callers on API or diagnostics tasks therefore never cross §6.1's
 * single-owner boundary. `available` is false before successful display
 * initialisation.
 */
void slate_display_heap_metrics(slate_display_heap_metrics_t *out);

#ifdef __cplusplus
}
#endif
