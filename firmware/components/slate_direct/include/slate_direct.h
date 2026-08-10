/*
 * Slate — the direct integration provider.
 *
 * design.md ADR-3, §5.4 (this adapter), §5.1 (the provider boundary), §5.2
 * (normalized snapshots), §5.3 (semantic actions), §4.1 (`POST /direct/state`),
 * §4.2 (`provider_attach`, `action`, `action_result`).
 *
 * §5.4 calls this "the smallest useful interoperability path": a script or
 * Node-RED flow publishes normalized state over HTTP and receives semantic
 * actions over the device WebSocket, with no broker, cloud service or companion
 * process anywhere in it. It is also the reference implementation of the
 * provider contract, and that is the more important of its two jobs — ADR-3's
 * claim that the runtime is provider-neutral is only worth anything if a second
 * provider exists to keep the first one honest, and this one exists a milestone
 * before Home Assistant does.
 *
 * The component owns three things and deliberately not a fourth:
 *
 *   publication   `POST /api/v1/direct/state` parses §5.2 and hands the result
 *                 to slate_state_publish(), which is where the three refusals
 *                 §5.4 names are decided for every provider rather than for
 *                 this route.
 *   status        §5.4's `degraded` with no attached consumer, `online` with
 *                 one. §5.2 makes that distinction load-bearing: `degraded`
 *                 does not stale anything, so a publish-only script keeps a
 *                 fresh dashboard with no WebSocket client at all.
 *   dispatch      the `action` frame, and the `action_result` that answers it.
 *
 *   NOT the bus   §5.3's optimistic update, its pending marker and its 3 s
 *                 revert belong to the common core (#18). A private copy here
 *                 is exactly what ADR-3 exists to prevent, and it would be the
 *                 copy the Home Assistant adapter then had to disagree with.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "slate_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One semantic action to be carried to the attached consumer (§5.3).
 *
 * As much of §5.3's request as version 1 has a use for: every action in
 * slate_action_t either carries nothing or carries one number, and the pair of
 * flags says which. `params` in the frame is `{}` for the first kind and
 * `{"value": n}` for the second, which is §5.3's own spelling.
 *
 * The neutral request type is #18's to define, along with the routing and the
 * capability check that precedes it. This is the narrow thing the bus calls
 * once it has decided that this provider is the one — not a second bus.
 */
typedef struct {
    const char *resource;
    slate_action_t action;
    bool has_value;
    int32_t value;
} slate_direct_action_t;

/**
 * @brief What became of a dispatched action, from §4.2's `action_result`.
 *
 * Runs on the WebSocket receive task, so it must not block. `error` is §5.4's
 * optional stable string and is NULL when the client did not send one; it does
 * not outlive the call.
 *
 * Only a real answer from the consumer arrives here. A dispatch that could not
 * be delivered says so by returning an error instead, so a caller never has to
 * decide whether a callback it has not received yet is still coming.
 */
typedef void (*slate_direct_result_fn)(void *ctx, uint32_t id, bool success, const char *error);

/**
 * @brief Register the provider, its route and its WebSocket attachment.
 *
 * Call after slate_state_init(), slate_api_init() and slate_ws_init(). The
 * provider registers with the store even when the route cannot be added, so a
 * configuration binding `direct` still resolves (§3.3) instead of presenting
 * SLATE_PRESENT_MISSING_PROVIDER on a panel whose HTTP server merely ran out of
 * handler slots.
 */
esp_err_t slate_direct_init(void);

/** @brief Install the result handler. NULL removes it. #18 is its owner. */
void slate_direct_set_result_handler(slate_direct_result_fn fn, void *ctx);

/**
 * @brief Send one action to the attached consumer (§5.4).
 *
 * `id` is the caller's correlation id and is echoed in the `action_result` that
 * answers it; this component neither allocates nor remembers it, because the
 * table of what is still outstanding is the action bus's (#18).
 *
 * ESP_ERR_INVALID_STATE when no consumer is attached, which is §5.4's "new
 * actions fail immediately rather than waiting three seconds" and is why this
 * reports failure by return value rather than through the result callback.
 * ESP_ERR_NO_MEM when the consumer is a full queue behind, ESP_ERR_INVALID_ARG
 * for an unknown action or a malformed resource id, and ESP_ERR_INVALID_SIZE
 * for a resource id whose frame will not fit one — the transport owns that
 * ceiling, so there is not a second copy of it here to drift out of step.
 *
 * Safe from any task.
 */
esp_err_t slate_direct_dispatch(uint32_t id, const slate_direct_action_t *action);

#ifdef SLATE_DIRECT_SELFTEST

/**
 * @brief Exercise §5.4's whole round trip on the device, before a tile exists.
 *
 * Development verifier for #74, called by main only when built with
 * `-DSLATE_DIRECT_SELFTEST=1`. The end-to-end sentence in that issue's done-when
 * needs the UI runtime (#20) and the light component (#22) to supply the tap;
 * this drives everything on the transport side of them — the real loopback HTTP
 * route with the real token, every refusal, attachment and `provider_busy`,
 * dispatch, both `action_result` directions, and the status transitions — and
 * logs PASS/FAIL per case. ESP_FAIL if any case failed.
 *
 * It leaves its fixture resources bound, because nothing binds a resource until
 * #19 parses a configuration and an external `curl` needs something to publish
 * to. That is the whole reason this knob exists rather than a host test, and it
 * goes away with #19 and #20.
 */
esp_err_t slate_direct_selftest(void);

#endif

#ifdef __cplusplus
}
#endif
