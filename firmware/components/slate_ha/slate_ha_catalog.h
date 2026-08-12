/* Private Home Assistant discovery catalog (§5.7). */

#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

typedef enum {
    SLATE_HA_CATALOG_ENTITIES = 0,
    SLATE_HA_CATALOG_DEVICES,
    SLATE_HA_CATALOG_AREAS,
    SLATE_HA_CATALOG_STATES,
    SLATE_HA_CATALOG_STAGE_COUNT,
} slate_ha_catalog_stage_t;

esp_err_t slate_ha_catalog_init(void);

/* Start one four-result refresh while retaining the last complete cache. */
esp_err_t slate_ha_catalog_begin(void);

/* Reduce a borrowed HA result immediately; no cJSON pointer is retained. */
esp_err_t slate_ha_catalog_accept(slate_ha_catalog_stage_t stage, bool success,
                                  const cJSON *result, bool *complete,
                                  bool *degraded);

/* Abandon only the in-flight refresh; the last complete catalog stays valid. */
void slate_ha_catalog_cancel(void);

/* `slate_api_resources_append_fn` implementation for the HA provider. */
esp_err_t slate_ha_catalog_append(void *ctx, cJSON *array);

/* Credentials now name another HA instance; no old catalog may survive. */
void slate_ha_catalog_clear(void);

#ifdef SLATE_HA_SELFTEST
esp_err_t slate_ha_catalog_selftest(void);
#endif
