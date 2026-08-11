/* Private Home Assistant compressed-entity cache and normalizer. */

#pragma once

#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

esp_err_t slate_ha_entities_init(void);

/* Replace the exact entity-id set supplied by the active configuration. */
esp_err_t slate_ha_entities_bind(const char *const *resources, size_t count);

/* Append one atomic snapshot of the current ids and return its count. */
esp_err_t slate_ha_entities_append_ids(cJSON *array, size_t *count);

/* A new subscription starts with every requested entity absent until HA adds it. */
void slate_ha_entities_prepare_subscription(void);

/* Expand one subscribe_entities event and publish complete neutral snapshots. */
esp_err_t slate_ha_entities_process_event(const cJSON *event);

#ifdef SLATE_HA_SELFTEST
esp_err_t slate_ha_entities_selftest(void);
#endif
