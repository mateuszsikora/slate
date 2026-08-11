/*
 * Slate — HTTP boundary for the active dashboard configuration.
 *
 * DESIGN.md ADR-1/ADR-4/ADR-5, §4.1, §6.4 and §10. Parsing, rendering,
 * persistence and WebSocket delivery remain owned by their existing
 * components; this component orders those operations for GET and PUT.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register GET/PUT /config and POST /config/validate. */
esp_err_t slate_config_api_init(void);

#ifdef SLATE_CONFIG_API_SELFTEST
/** @brief Exercise request-mode parsing invariants without writing flash. */
esp_err_t slate_config_api_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
