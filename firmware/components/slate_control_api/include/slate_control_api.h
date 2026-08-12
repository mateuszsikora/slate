/* Slate — authenticated device-mode and recovery controls (DESIGN.md §4.1). */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register POST /mode, /identify and /factory_reset.
 *
 * All three routes require the device token on every interface. In particular,
 * factory reset is not part of §4.3's setup-access-point exception.
 */
esp_err_t slate_control_api_init(void);

#ifdef __cplusplus
}
#endif
