/* Private Home Assistant compressed-entity cache and normalizer. */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "slate_state.h"

esp_err_t slate_ha_entities_init(void);

/* Replace the exact entity-id set; `changed` reports membership changes. */
esp_err_t slate_ha_entities_bind(const char *const *resources, size_t count,
                                 bool *changed);

/* Append one atomic snapshot of the current ids and return its count. */
esp_err_t slate_ha_entities_append_ids(cJSON *array, size_t *count);

/* A new subscription starts with every requested entity absent until HA adds it. */
esp_err_t slate_ha_entities_prepare_subscription(void);

/* Expand one subscribe_entities event and publish complete neutral snapshots. */
esp_err_t slate_ha_entities_process_event(const cJSON *event);

/* Normalize one object from HA's `get_states` result for the discovery catalog. */
esp_err_t slate_ha_entities_normalize_full_state(const cJSON *state,
                                                 slate_resource_t *out);

#ifdef SLATE_HA_SELFTEST
esp_err_t slate_ha_entities_selftest(void);
#endif
