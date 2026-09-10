/*
 * Slate — the Onkyo/Integra eISCP provider.
 *
 * DESIGN.md ADR-3 (integrations are providers behind a neutral core), §5.1 (the
 * provider boundary), §5.2 (normalized snapshots), §5.3 (semantic actions),
 * §5.9 (the Shelly adapter, which this one is deliberately not shaped like).
 *
 * The second adapter that reaches a device by itself, and the first that does
 * not poll. A receiver announces itself: turn the volume knob on the front
 * panel and an `MVL` frame arrives unasked, so the panel learns about it the
 * same way it learns about a tap. That is the point of building this one
 * second — `shelly` proved the provider boundary carries a poller, and a
 * boundary that only carries pollers is not neutral, it is a polling engine
 * with an interface on it.
 *
 * ## What it looks like in a dashboard
 *
 *     {"provider": "onkyo", "resource": "192.168.1.60/main"}
 *     {"provider": "onkyo", "resource": "192.168.1.60/input"}
 *     {"provider": "onkyo", "resource": "192.168.1.60/input:2b"}
 *
 * Same `<host>/<role>` grammar §5.9 established, and for the same reason: §5.1
 * hands a provider the resource ids the active dashboard binds, so the binding
 * set is the device list and there is nothing else to configure.
 *
 * | role | kind | is |
 * |------|--------|----|
 * | `main` | `light` | the receiver: power, and volume as brightness |
 * | `input` | `sensor` | the selected input, by name |
 * | `input:<code>` | `scene` | select that input; `<code>` is eISCP's own, `2b` for NET |
 * | `mute` | `scene` | toggle mute |
 *
 * ## Why a receiver is a light
 *
 * §5.2 has four kinds and none of them is an amplifier. `light` is the one
 * whose shape fits — a thing that is on or off and has one continuous level —
 * and mapping volume onto `brightness` is what gets the 2×2 tile's real slider
 * instead of a row of blind step buttons. The tile carries `icon: volume-high`
 * so the screen does not claim it is a lamp. A media-player component is
 * §15's, and when it exists this adapter changes its `kind` and nothing else.
 *
 * ## No authentication, and no discovery either
 *
 * eISCP has no credential. A receiver on the LAN answers whoever connects,
 * which is also true of the remote control. Onkyo's own discovery is a UDP
 * broadcast on the same port and is not implemented: the address goes in the
 * binding, as it does for `shelly`, and mDNS is the better answer for both when
 * one of them needs it.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief §3.3's provider id, as written in a configuration binding. */
#define SLATE_ONKYO_PROVIDER_ID "onkyo"

/**
 * @brief Register the provider with the store and the action bus.
 *
 * Call after slate_state_init() and slate_action_init(). Registration is what
 * makes an `onkyo` binding resolvable at all (§3.3), so it happens even when
 * the connection task below cannot start — a tile then shows a missing
 * resource rather than a missing provider, which is the more accurate of the
 * two. The provider registers UNCONFIGURED and stays there until a dashboard
 * binds a receiver to it.
 */
esp_err_t slate_onkyo_init(void);

/**
 * @brief Start the connection task and attach to the Wi-Fi lifecycle.
 *
 * Call after slate_wifi_init(). A station loss marks this provider `offline`,
 * which stales its resources and only its own (§5.2); recovery reconnects with
 * backoff and asks the receiver for everything again.
 */
esp_err_t slate_onkyo_start(void);

#ifdef SLATE_ONKYO_SELFTEST

/**
 * @brief Exercise the id parser, the eISCP framing and the event mapping.
 *
 * Built only with `-DSLATE_ONKYO_SELFTEST=1`. It needs no receiver and no
 * network — it drives the frame reader with byte buffers, including a frame
 * split across two reads, which is the case a stream protocol gets wrong — but
 * it is not free of side effects: it moves the adapter's cached receiver state,
 * installs and releases binding sets, and opens one real socket so that the
 * handover's `close()` is something a check can observe.
 * Call after slate_state_init() and slate_onkyo_init(), and before
 * slate_onkyo_start(). ESP_FAIL if any case failed.
 */
esp_err_t slate_onkyo_selftest(void);

#endif

#ifdef __cplusplus
}
#endif
