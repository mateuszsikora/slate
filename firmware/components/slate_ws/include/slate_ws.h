/*
 * Slate — authenticated WebSocket diagnostics and editor event channel.
 *
 * design.md §4.2 and §11.3. The component owns the protocol and the retained
 * log ring. It does not own UI objects: mode is a small piece of shared state,
 * and later UI/configuration components use the functions below without
 * receiving an HTTP server or WebSocket handle.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SLATE_WS_MODE_NORMAL = 0,
    SLATE_WS_MODE_EDIT,
} slate_ws_mode_t;

/** @brief Register `/api/v1/ws`, start the dispatcher and retain future logs. */
esp_err_t slate_ws_init(void);

/** @brief Current editor mode; safe from any task. */
slate_ws_mode_t slate_ws_mode(void);

/**
 * @brief Publish the §4.2 `reloaded` event to authenticated clients.
 *
 * The latest pending reload replaces an older unsent one for a slow client:
 * configuration replacement is state, not a journal, and only the tree now on
 * screen is useful to an editor reconnecting after backpressure.
 */
esp_err_t slate_ws_publish_reloaded(unsigned schema, size_t tiles);

#ifdef SLATE_WS_SELFTEST
/** @brief Run component invariants that do not require an external client. */
esp_err_t slate_ws_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
