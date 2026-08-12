/*
 * Slate — authenticated WebSocket diagnostics and editor event channel.
 *
 * design.md §4.2 and §11.3. The component owns the protocol and the retained
 * log ring. It does not own UI objects: mode is a small piece of shared state,
 * and later UI/configuration components use the functions below without
 * receiving an HTTP server or WebSocket handle.
 */

#pragma once

#include <stdbool.h>
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

/** @brief Called after the shared editor mode changes; must not block. */
typedef void (*slate_ws_mode_observer_fn)(void *ctx, slate_ws_mode_t mode);

/**
 * @brief Start retaining logs before any Slate subsystem initialisation.
 *
 * This installs the application log hook and uses no dynamic allocation. Call
 * at the start of app_main() so the backlog contains the boot report and board
 * bring-up even though the HTTP server necessarily starts later. Repeated and
 * concurrent calls are safe; the hook is installed exactly once.
 */
esp_err_t slate_ws_capture_init(void);

/** @brief Register `/api/v1/ws` and start its dispatcher task. */
esp_err_t slate_ws_init(void);

/** @brief Current editor mode; safe from any task. */
slate_ws_mode_t slate_ws_mode(void);

/**
 * @brief Change editor mode outside a WebSocket session.
 *
 * `POST /mode` uses the same state and observer path as the WebSocket frame,
 * so transient configuration and touch suppression cannot disagree about the
 * active mode. An HTTP-created edit session has no ping owner and therefore
 * falls back to normal after the same 60-second safety deadline.
 */
esp_err_t slate_ws_mode_set(slate_ws_mode_t mode);

/**
 * @brief Install the one observer that owns mode-dependent application state.
 *
 * Passing NULL removes the observer. A second non-NULL observer is refused.
 * The callback runs after the WebSocket state lock is released, on whichever
 * task changed the mode, and must hand off any blocking work.
 * Returns ESP_ERR_NOT_FOUND before the WebSocket task is available.
 */
esp_err_t slate_ws_mode_observer_set(slate_ws_mode_observer_fn observer, void *ctx);

/* --- Provider action consumers (§4.2, §5.4) ------------------------------ */

/**
 * @brief Told that this provider's action consumer came or went.
 *
 * §5.4 makes the attachment a lifecycle event and not merely a routing detail:
 * the direct provider is `degraded` with no consumer attached and `online` with
 * one, and a detach has to fail new actions immediately rather than let them
 * wait out §5.3's three seconds. Runs on the WebSocket task, so it must not
 * block; a detach can therefore arrive from a socket closing rather than from
 * anything the provider did.
 */
typedef void (*slate_ws_attach_fn)(void *ctx, bool attached);

/**
 * @brief Deliver an `action_result` frame (§4.2) to the provider that sent it.
 *
 * `error` is the optional stable string of §5.4 and is NULL when the client did
 * not supply one. It and the frame it came from are freed when this returns, so
 * a provider that needs either afterwards copies it. Late and duplicate results
 * are passed through: §5.3 puts "duplicate results are ignored" in the action
 * bus, which is the component that knows which ids are still pending.
 */
typedef void (*slate_ws_result_fn)(void *ctx, uint32_t id, bool success, const char *error);

/**
 * @brief A provider that accepts action consumers over the WebSocket.
 *
 * `id` must outlive the firmware, on the same reasoning as the store's
 * registration: it is the §3.3 provider id, matched against the one a client
 * names in `provider_attach`. Either callback may be NULL.
 */
typedef struct {
    const char *id;
    slate_ws_attach_fn on_attach;
    slate_ws_result_fn on_result;
    void *ctx;
} slate_ws_provider_t;

/**
 * @brief Register a provider so clients may attach to it.
 *
 * The attachment slot lives here rather than in the provider because this is the
 * only component that can see a client disappear: §5.4's single consumer is a
 * property of a session, and a provider holding a client handle would have to
 * poll to notice a socket close.
 *
 * ESP_ERR_INVALID_STATE if the id is already registered, ESP_ERR_NO_MEM if the
 * table is full, ESP_ERR_INVALID_ARG for a NULL or empty id.
 */
esp_err_t slate_ws_provider_register(const slate_ws_provider_t *provider);

/** @brief Whether an authenticated client is attached to `id` (§5.4). */
bool slate_ws_provider_is_attached(const char *id);

/**
 * @brief Queue one already-formatted frame to a provider's attached consumer.
 *
 * The envelope is §4.2's and its contents are the provider's, which is what
 * keeps this component free of §5.3's action vocabulary. Frames are queued
 * rather than sent inline because esp_http_server owns the socket and this may
 * be called from any task; the queue is short on purpose.
 *
 * ESP_ERR_INVALID_STATE when nothing is attached — §5.4's "new actions fail
 * immediately" — ESP_ERR_NO_MEM when the consumer is behind by a full queue,
 * and ESP_ERR_INVALID_SIZE for a frame larger than one action can need.
 */
esp_err_t slate_ws_provider_send(const char *id, const char *text, size_t len);

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
