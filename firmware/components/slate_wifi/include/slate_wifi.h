/*
 * Slate — WiFi station and the §9.4 connection state machine.
 *
 * design.md §9.3 (the failure vocabulary), §9.4 (the state machine), §6.1
 * (stack), §4.1 (`/info.network`, `/status`).
 *
 *
 * WHAT THIS COMPONENT OWNS, AND WHAT IT DELIBERATELY DOES NOT
 *
 * It owns the station: credentials out of NVS, association, the retry backoff,
 * and the decision that the panel needs a way back in. It does not own the
 * setup access point — that is #55 — and it never calls esp_wifi_set_mode()
 * after start. The mode is set once, to WIFI_MODE_STA; when #55 moves it to
 * WIFI_MODE_APSTA nothing here writes it back, so raising the access point
 * cannot be undone by a reconnect this component happens to be in the middle
 * of.
 *
 * The hand-off runs over the default event loop rather than a callback,
 * because two subscribers already exist — #55 raises and tears down the access
 * point, #6 puts the same facts on the screen — and neither should have to
 * know about the other.
 *
 *
 * THE PROPERTY THIS COMPONENT EXISTS TO GUARANTEE
 *
 * §9.4: "raising the access point must never stop the station trying." The
 * retry loop here does not observe SLATE_WIFI_EVENT_SETUP_REQUESTED and has no
 * way to be told to stop; raising the access point is something a subscriber
 * does, not a state this machine enters. A router that comes back at minute
 * seven finds the panel still asking for it, with nobody in the room.
 *
 *
 * THREADING
 *
 * slate_wifi_init() is called once from app_main, after slate_store_init().
 * Everything else here is safe from any task. The state machine runs on its
 * own task; the accessors read a snapshot under the same lock it writes under.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_event.h"

#include "slate_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- The §9.3 failure vocabulary ---------------------------------------- */

/*
 * One vocabulary for the screen and for `/info.network.last_error`, so a
 * report from the panel and a report from the API cannot disagree. The strings
 * are §9.3's table and are contract (§4.1, ADR-4) — slate_wifi_error_str() is
 * the only place they are spelled.
 */
typedef enum {
    SLATE_WIFI_ERR_NONE = 0,   /**< connected, or has not tried yet */
    SLATE_WIFI_ERR_BAD_PASSWORD,
    SLATE_WIFI_ERR_NOT_FOUND,
    SLATE_WIFI_ERR_NO_IP,
    SLATE_WIFI_ERR_AUTH_TIMEOUT,
} slate_wifi_error_t;

/**
 * @brief `bad_password`, `not_found`, `no_ip`, `auth_timeout`, or NULL.
 *
 * NULL rather than `"none"` for SLATE_WIFI_ERR_NONE, because §4.1 serialises
 * that case as JSON `null` and a sentinel string would have to be special-cased
 * back into one at the point where forgetting to is a wrong answer on a page
 * someone is reading to fix their network.
 */
const char *slate_wifi_error_str(slate_wifi_error_t err);

/* --- State -------------------------------------------------------------- */

typedef enum {
    SLATE_WIFI_STATE_STOPPED = 0,  /**< before slate_wifi_init() */
    SLATE_WIFI_STATE_UNCONFIGURED, /**< no credentials — §9.4's first branch */
    SLATE_WIFI_STATE_CONNECTING,   /**< cold boot, inside the three attempts */
    SLATE_WIFI_STATE_CONNECTED,
    SLATE_WIFI_STATE_RETRYING,     /**< lost at runtime, inside the backoff */
    SLATE_WIFI_STATE_FAILED,       /**< the three attempts failed; still retrying */
} slate_wifi_state_t;

/** @brief A short lowercase name for logs and for #6's screen. */
const char *slate_wifi_state_str(slate_wifi_state_t state);

/**
 * @brief A consistent snapshot of the station, for §4.1's `/info` and `/status`.
 *
 * Copied rather than exposed as a pointer: `/status` is polled, the state
 * machine writes these fields from another task, and a serialiser walking a
 * live struct can emit an address that belongs to a previous association next
 * to an SSID that belongs to this one.
 */
typedef struct {
    slate_wifi_state_t state;
    bool connected;                            /**< associated *and* addressed */
    char sta_ssid[SLATE_WIFI_SSID_BUF_LEN];    /**< the configured network, "" if none */
    char ip[16];                               /**< dotted quad, "" when not connected */
    int rssi;                                  /**< dBm; 0 when not connected */
    slate_wifi_error_t last_error;
    unsigned attempts;                         /**< consecutive failures since the last address */
} slate_wifi_status_t;

void slate_wifi_status(slate_wifi_status_t *out);

/* --- Events ------------------------------------------------------------- */

ESP_EVENT_DECLARE_BASE(SLATE_WIFI_EVENT);

/** @brief Why the panel needs a way back in — carried by SETUP_REQUESTED. */
typedef enum {
    SLATE_WIFI_SETUP_NO_CREDENTIALS, /**< cold boot, nothing in NVS */
    SLATE_WIFI_SETUP_CONNECT_FAILED, /**< cold boot, three attempts failed */
    SLATE_WIFI_SETUP_STATION_LOST,   /**< running, and down for five minutes */
} slate_wifi_setup_reason_t;

typedef enum {
    /**
     * The station has an address. Data: slate_wifi_status_t.
     */
    SLATE_WIFI_EVENT_CONNECTED,

    /**
     * The station lost its address or failed to get one. Data:
     * slate_wifi_status_t, with `last_error` set. Fires once per loss, not
     * once per retry — a subscriber that redraws on this must not be woken
     * thirty times while a router reboots.
     */
    SLATE_WIFI_EVENT_DISCONNECTED,

    /**
     * #55: raise the setup access point. Data: slate_wifi_setup_reason_t.
     *
     * The reason matters because §9.4 makes the two cold-boot cases and the
     * runtime case look different on screen: a full-screen setup card for a
     * panel that has never worked, a banner over a dashboard that is still
     * showing yesterday's values for one whose router went away.
     *
     * Fires at most once per outage. The station keeps trying underneath.
     */
    SLATE_WIFI_EVENT_SETUP_REQUESTED,

    /**
     * #55: tear the setup access point down — the station is back. Posted only
     * if SETUP_REQUESTED was, so a subscriber never has to track whether it
     * has an access point up. No data.
     */
    SLATE_WIFI_EVENT_SETUP_RELEASED,
} slate_wifi_event_id_t;

/* --- Lifecycle ---------------------------------------------------------- */

/**
 * @brief Bring the station up and start the §9.4 state machine.
 *
 * Call once from app_main, after slate_store_init() — the store mints the
 * device token from an entropy source that must not be touched once RF is
 * running, and this function is what starts RF (slate_store_set_rf_active()).
 *
 * Creates the default event loop and the station netif if they do not exist,
 * so it composes with whatever #55 and #10 do later. Returns as soon as the
 * radio is started; association happens on the state machine's task, because
 * §9.4's cold-boot branch takes up to three attempts and app_main has a
 * display to bring up.
 *
 * With no credentials in NVS this still starts the radio: #55 needs it up to
 * raise the access point, and `GET /wifi/scan` needs it to sweep.
 */
esp_err_t slate_wifi_init(void);

/**
 * @brief Store credentials and apply them — the primitive under `POST /wifi`.
 *
 * Persists first, then restarts the association sequence from attempt one, so
 * a power cut between the two leaves the panel trying the new network on the
 * next boot rather than the old one forever.
 *
 * Returns as soon as the credentials are stored. It cannot return the outcome
 * of the association and no amount of waiting would let it: §9.3 — the access
 * point moves to the router's channel at the instant the station succeeds and
 * drops the browser that submitted the form. The result reaches the person
 * through the screen and through `/info.network`.
 *
 * `password` may be NULL or empty for an open network.
 */
esp_err_t slate_wifi_connect(const char *ssid, const char *password);

/**
 * @brief Forget the credentials and disconnect — the primitive under
 *        `DELETE /wifi` (§9.5).
 *
 * The state machine lands in SLATE_WIFI_STATE_UNCONFIGURED and posts
 * SETUP_REQUESTED(no_credentials), which is what raises the access point. So
 * this is safe to call over the API from a device that has no other way back:
 * the reply goes out on a connection that is about to die, and the panel comes
 * up on its own network with the address on the screen.
 */
esp_err_t slate_wifi_forget(void);

#ifdef __cplusplus
}
#endif
