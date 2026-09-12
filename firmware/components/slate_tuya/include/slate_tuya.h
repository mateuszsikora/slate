/*
 * Slate — the Tuya / Smart Life cloud integration provider.
 *
 * DESIGN.md ADR-3 (integrations are providers behind a neutral core), §5.1 (the
 * provider boundary), §5.2 (normalized snapshots), §5.3 (semantic actions).
 *
 * The second provider that reaches its devices by itself, and the first that
 * leaves the LAN to do it: lights, covers and sensors behind the Smart Life or
 * Tuya Smart app are polled through Tuya's official OpenAPI, so a panel with
 * Tuya devices works with no automation system and no companion script — the
 * same property that made the Shelly provider worth having, bought here at the
 * price of a cloud dependency. That price is the design's, stated where the
 * README says what a panel talks to: the daily update check stopped being the
 * only thing leaving the network the day this component shipped.
 *
 * ## Credentials, not bindings, are the configuration
 *
 * Shelly spends §3.3's opaque resource id on its whole configuration — the
 * address is the binding. Tuya's ids are 22-character hex strings minted by
 * somebody else's cloud, so that freedom buys nothing here: nobody types a
 * Tuya device id from memory. Instead a cloud project's Access ID, Access
 * Secret, region and app-account UID are entered once on the editor's
 * Integrations page (`POST /tuya`), stored write-only in NVS exactly like the
 * Home Assistant token, and the editor's resource picker lists devices by the
 * names the app already knows them by. The binding still carries the device
 * id — a dashboard exported to another panel takes its devices with it, as
 * long as that panel is configured for the same cloud project.
 *
 * ## One task owns the cloud
 *
 * The poller task owns the live client, the device tables and the published
 * last-known values, exactly as the Shelly poller owns its relays, and for the
 * same reason: `subscribe()` runs on the LVGL task (§6.4) and must never wait
 * on a socket. The HTTP routes park long requests on a small configure task
 * (the Home Assistant precedent), the picker's catalog builds a throwaway
 * client of its own rather than borrowing the poller's, and a credentials
 * change is picked up by the poller comparing the store against its client at
 * the top of each pass — so no route ever mutates state another task is
 * reading.
 *
 * ## What it does not do
 *
 * No message-queue push: a wall panel refreshes by glances, not milliseconds,
 * and a second TLS connection with a subscription to manage is a lot of
 * failure modes for a few seconds of freshness. No scene sync: a scene bar's
 * bindings are the panel's own business. And no local Tuya protocol — the
 * local key route would put per-device keys in NVS and a UDP probe loop on the
 * radio for devices the cloud already reaches, which is complexity this
 * component refuses until the cloud is proven not enough.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief §3.3's provider id, as written in a configuration binding. */
#define SLATE_TUYA_PROVIDER_ID "tuya"

/**
 * @brief Register the provider with the store and the action bus, and mount
 * the /api/v1/tuya routes and the `tuya` resource catalog.
 *
 * Call after slate_state_init(), slate_action_init() and slate_api_init(),
 * from the same start_api() phase as the other adapters. Registration happens
 * even when no credentials are stored: a binding names `tuya` on a panel that
 * was never configured sees §3.3's missing resource rather than a missing
 * provider, and `GET /resources?provider=tuya` can answer 409
 * `provider_unconfigured` rather than 404.
 */
esp_err_t slate_tuya_init(void);

/**
 * @brief Start the poller and attach to the Wi-Fi lifecycle.
 *
 * Call after slate_wifi_init(). A station loss marks this provider `offline`,
 * which stales its resources and only its own (§5.2); recovery resumes the
 * sweep without rebuilding the UI tree. The provider starts `unconfigured`
 * and stays there until credentials are stored, bound devices or none: a
 * panel with no Tuya credentials has no Tuya integration, and saying anything
 * else about it would be describing an empty set.
 */
esp_err_t slate_tuya_start(void);

#ifdef SLATE_TUYA_SELFTEST

/**
 * @brief Exercise the bounded resource-id codec, category table and one snapshot
 * through the live store registration.
 *
 * Built only with `-DSLATE_TUYA_SELFTEST=1`. It needs no device, no network
 * and no credentials, but it is **not** free of side effects: it takes the
 * bind lock, drives a fixture binding set through `subscribe()`, publishes a
 * snapshot the store is expected to refuse, and moves this provider's status.
 * Call it after slate_state_init() and slate_tuya_init(), and before
 * slate_tuya_start() so the poller is not competing for the tables. It
 * restores the empty binding set it found. ESP_FAIL if any case failed.
 *
 * The DP mapping itself is slate_tuya_map's to check — on the host, where a
 * fixture can run against real cJSON. What only this component can verify is
 * the seam around it: the `/humidity` resource codec (the map module selects
 * the reading, this component returns the unsuffixed cloud id) and the
 * status the store holds for a provider with credentials but no bindings.
 */
esp_err_t slate_tuya_selftest(void);

#endif

#ifdef __cplusplus
}
#endif
