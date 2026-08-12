/*
 * Slate — provider-neutral declarative UI runtime.
 *
 * DESIGN.md ADR-1–ADR-3, §3.2, §5.1–§5.3 and §6.4. The runtime consumes the
 * owned model produced by slate_config and copies everything it needs into an
 * LVGL tree. It never receives provider payloads and never hands LVGL objects
 * to a provider or API task.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "slate_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool configured;
    unsigned schema;
    size_t tiles;
} slate_ui_config_info_t;

/**
 * @brief Attach the runtime to the display/state/action boundaries and load
 *        the configuration persisted in LittleFS.
 *
 * A missing document renders the unconfigured presentation. An invalid stored
 * document renders an error presentation; a future schema specifically asks
 * for newer firmware. Call once after the display and providers are ready.
 */
esp_err_t slate_ui_init(void);

/**
 * @brief Build and activate one already-validated configuration.
 *
 * This call is synchronous. The caller retains ownership of @p config and may
 * free it as soon as the function returns. A replacement is built off-screen
 * on the LVGL task, then slate_state_bind() activates its provider-qualified
 * binding set. Only after both succeed is the new screen loaded and the old
 * tree destroyed. Any failure leaves the current tree and subscriptions live.
 */
esp_err_t slate_ui_rebuild(const slate_config_t *config);

/**
 * @brief Replace the current presentation with the document stored in LittleFS.
 *
 * This is the inverse of a RAM-only configuration preview. A valid stored
 * document is rebuilt through the same atomic boundary as slate_ui_rebuild().
 * A missing, unreadable or invalid document restores the corresponding
 * unconfigured/error presentation and clears provider subscriptions. @p out
 * may be NULL; otherwise it describes a successfully activated dashboard and
 * remains zeroed for an unconfigured/error presentation.
 */
esp_err_t slate_ui_restore_stored(slate_ui_config_info_t *out);

/** @brief Whether the runtime has successfully attached to the display task. */
bool slate_ui_ready(void);

/**
 * @brief Apply §6.5's edit-mode presentation and touch policy.
 *
 * Safe from any task. The system bar redraw is queued onto the LVGL owner.
 * Entering edit mode suppresses semantic actions immediately; leaving it keeps
 * them suppressed until slate_ui_mode_restore_complete().
 */
void slate_ui_mode_set(bool edit);

/**
 * @brief Finish leaving edit mode after the persisted tree is active.
 *
 * Safe against a concurrent return to edit mode; actions are enabled only if
 * the matching normal-mode transition is still current.
 */
void slate_ui_mode_restore_complete(void);

/**
 * @brief Refresh an error/pairing presentation after the station gets an IP.
 *
 * Dashboards do not rebuild for a lease renewal. An unconfigured or invalid
 * dashboard does, because §4.3's QR and plain-text recovery address have just
 * become available.
 */
void slate_ui_network_connected(void);

#ifdef SLATE_UI_SELFTEST
/** @brief Run issue #20's deterministic runtime verifier on the LVGL task. */
esp_err_t slate_ui_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
