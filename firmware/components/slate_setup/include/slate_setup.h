/*
 * Slate — the setup access point and the browser setup portal.
 *
 * DESIGN.md §9 (all of it), §9.2 (the access point), §9.4 (when it is raised),
 * §4.1 (`/wifi/scan`, `POST /wifi`, `DELETE /wifi`), §4.3 (the one place the
 * token rule is relaxed), §6.2 (what the second interface costs), §12.
 *
 *
 * WHAT THIS COMPONENT IS FOR
 *
 * §9's promise: "there is no combination of circumstances in which a powered
 * panel is unreachable, and no failure that is fixed by a USB cable." A panel
 * that cannot associate raises its own network, serves a page over it, and is
 * pointed at a router from a phone. That is the whole of it.
 *
 *
 * WHAT IT OWNS, AND WHAT IT DELIBERATELY DOES NOT
 *
 * It owns the access point: the second interface, the DHCP server, the captive
 * DNS responder, the cached scan, the setup page and the three endpoints §4.3
 * lets the page reach without a token. It is also the only place in the
 * firmware that writes the WiFi mode after `esp_wifi_start()`, which is what
 * slate_wifi.h promises about the station: raising the access point cannot be
 * undone by a reconnect the station happens to be in the middle of.
 *
 * It does not own the station, the retry backoff, or the decision that the
 * panel needs a way back in. Those are slate_wifi's, and they arrive here as
 * SLATE_WIFI_EVENT_SETUP_REQUESTED / SETUP_RELEASED over the default event
 * loop. This component never asks the station to stop, and has no way to: §9.4
 * makes "raising the access point must never stop the station trying" a
 * requirement, and the two components are split along that line so that it
 * cannot be violated by accident from either side.
 *
 *
 * THE THING THAT SURPRISES EVERYONE, ONCE
 *
 * `POST /wifi` cannot report whether it worked, and no amount of engineering
 * makes it able to. There is one radio; the access point must follow the
 * station's channel, so at the instant the station associates the access point
 * moves and drops every client attached to it — including the browser that
 * submitted the form, at the exact moment of success (§9.3). The endpoint
 * answers when the credentials are stored and the panel reports the outcome.
 * Do not add a polling loop to the page for the success path.
 *
 *
 * THREADING
 *
 * slate_setup_init() is called once from app_main, after slate_api_init() (the
 * routes register against its server) and **before** slate_wifi_init(). The
 * order is not a preference: the station posts SETUP_REQUESTED as soon as it
 * finds NVS empty, it posts it once, and an event posted before this component
 * is watching is a factory-fresh panel that never raises an access point at all.
 *
 * Nothing else here is public. Internally the access point is raised and torn
 * down on this component's own task, never on the event loop task: a sweep of
 * the band takes seconds, and blocking the event loop for it would hold up the
 * station's own events — which is the one thing §9.4 says must keep moving.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the setup routes and start watching for §9.4's hand-off.
 *
 * Returns as soon as it is watching. It does not raise the access point:
 * whether the panel needs one is slate_wifi's decision, and on a panel with
 * working credentials the answer is never. On a panel with none, the event is
 * already on its way by the time this returns — slate_wifi posts it as soon as
 * it finds NVS empty — so a cold boot raises the access point without anybody
 * asking for it here.
 *
 * Degrades rather than fails hard, for the reason §9 gives about every other
 * init in this firmware: a component that could not come up is not made to work
 * by rebooting into the same failure, and this is the component that exists for
 * when other things have already gone wrong.
 */
esp_err_t slate_setup_init(void);

#ifdef __cplusplus
}
#endif
