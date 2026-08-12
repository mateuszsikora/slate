/* Private Home Assistant semantic-action mapping and result correlation. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "slate_action.h"

#define SLATE_HA_ACTION_ERROR_MAX 31

/** A bus request copied before its caller-owned strings leave the UI task. */
typedef struct {
    uint32_t bus_id;
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    slate_action_t action;
    slate_action_value_type_t value_type;
    union {
        bool boolean;
        int32_t number;
    } value;
} slate_ha_action_request_t;

esp_err_t slate_ha_action_request_copy(slate_ha_action_request_t *out, uint32_t bus_id,
                                       const slate_action_request_t *request);

/** Build one HA `call_service` command. The caller owns the returned object. */
esp_err_t slate_ha_action_frame(uint32_t command_id,
                                const slate_ha_action_request_t *request,
                                cJSON **out);

/** Parse `success` and reduce provider-native failures to one stable log reason. */
esp_err_t slate_ha_action_result_fields(const cJSON *root, bool *success,
                                        char *error, size_t error_len);

/** Allocate the bounded command-id/action-id correlation table. Idempotent. */
esp_err_t slate_ha_action_tracker_init(void);

esp_err_t slate_ha_action_track(uint32_t generation, uint32_t command_id,
                                uint32_t bus_id);

/** Remove one matching correlation. False means unknown, stale or already handled. */
bool slate_ha_action_take(uint32_t generation, uint32_t command_id,
                          uint32_t *bus_id);

/** Forget every HA command correlation. The common bus owns visible reversion. */
void slate_ha_action_clear(void);

#ifdef SLATE_HA_SELFTEST
esp_err_t slate_ha_actions_selftest(void);
#endif
