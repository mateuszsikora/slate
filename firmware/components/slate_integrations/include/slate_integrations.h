/* Slate — management API for user-facing integrations. */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the named External API key management routes. */
esp_err_t slate_integrations_init(void);

#ifdef __cplusplus
}
#endif
