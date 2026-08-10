/*
 * Slate — provider-neutral semantic action bus.
 *
 * docs/DESIGN.md ADR-3, §5.1, §5.3 and §7.5. Components ask this bus to
 * `toggle` or `set_brightness`; adapters translate that request into their own
 * transport. Neither side can express the other's vocabulary through this API.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "slate_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The typed parameter carried by a semantic action. */
typedef enum {
    SLATE_ACTION_VALUE_NONE = 0,
    SLATE_ACTION_VALUE_BOOL,
    SLATE_ACTION_VALUE_NUMBER,
} slate_action_value_type_t;

typedef struct {
    const char *provider;
    const char *resource;
    slate_action_t action;
    slate_action_value_type_t value_type;
    union {
        bool boolean;
        int32_t number;
    } value;
} slate_action_request_t;

/**
 * @brief Dispatch one already-validated request through a provider adapter.
 *
 * The callback runs without the bus lock. Returning an error is an immediate
 * delivery failure; returning ESP_OK only acknowledges that the adapter took
 * responsibility for reporting a later result. The request and its strings are
 * borrowed only for the duration of the callback.
 */
typedef esp_err_t (*slate_action_dispatch_fn)(void *ctx, uint32_t id,
                                              const slate_action_request_t *request);

typedef struct {
    const char *id;
    slate_action_dispatch_fn dispatch;
    void *ctx;
} slate_action_provider_t;

typedef enum {
    SLATE_ACTION_IDLE = 0,
    SLATE_ACTION_PENDING,
    SLATE_ACTION_ERROR,
} slate_action_phase_t;

/**
 * @brief The action-owned overlay a component applies to confirmed state.
 *
 * `optimistic` is meaningful only while phase is PENDING. ERROR deliberately
 * carries no alternative state: the component immediately falls back to the
 * confirmed `slate_resource_t` and shows its brief failure treatment.
 */
typedef struct {
    slate_action_phase_t phase;
    uint32_t id;
    slate_state_value_t optimistic;
} slate_action_feedback_t;

/** @brief Initialise the bus and its 3 s deadline timer. Idempotent. */
esp_err_t slate_action_init(void);

/** @brief Register one provider adapter. Provider ids are unique. */
esp_err_t slate_action_provider_register(const slate_action_provider_t *provider);

/**
 * @brief Validate, apply optimistically and route one semantic action.
 *
 * The current resource must be present, fresh and available, and must advertise
 * the requested capability. Required values are checked against the normalized
 * capability range before the provider sees them. On successful creation,
 * `out_id` receives the correlation id even when delivery then fails.
 */
esp_err_t slate_action_dispatch(const slate_action_request_t *request, uint32_t *out_id);

/**
 * @brief Deliver an adapter acknowledgement or explicit failure.
 *
 * Success acknowledges delivery but does not confirm state. A matching provider
 * snapshot does that. Failure reverts immediately. Unknown, duplicate and late
 * results are ignored.
 */
void slate_action_result(const char *provider, uint32_t id, bool success, const char *error);

/**
 * @brief Revert every pending action owned by a provider that disconnected.
 *
 * Adapters call this when their action transport becomes unavailable. Existing
 * requests fail immediately; new requests are still refused by the adapter's
 * dispatch callback until its transport returns.
 */
void slate_action_provider_unavailable(const char *provider, const char *error);

/** @brief Copy the current optimistic/error overlay for one resource. */
esp_err_t slate_action_feedback(const char *provider, const char *resource,
                                slate_action_feedback_t *out);

/** @brief A cheap wake used by the future UI task when feedback changes. */
typedef void (*slate_action_wake_fn)(void *ctx);

/** @brief Install the sole UI wake callback. NULL removes it. */
void slate_action_set_wake(slate_action_wake_fn wake, void *ctx);

#ifdef SLATE_ACTION_SELFTEST
/** @brief Exercise §5.3 against the real state store and a fixture adapter. */
esp_err_t slate_action_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
