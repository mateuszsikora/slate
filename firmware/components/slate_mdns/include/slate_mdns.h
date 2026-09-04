/*
 * Slate — mDNS advertisement.
 *
 * DESIGN.md §4.3, §9.2, §10.
 *
 * The panel answers to `slate-<mac6>.local` and advertises the HTTP service the
 * editor and the API are served from. §4.3 asks for this because the device
 * token outlives a DHCP lease: an address in a bookmark can stop being the
 * panel's address, and the name is what the editor falls back to when it does.
 *
 * The responder is a device-wide service rather than one component's private
 * tool. It used to be started by the Home Assistant adapter, which needs mDNS
 * to *discover* an instance (§5.5) — but the panel's own identity is not that
 * component's to own, and it disappeared entirely on a build without it.
 * Discovery now asks this component whether the responder is up.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the responder, claim `slate-<mac6>.local` and advertise `_http._tcp`.
 *
 * Call once, after the default event loop exists and before the components
 * that query mDNS. Both interfaces are covered: §4.3 requires the name on the
 * setup access point too, so the same address works before the panel has ever
 * joined a network.
 *
 * Degraded rather than fatal, like everything else the boot brings up: a panel
 * that could not claim its name is still reachable at its address, which is on
 * its own screen (§9.3).
 */
esp_err_t slate_mdns_init(void);

/**
 * @brief Whether the responder is running, and queries can be issued.
 *
 * `mdns_query_*` on an uninitialised responder is an error every caller would
 * otherwise have to translate; this is the question they actually have.
 */
bool slate_mdns_ready(void);

#ifdef __cplusplus
}
#endif
