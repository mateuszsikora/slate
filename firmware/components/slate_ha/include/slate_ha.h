/*
 * Slate — Home Assistant connection provider.
 *
 * design.md ADR-3, §4.1, §5.1, §5.5 and §12. Home Assistant transport
 * details terminate in this component; the state store sees only provider
 * lifecycle status and the UI runtime cannot name a WebSocket frame.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLATE_HA_PROVIDER_ID "ha"

/**
 * @brief Register the neutral provider and the `/ha` API routes.
 *
 * Call after slate_state_init() and slate_api_init(). Network events are not
 * consumed until slate_ha_start(), because the API deliberately starts before
 * the radio so the setup access point already has a page to serve.
 */
esp_err_t slate_ha_init(void);

/**
 * @brief Attach to the Wi-Fi lifecycle and connect stored HA credentials.
 *
 * Call after slate_wifi_init(), once the default event loop exists. The
 * current Wi-Fi snapshot is read after handler registration so a connection
 * event that raced startup cannot be missed.
 */
esp_err_t slate_ha_start(void);

#ifdef SLATE_HA_SELFTEST

/** @brief Exercise URL normalization and the contractual backoff sequence. */
esp_err_t slate_ha_selftest(void);

#endif

#ifdef __cplusplus
}
#endif
