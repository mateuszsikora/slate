/*
 * Slate — internals shared between this component's three source files.
 *
 * Deliberately not in include/: none of it is contract. slate_setup.h is the
 * whole public surface, and it is one function.
 *
 *   slate_setup.c      the access point, the task that raises it, the scan cache
 *   slate_setup_api.c  the setup page and §4.1's three `/wifi` endpoints
 *   slate_setup_dns.c  §9.2's captive DNS responder
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#include "slate_store.h"

/*
 * How many networks the cache keeps. Twenty is not a limit anybody will reach
 * on a domestic band and is well short of what a block of flats produces, which
 * is the case that matters: the page has to be readable, and a list of sixty
 * SSIDs sorted by signal is not more useful than the strongest twenty. The
 * driver's own scan list is what is truncated, so the memory is bounded here
 * rather than by the room.
 */
#define SLATE_SETUP_SCAN_MAX 20

/** One row of `GET /wifi/scan` (§4.1), and one row of the page's list. */
typedef struct {
    char ssid[SLATE_WIFI_SSID_BUF_LEN];
    int8_t rssi;
    uint8_t channel;
    wifi_auth_mode_t auth;
} slate_setup_network_t;

/**
 * @brief Sweep the band and replace the cache, serialised against itself.
 *
 * §9.2 does not scan per request: a sweep makes the access point unresponsive
 * for its duration, which a browser mid-request experiences as the panel having
 * crashed. So this is called once when the access point comes up and again only
 * behind the page's explicit refresh button.
 *
 * ESP_ERR_WIFI_STATE when the driver refuses because the station is in the
 * middle of an association — the caller serves what it already has rather than
 * failing the request, since a stale list plus a manual SSID field is a working
 * page and an error is not.
 */
esp_err_t slate_setup_scan(void);

/**
 * @brief Copy the cache out, newest sweep first in signal order.
 *
 * @param out      array of at least SLATE_SETUP_SCAN_MAX rows
 * @param age_us   how long ago the sweep was taken, or -1 if there has not been
 *                 one — which the page shows rather than pretending to an empty
 *                 room
 * @return how many rows were written
 */
size_t slate_setup_scan_copy(slate_setup_network_t *out, int64_t *age_us);

/** @brief Register `GET /`, `GET /wifi/scan`, `POST /wifi`, `DELETE /wifi`. */
esp_err_t slate_setup_api_init(void);

/**
 * @brief Start answering every DNS query with SLATE_SETUP_AP_ADDRESS.
 *
 * Bound to the access point's address rather than to every interface, which is
 * not a detail: a responder on `0.0.0.0` would answer the household's DNS
 * queries with `192.168.4.1` for as long as the panel is on the LAN.
 */
esp_err_t slate_setup_dns_start(void);

/** @brief Stop the responder and wait for its task to be gone. */
void slate_setup_dns_stop(void);

#ifdef SLATE_SETUP_SELFTEST

/**
 * @brief Exercise the access point's own interface, from the device.
 *
 * Development verifier, called from raise_ap() when built with
 * `-DSLATE_SETUP_SELFTEST=1`. It exists because the one thing about this
 * component that cannot be checked from a desk is the thing most worth checking:
 * §4.3 relaxes the token rule "on the access point interface only", and the
 * check behind that is a getsockname() against 192.168.4.1. A request from
 * another machine cannot reach that interface without a second radio in the
 * room, but a request the panel makes to its own access point address arrives on
 * it — so the exception, and the 401 everything outside it still gets, are
 * verifiable on hardware without a phone in the loop.
 *
 * Logs PASS/FAIL per case and returns ESP_FAIL if any case failed. It never
 * stores credentials: every `POST /wifi` it makes is a refusal.
 */
esp_err_t slate_setup_selftest(void);

#endif
