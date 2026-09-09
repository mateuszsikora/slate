/*
 * Slate — the Shelly integration provider.
 *
 * DESIGN.md ADR-3 (integrations are providers behind a neutral core), §5.1 (the
 * provider boundary), §5.2 (normalized snapshots), §5.3 (semantic actions).
 *
 * The first provider that reaches a device on its own. `direct` waits to be
 * pushed to and `ha` needs an automation system in the middle; this one polls
 * relays on the LAN and publishes what they say, so a panel with no companion
 * process anywhere still shows live state. That is the whole point of it — a
 * dashboard fed by `direct` goes to dashes the moment its feeder stops, and a
 * wall panel that depends on somebody's laptop being awake is not one.
 *
 * ## The address is in the resource id
 *
 * §3.3 makes resource ids opaque outside their provider, and this adapter
 * spends that freedom on its configuration:
 *
 *     {"provider": "shelly", "resource": "192.168.1.51/switch:0"}
 *     {"provider": "shelly", "resource": "192.168.1.51/power:0"}
 *     {"provider": "shelly", "resource": "shelly1-abcdef123456.local/switch:0"}
 *
 * §5.1's second core operation already hands a provider "the set of resource
 * ids the configuration references", so the binding set *is* the device list
 * and there is nothing else to store. No `POST /shelly`, no NVS, no picker
 * needed before the first tile works, and a dashboard exported to another panel
 * takes its devices with it.
 *
 * What that costs is DHCP: a lease that moves breaks a binding, and the fix is
 * a reservation on the router or the mDNS name above. It is the honest trade —
 * the alternative is a second copy of the device list that has to be kept in
 * step with the one already in the document.
 *
 * ## Two generations behind one name
 *
 * `switch:0` is a switch on both a Plus 2PM and a Shelly 1; which HTTP dialect
 * says so is discovered, not configured. `GET /shelly` answers on both and
 * carries `gen` only on the newer one, so the adapter probes a host and then
 * uses `/rpc/Switch.*` or `/relay/N` accordingly, probing again whenever that
 * host stops answering — a relay swapped for a newer model keeps the binding
 * and changes the dialect. Nobody writing a dashboard should have to know which
 * generation is in which ceiling.
 *
 * ## What it does not do
 *
 * No authentication: these devices ship open on a LAN and the ones here report
 * `auth: false`. A password-protected Shelly needs credential storage, which is
 * §4.3's problem and not this component's until such a device exists. No
 * discovery catalog for the editor's picker either — `GET /resources` returns
 * the bound set, and mDNS discovery is the natural next step rather than a
 * prerequisite.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief §3.3's provider id, as written in a configuration binding. */
#define SLATE_SHELLY_PROVIDER_ID "shelly"

/**
 * @brief Register the provider with the store and the action bus.
 *
 * Call after slate_state_init() and slate_action_init(). Registration is what
 * makes a `shelly` binding resolvable at all (§3.3), so it happens even when
 * the polling task below cannot start: a tile then shows a missing resource
 * rather than a missing provider, which is the more accurate of the two.
 *
 * The provider registers UNCONFIGURED and stays there until a configuration
 * binds something to it — a panel whose dashboard names no Shelly has no Shelly
 * integration, and saying `online` about that would be describing an empty set.
 */
esp_err_t slate_shelly_init(void);

/**
 * @brief Start the poller and attach to the Wi-Fi lifecycle.
 *
 * Call after slate_wifi_init(). A station loss marks this provider `offline`,
 * which stales its resources and only its own (§5.2); recovery resumes the
 * sweep without rebuilding the UI tree.
 */
esp_err_t slate_shelly_start(void);

#ifdef SLATE_SHELLY_SELFTEST

/**
 * @brief Exercise the id parser, both generation mappings, and the handover.
 *
 * Built only with `-DSLATE_SHELLY_SELFTEST=1`. It needs no device and no
 * network, but it is **not** free of side effects and is not pure over its
 * inputs: it takes the bind lock, drives four binding sets through
 * `subscribe()` and the poller's adoption of them, publishes one snapshot the
 * store is expected to refuse, and moves this provider's status. Call it after
 * slate_state_init() and slate_shelly_init(), and before slate_shelly_start()
 * so the poller is not competing for the tables. It restores the empty binding
 * set it found. ESP_FAIL if any case failed.
 *
 * The parser and the two mappings are where a Shelly's own vocabulary becomes
 * §5.2's, so a mistake in them is a wrong number on a wall rather than a crash
 * — nothing else in the build would notice. The handover cases are here for the
 * opposite reason: that code is what keeps a sweep off the UI task, and it has
 * no observable surface at all until it fails.
 */
esp_err_t slate_shelly_selftest(void);

#endif

#ifdef __cplusplus
}
#endif
