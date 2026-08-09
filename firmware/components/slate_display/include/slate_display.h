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

typedef struct {
    bool available;
    size_t free_size;
    size_t total_size;
    uint8_t frag_pct;
} slate_display_heap_metrics_t;

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

/** @brief Whether panel and LVGL initialisation completed successfully. */
bool slate_display_ready(void);

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
