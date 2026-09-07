/*
 * Slate — WiFi station. See include/slate_wifi.h for the contract.
 *
 * DESIGN.md §9.3, §9.4, §4.1.
 *
 * The whole of §9.4 is one loop on one task, deliberately. The alternative —
 * event handlers arming esp_timer callbacks that arm each other — spreads a
 * state machine that fits on a page across four callbacks that each see a
 * third of it, and the property this file exists to guarantee ("raising the
 * access point must never stop the station trying") is exactly the kind that
 * dies in the gap between two of them.
 */

#include "slate_wifi.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"

static const char *TAG = "wifi";

ESP_EVENT_DEFINE_BASE(SLATE_WIFI_EVENT);

/* --- §9.4's numbers, in one place --------------------------------------- */

/* "associate, 3 attempts" before the access point goes up on a cold boot. */
#define COLD_BOOT_ATTEMPTS 3

/* "still down after 5 min" for a station lost while running. */
#define STATION_LOST_GRACE_US (5 * 60 * 1000000LL)

/* 1 -> 2 -> 4 -> 8 -> 15 -> 30 s, then 30 s for as long as it takes. Written
 * out rather than computed: the sequence doubles and then stops doubling, and
 * a shift with a clamp reads like an approximation of it. */
static const uint32_t BACKOFF_MS[] = {1000, 2000, 4000, 8000, 15000, 30000};
#define BACKOFF_STEPS (sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0]))

/*
 * Two timeouts esp_wifi does not give us.
 *
 * An association that neither succeeds nor reports a reason is §9.3's
 * `auth_timeout` — "<ssid> did not answer". The driver usually reports a
 * reason within a few seconds, so this is a backstop rather than the normal
 * path.
 *
 * A lease that never arrives is `no_ip`, and it has no event at all:
 * WIFI_EVENT_STA_CONNECTED has already fired and IP_EVENT_STA_GOT_IP simply
 * never does. Without this timer, that state is indistinguishable from a slow
 * router and the panel sits in it forever — associated, addressless and
 * unreachable, which is the one failure §9 says must not exist.
 */
#define ASSOC_TIMEOUT_MS 15000
#define DHCP_TIMEOUT_MS  15000

/*
 * §9.6's trial, in three numbers.
 *
 * TRIAL_MS is the section's "roughly fifteen seconds", and it bounds the whole
 * proof rather than either half of it: a duplicate address is found in the first
 * second or it is not there, and everything after that is the gateway being
 * given every chance to answer before a working configuration is thrown away.
 *
 * The conflict check is three ARP announcements, which is RFC 5227's count. The
 * interval is its lower bound — the RFC's one-to-two seconds is written for a
 * host that is about to claim an address it may keep for weeks, and this one is
 * standing on a configuration a person is watching a panel for.
 */
#define TRIAL_MS            15000
#define TRIAL_CONFLICT_TRIES 3
#define TRIAL_TICK_MS       400

/* How long to wait for the radio to report the disconnect we asked for. See
 * abandon_attempt(). */
#define SETTLE_MS 500

/* --- Task plumbing ------------------------------------------------------ */

typedef enum {
    MSG_ASSOCIATED,   /* WIFI_EVENT_STA_CONNECTED */
    MSG_GOT_IP,       /* IP_EVENT_STA_GOT_IP */
    MSG_DISCONNECTED, /* WIFI_EVENT_STA_DISCONNECTED, `reason` set */
    MSG_APPLY,        /* credentials changed — start over with what is in NVS */
    MSG_FORGET,       /* credentials gone */
} msg_kind_t;

typedef struct {
    msg_kind_t kind;
    uint8_t reason;
} msg_t;

#define MSG_QUEUE_DEPTH 8
#define TASK_STACK      4096
#define TASK_PRIORITY   5

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static esp_netif_t *s_netif;

/* The snapshot slate_wifi_status() copies out. Written by the state machine
 * task and by the IP event handler, read by anything. */
static slate_wifi_state_t s_state = SLATE_WIFI_STATE_STOPPED;
static slate_wifi_error_t s_last_error;
static unsigned s_attempts;
static char s_ip[16];

/* Whether SETUP_REQUESTED is outstanding, and what for, so SETUP_RELEASED is
 * posted exactly once and only to a subscriber that has an access point up to
 * tear down. Task-local in everything but name — only station_task touches
 * them. */
static bool s_setup_requested;
static slate_wifi_setup_reason_t s_setup_reason;

/* --- The vocabulary ----------------------------------------------------- */

const char *slate_wifi_error_str(slate_wifi_error_t err)
{
    switch (err) {
    case SLATE_WIFI_ERR_BAD_PASSWORD: return "bad_password";
    case SLATE_WIFI_ERR_NOT_FOUND:    return "not_found";
    case SLATE_WIFI_ERR_NO_IP:        return "no_ip";
    case SLATE_WIFI_ERR_AUTH_TIMEOUT: return "auth_timeout";
    case SLATE_WIFI_ERR_GATEWAY_UNREACHABLE: return "gateway_unreachable";
    case SLATE_WIFI_ERR_ADDRESS_IN_USE:      return "address_in_use";
    case SLATE_WIFI_ERR_NONE:         break;
    }
    return NULL;
}

const char *slate_wifi_state_str(slate_wifi_state_t state)
{
    switch (state) {
    case SLATE_WIFI_STATE_STOPPED:      return "stopped";
    case SLATE_WIFI_STATE_UNCONFIGURED: return "unconfigured";
    case SLATE_WIFI_STATE_CONNECTING:   return "connecting";
    case SLATE_WIFI_STATE_CONNECTED:    return "connected";
    case SLATE_WIFI_STATE_RETRYING:     return "retrying";
    case SLATE_WIFI_STATE_FAILED:       return "failed";
    }
    return "unknown";
}

/*
 * esp_wifi's reason codes collapsed onto §9.3's four strings.
 *
 * There are seventy of them and four things a person standing in front of the
 * panel can do about any of them: retype the password, check the name, look at
 * the router's DHCP, or wait. That is why most of this mapping is the default
 * — a fifth bucket for WIFI_REASON_ASSOC_TOOMANY would be honest and useless.
 */
static slate_wifi_error_t error_from_reason(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return SLATE_WIFI_ERR_NOT_FOUND;

    /* Everything that means "the handshake did not complete". A wrong
     * passphrase surfaces as any of these depending on the router, so they
     * have to agree: a panel that prints `auth_timeout` for a typo sends
     * somebody to reboot a router that is working perfectly. */
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return SLATE_WIFI_ERR_BAD_PASSWORD;

    default:
        return SLATE_WIFI_ERR_AUTH_TIMEOUT;
    }
}

/* --- Snapshot ----------------------------------------------------------- */

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void set_state(slate_wifi_state_t state)
{
    LOCK();
    s_state = state;
    UNLOCK();
}

static void set_error(slate_wifi_error_t err)
{
    LOCK();
    s_last_error = err;
    UNLOCK();
}

void slate_wifi_status(slate_wifi_status_t *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    if (s_lock == NULL) {
        out->state = SLATE_WIFI_STATE_STOPPED;
        return;
    }

    LOCK();
    out->state = s_state;
    out->connected = s_state == SLATE_WIFI_STATE_CONNECTED;
    out->last_error = s_last_error;
    out->attempts = s_attempts;
    strlcpy(out->ip, s_ip, sizeof(out->ip));
    UNLOCK();

    /* Read through rather than cached: the configured network is the store's
     * fact, and a copy here is a second one to keep in step with
     * `POST /wifi`. */
    if (slate_store_wifi_ssid_get(out->sta_ssid, sizeof(out->sta_ssid)) != ESP_OK) {
        out->sta_ssid[0] = '\0';
    }

    /*
     * §4.1's "reported rather than echoed", asked of the thing that actually
     * knows: the DHCP client is running or it is not, and a static address is
     * precisely the case where it is not. Read here rather than tracked in a
     * field, so the answer cannot drift from the interface during the window
     * §9.6's revert opens between the two.
     */
    esp_netif_dhcp_status_t dhcp = ESP_NETIF_DHCP_INIT;
    out->ipv4_static = s_netif != NULL &&
                       esp_netif_dhcpc_get_status(s_netif, &dhcp) == ESP_OK &&
                       dhcp == ESP_NETIF_DHCP_STOPPED;

    /* Only meaningful while associated, and asking the driver for it while it
     * is not costs an error log on every poll of `/status`. */
    if (out->connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
        }
    }
}

/* --- Events out --------------------------------------------------------- */

static void post_event(slate_wifi_event_id_t id, const void *data, size_t len)
{
    esp_err_t err = esp_event_post(SLATE_WIFI_EVENT, id, (void *) data, len, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "posting event %d: %s", (int) id, esp_err_to_name(err));
    }
}

static void post_status(slate_wifi_event_id_t id)
{
    slate_wifi_status_t status;
    slate_wifi_status(&status);
    post_event(id, &status, sizeof(status));
}

static void request_setup(slate_wifi_setup_reason_t reason)
{
    /* Re-posted when the reason changes even though the access point is
     * already up: #6 prints the reason, and a panel whose credentials were
     * just deleted must not keep showing "could not connect to <ssid>" for a
     * network it no longer knows about. */
    if (s_setup_requested && s_setup_reason == reason) {
        return;
    }
    s_setup_requested = true;
    s_setup_reason = reason;

    ESP_LOGW(TAG, "requesting the setup access point, reason %d", (int) reason);
    post_event(SLATE_WIFI_EVENT_SETUP_REQUESTED, &reason, sizeof(reason));
}

static void release_setup(void)
{
    if (!s_setup_requested) {
        return;
    }
    s_setup_requested = false;

    ESP_LOGI(TAG, "releasing the setup access point");
    post_event(SLATE_WIFI_EVENT_SETUP_RELEASED, NULL, 0);
}

/* --- Events in ---------------------------------------------------------- */

static void send_msg(msg_kind_t kind, uint8_t reason)
{
    msg_t msg = {.kind = kind, .reason = reason};

    /* Never blocks. This runs on the event loop task, and a full queue means
     * the state machine is already wedged — blocking here would take the event
     * loop, and with it every other subscriber, down alongside it. */
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGE(TAG, "state machine queue full — dropped message %d", (int) kind);
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;

    switch (id) {
    case WIFI_EVENT_STA_CONNECTED:
        send_msg(MSG_ASSOCIATED, 0);
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = data;
        send_msg(MSG_DISCONNECTED, ev->reason);
        break;
    }

    default:
        break;
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;

    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const ip_event_got_ip_t *ev = data;

    LOCK();
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    UNLOCK();

    send_msg(MSG_GOT_IP, 0);
}

/* --- §9.6: the address, and the interface it goes on -------------------- */

/** A prefix length as the mask lwIP wants, in network byte order. */
static uint32_t netmask_from_prefix(unsigned prefix)
{
    return esp_netif_htonl(0xFFFFFFFFu << (32 - prefix));
}

/**
 * Clear every resolver on the station.
 *
 * lwIP keeps whatever was set until something overwrites that exact slot, so a
 * configuration naming one server leaves the second and third from whatever the
 * panel was on before — a resolver belonging to a network that is no longer
 * within reach. The DHCP path clears them for free (esp_netif_dhcpc_start()
 * does it), so this is only ever the static one.
 */
static esp_err_t clear_dns(void *ctx)
{
    (void) ctx;
    for (u8_t slot = 0; slot < DNS_MAX_SERVERS; slot++) {
        dns_setserver(slot, NULL);
    }
    return ESP_OK;
}

/** Put the station back on DHCP. Idempotent, and the state every panel boots in. */
static esp_err_t apply_dhcp(void)
{
    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }

    /* Zeroed before the client starts, because §9.6's revert runs this on a
     * live association: without it the DISCOVER would go out from a static
     * address that has just been shown to belong to somebody else. */
    const esp_netif_ip_info_t none = {0};
    err = esp_netif_set_ip_info(s_netif, &none);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_dhcpc_start(s_netif);
    if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        err = ESP_OK;
    }

    /*
     * After the client has been started, and not instead of what it does
     * itself: esp_netif_dhcpc_start() clears the resolvers with
     * dns_clear_servers(true), and the `true` is "keep the fallback" — which is
     * the exact slot apply_static() puts a third static server in. Without this
     * line a reverted configuration's third resolver outlives it.
     */
    esp_netif_tcpip_exec(clear_dns, NULL);
    return err;
}

/** Put the stored static configuration on the station. */
static esp_err_t apply_static(const slate_ipv4_config_t *cfg)
{
    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }

    const esp_netif_ip_info_t info = {
        .ip.addr = esp_netif_htonl(cfg->address),
        .netmask.addr = netmask_from_prefix(cfg->prefix),
        .gw.addr = esp_netif_htonl(cfg->gateway),
    };
    err = esp_netif_set_ip_info(s_netif, &info);
    if (err != ESP_OK) {
        return err;
    }

    esp_netif_tcpip_exec(clear_dns, NULL);

    static const esp_netif_dns_type_t SLOTS[SLATE_IPV4_DNS_MAX] = {
        ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP, ESP_NETIF_DNS_FALLBACK};
    for (unsigned i = 0; i < cfg->dns_count && i < SLATE_IPV4_DNS_MAX; i++) {
        esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4};
        dns.ip.u_addr.ip4.addr = esp_netif_htonl(cfg->dns[i]);

        esp_err_t dns_err = esp_netif_set_dns_info(s_netif, SLOTS[i], &dns);
        if (dns_err != ESP_OK) {
            /* Not fatal, and deliberately so: a panel that has an address and
             * no resolver still answers `GET /info` at the number on the
             * screen, which is where the person fixing it is looking. */
            ESP_LOGW(TAG, "DNS server %u: %s", i + 1, esp_err_to_name(dns_err));
        }
    }
    return ESP_OK;
}

/**
 * Set up the interface for this attempt, and report what it ended up being.
 *
 * Read out of NVS every time for the reason the credentials are: this is the
 * path `POST /wifi` changes, and a cached copy is the bug where a corrected
 * address is stored, acknowledged and then not used.
 */
static void apply_addressing(slate_ipv4_config_t *out)
{
    slate_store_ipv4_get(out);

    esp_err_t err;
    if (slate_ipv4_state_is_applied(out->state)) {
        err = apply_static(out);
        if (err == ESP_OK) {
            return;
        }
        /* The interface refused the address, so there is nothing to prove and
         * nothing to prove it with. Falling back rather than failing the
         * attempt keeps §9's promise about a reachable panel; the stored
         * configuration is untouched, so a fixed firmware applies it again. */
        ESP_LOGE(TAG, "applying the static address: %s — falling back to DHCP",
                 esp_err_to_name(err));
        out->state = SLATE_IPV4_DHCP;
    }

    err = apply_dhcp();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "returning the station to DHCP: %s", esp_err_to_name(err));
    }
}

/* --- §9.6: proving the address before it is kept ------------------------ */

/*
 * ARP rather than ICMP, because gateways that drop pings are common enough that
 * pinging one would revert configurations that work (§9.6). It answers the
 * second question for free: a host already holding the address replies from a
 * MAC that is not ours, which is duplicate address detection with no extra
 * machinery.
 *
 * One deviation from RFC 5227, and it is forced. The RFC probes with a sender
 * address of 0.0.0.0 *before* claiming the address; lwIP will not do that from
 * the public API — etharp_request() always sends the interface's own address as
 * the sender, and etharp_input() declines to cache anything at all while the
 * interface has none, so a reply to such a probe is invisible from here. What is
 * implemented instead is §2.4's ongoing detection: claim the address, announce
 * it, and watch for somebody answering for it. The cost is that the panel holds
 * a possibly-duplicate address for the second the check takes, which is the same
 * exposure a host defending its address already has and is bounded by a revert
 * that is already written.
 */

typedef struct {
    struct netif *netif;
    ip4_addr_t target;
    struct eth_addr mac; /* out */
    bool found;          /* out */
} arp_ctx_t;

static esp_err_t arp_forget_all(void *ctx)
{
    /* So the answer comes from this association rather than from a table entry
     * left over from the network the panel was on before it. */
    etharp_cleanup_netif(((arp_ctx_t *) ctx)->netif);
    return ESP_OK;
}

static esp_err_t arp_ask(void *ctx)
{
    arp_ctx_t *arp = ctx;
    return etharp_request(arp->netif, &arp->target) == ERR_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t arp_answer(void *ctx)
{
    arp_ctx_t *arp = ctx;
    struct eth_addr *mac = NULL;
    const ip4_addr_t *ip = NULL;

    /* etharp_find_addr() reports stable entries only, so a pending one — a
     * request sent and nothing back yet — is correctly not an answer. */
    arp->found = etharp_find_addr(arp->netif, &arp->target, &mac, &ip) >= 0 && mac != NULL;
    if (arp->found) {
        arp->mac = *mac;
    }
    return ESP_OK;
}

typedef enum {
    TRIAL_CONFIRMED,
    TRIAL_ADDRESS_IN_USE,
    TRIAL_GATEWAY_UNREACHABLE,
    TRIAL_INTERRUPTED, /* something outranking the trial arrived; see the msg */
} trial_t;

/**
 * Wait, unless a message arrives that the trial is not allowed to sit on.
 *
 * A lost association or a change of credentials both make the question the
 * trial is asking irrelevant, and both have to reach the state machine rather
 * than expire in here.
 *
 * @return true if `msg` was filled and the trial must stop.
 */
static bool trial_wait(uint32_t ms, msg_t *msg)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t) (deadline - now) <= 0) {
            return false;
        }
        if (xQueueReceive(s_queue, msg, deadline - now) != pdTRUE) {
            continue;
        }
        /* The address arriving again is this trial's own subject, not news. */
        if (msg->kind == MSG_GOT_IP || msg->kind == MSG_ASSOCIATED) {
            continue;
        }
        return true;
    }
}

/** §9.6's proof: nobody else holds the address, and the gateway answers. */
static trial_t run_trial(const slate_ipv4_config_t *cfg, msg_t *interrupt)
{
    arp_ctx_t arp = {.netif = s_netif != NULL ? esp_netif_get_netif_impl(s_netif) : NULL};
    uint8_t own_mac[ETH_HWADDR_LEN] = {0};

    if (arp.netif == NULL || esp_wifi_get_mac(WIFI_IF_STA, own_mac) != ESP_OK) {
        /* Nothing to ask with. Reverting rather than confirming, because §9.6
         * keeps an address only when it has answered, and an unproven one is
         * the failure the whole section is about. */
        ESP_LOGE(TAG, "no interface to prove the static address on");
        return TRIAL_GATEWAY_UNREACHABLE;
    }

    esp_netif_tcpip_exec(arp_forget_all, &arp);
    int64_t deadline = esp_timer_get_time() + (int64_t) TRIAL_MS * 1000;

    /* Is the address already somebody's? An announcement for our own address is
     * a question its real owner has to answer. */
    arp.target.addr = esp_netif_htonl(cfg->address);
    for (unsigned try = 0; try < TRIAL_CONFLICT_TRIES; try++) {
        esp_netif_tcpip_exec(arp_ask, &arp);
        if (trial_wait(TRIAL_TICK_MS, interrupt)) {
            return TRIAL_INTERRUPTED;
        }
        esp_netif_tcpip_exec(arp_answer, &arp);

        if (arp.found && memcmp(arp.mac.addr, own_mac, ETH_HWADDR_LEN) != 0) {
            ESP_LOGE(TAG, "the address is answered by %02x:%02x:%02x:%02x:%02x:%02x",
                     arp.mac.addr[0], arp.mac.addr[1], arp.mac.addr[2], arp.mac.addr[3],
                     arp.mac.addr[4], arp.mac.addr[5]);
            return TRIAL_ADDRESS_IN_USE;
        }
    }

    /* And does the next hop exist? This is the one a typo lands on, and the
     * whole of the remaining budget goes to it: a gateway that answers slowly
     * is a working configuration, and throwing one away is the more expensive
     * of the two mistakes available here. */
    arp.target.addr = esp_netif_htonl(cfg->gateway);
    while (esp_timer_get_time() < deadline) {
        esp_netif_tcpip_exec(arp_ask, &arp);
        if (trial_wait(TRIAL_TICK_MS, interrupt)) {
            return TRIAL_INTERRUPTED;
        }
        esp_netif_tcpip_exec(arp_answer, &arp);

        if (arp.found) {
            return TRIAL_CONFIRMED;
        }
    }
    return TRIAL_GATEWAY_UNREACHABLE;
}

/* --- The state machine -------------------------------------------------- */

typedef enum {
    OUTCOME_CONNECTED,
    OUTCOME_FAILED,
    OUTCOME_RESTART, /* the credentials changed under us */
} outcome_t;

/** True when a message means "start over with what is in NVS now". */
static bool is_restart(const msg_t *msg)
{
    return msg->kind == MSG_APPLY || msg->kind == MSG_FORGET;
}

/**
 * Give up on the current attempt and wait for the radio to say it has.
 *
 * esp_wifi_disconnect() is asynchronous. The WIFI_EVENT_STA_DISCONNECTED it
 * produces lands on the queue after the caller has already decided this
 * attempt failed, and read during the *next* attempt it looks like that
 * attempt failing instantly — which turns the backoff into a spin and reports
 * the previous failure's reason as the new one. Waiting here, once, is
 * cheaper than a staleness filter every reader has to repeat.
 *
 * An attempt that never associated produces no event at all, hence the bound.
 * It is only paid on the timeout paths, which are the rare ones — a failure
 * the driver reports arrives as MSG_DISCONNECTED and never comes through here.
 *
 * @return true if the credentials changed while waiting.
 */
static bool abandon_attempt(void)
{
    esp_wifi_disconnect();

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SETTLE_MS);
    bool restart = false;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = (int32_t) (deadline - now) > 0 ? deadline - now : 0;

        msg_t msg;
        if (xQueueReceive(s_queue, &msg, wait) != pdTRUE) {
            return restart;
        }
        if (is_restart(&msg)) {
            restart = true;
        } else if (msg.kind == MSG_DISCONNECTED) {
            return restart;
        }
    }
}

/**
 * Is somebody attached to the setup access point?
 *
 * Asked of the driver on every check rather than counted from
 * WIFI_EVENT_AP_STACONNECTED and AP_STADISCONNECTED, because the two are not
 * symmetrical: taking the access point down (#55's release_ap(), which is the
 * normal way it ends) removes the interface without a disconnect per client, and
 * a count that has drifted upwards is a panel holding its retry at the ceiling
 * for a room nobody is in. This answer cannot drift — it is the driver's own
 * association table, and it is empty whenever there is no access point to be on.
 */
static bool setup_client_attached(void)
{
    /* Nobody can be attached to an access point that was never asked for, and
     * this is the common case: every panel retrying an outage before §9.4's five
     * minutes are up runs the loop below with no access point at all. */
    if (!s_setup_requested) {
        return false;
    }

    wifi_sta_list_t clients;
    return esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0;
}

/**
 * Wait out a delay, returning early if the credentials change.
 *
 * The early return is what makes `POST /wifi` feel immediate on a panel that
 * is thirty seconds into a backoff. Without it a corrected password sits
 * unused until a timer that was counting for the wrong one expires.
 *
 * The other half is §9.4's, and it is why this waits in slices instead of once.
 * An attempt takes the radio off the access point's channel for the couple of
 * seconds it lasts, and the early steps of the backoff spend that couple of
 * seconds every one, two, four — which is a person watching a setup page that
 * will not load while the panel tries credentials they are standing there to
 * replace. While anybody is attached, the wait is held at the backoff's own
 * ceiling: the thirty seconds an unattended panel settles at anyway, inside
 * §9.4's "at most a minute apart", with the sequence itself untouched.
 *
 * What it deliberately does not do is stop. §9.4 makes "raising the access point
 * must never stop the station trying" a requirement, so this changes the spacing
 * of attempts and never whether one happens — a panel whose network comes back
 * while somebody is on the setup page still rejoins it within the ceiling.
 */
static bool sleep_or_restart(uint32_t ms)
{
    /* Long enough that this is not a busy loop, short enough that a client
     * attaching extends the wait it is already inside, and that the last one
     * leaving returns the panel to its own cadence promptly. */
    const TickType_t SLICE = pdMS_TO_TICKS(1000);

    const uint32_t ceiling_ms = BACKOFF_MS[BACKOFF_STEPS - 1];
    const TickType_t ceiling = pdMS_TO_TICKS(ceiling_ms);

    /* At the ceiling there is nothing a client could add, so that wait — the one
     * every long outage settles into — is taken in a single sleep, exactly as it
     * was before any of this. Only the short steps are asked to look up. */
    const bool holdable = pdMS_TO_TICKS(ms) < ceiling;

    TickType_t began = xTaskGetTickCount();
    bool held = false;

    for (;;) {
        TickType_t wait_for = pdMS_TO_TICKS(ms);

        if (holdable && setup_client_attached()) {
            wait_for = ceiling;
            if (!held) {
                held = true;
                ESP_LOGI(TAG, "somebody is on the setup access point — next attempt in %" PRIu32
                              " ms rather than %" PRIu32 " ms",
                         ceiling_ms, ms);
            }
        }

        int32_t left = (int32_t) (began + wait_for - xTaskGetTickCount());
        if (left <= 0) {
            return false;
        }
        if (holdable && left > (int32_t) SLICE) {
            left = (int32_t) SLICE;
        }

        msg_t msg;
        if (xQueueReceive(s_queue, &msg, (TickType_t) left) == pdTRUE && is_restart(&msg)) {
            return true;
        }
    }
}

/** One association attempt, from esp_wifi_connect() to a verdict. */
static outcome_t attempt_connect(void)
{
    wifi_config_t cfg = {0};
    char ssid[SLATE_WIFI_SSID_BUF_LEN] = {0};
    char password[SLATE_WIFI_PASSWORD_BUF_LEN] = {0};

    /* Read every time rather than once at start: this is the path
     * `POST /wifi` changes, and a cached copy is the bug where a corrected
     * password is stored, acknowledged and then not used. */
    esp_err_t err = slate_store_wifi_ssid_get(ssid, sizeof(ssid));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading the station SSID: %s", esp_err_to_name(err));
        set_error(SLATE_WIFI_ERR_NOT_FOUND);
        return OUTCOME_FAILED;
    }

    /* A 32-character SSID fills esp_wifi's field exactly and is not
     * NUL-terminated there, which is why it is read into a buffer one byte
     * larger and copied rather than read in place. */
    memcpy(cfg.sta.ssid, ssid, strnlen(ssid, sizeof(cfg.sta.ssid)));

    if (slate_store_wifi_password_get(password, sizeof(password)) == ESP_OK) {
        memcpy(cfg.sta.password, password, strnlen(password, sizeof(cfg.sta.password)));
    }

    /*
     * The authmode threshold stays at the driver's default, so an open network
     * still associates. Raising it to WPA2 would report §9.3's `not_found` for
     * a network the setup page has just listed by name, which is the worst of
     * both: a true statement about the threshold and a lie about the room.
     */
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);

    /* Both copies off the stack. §12 keeps the passphrase out of the API; a
     * task stack is where the core dump of §11.3 would find it and put it in a
     * file somebody attaches to an issue. */
    memset(password, 0, sizeof(password));
    memset(cfg.sta.password, 0, sizeof(cfg.sta.password));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        set_error(SLATE_WIFI_ERR_AUTH_TIMEOUT);
        return OUTCOME_FAILED;
    }

    /*
     * The address goes on before the association, not after. §9.6's trial is
     * about a static address behaving on the network, and it can only behave on
     * one it was brought up with: esp_netif posts IP_EVENT_STA_GOT_IP for a
     * static configuration from the connected action, and only if the address
     * is already on the interface by then.
     */
    slate_ipv4_config_t ipv4;
    apply_addressing(&ipv4);

    ESP_LOGI(TAG, "attempt %u: associating with \"%s\", addressing %s", s_attempts + 1, ssid,
             slate_ipv4_state_str(ipv4.state));

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        set_error(SLATE_WIFI_ERR_AUTH_TIMEOUT);
        return OUTCOME_FAILED;
    }

    bool associated = false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ASSOC_TIMEOUT_MS);

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = (int32_t) (deadline - now) > 0 ? deadline - now : 0;

        msg_t msg;
        if (xQueueReceive(s_queue, &msg, wait) != pdTRUE) {
            /* Associated but never addressed is `no_ip`; never associated and
             * never told why is `auth_timeout`. */
            set_error(associated ? SLATE_WIFI_ERR_NO_IP : SLATE_WIFI_ERR_AUTH_TIMEOUT);
            ESP_LOGW(TAG, "attempt timed out — %s", slate_wifi_error_str(
                                                        associated ? SLATE_WIFI_ERR_NO_IP
                                                                   : SLATE_WIFI_ERR_AUTH_TIMEOUT));
            return abandon_attempt() ? OUTCOME_RESTART : OUTCOME_FAILED;
        }

        /*
         * §9.6, and this is the whole of it: an address that was just submitted
         * has to answer for itself before it is kept. Only a pending one — the
         * association after `POST /wifi` — is ever on trial. A confirmed
         * configuration is sticky, because a router that is down at some later
         * boot is an ordinary retry and discarding a working address over it
         * would be a worse bug than the one this proves against.
         *
         * `associated` is part of the condition rather than an assertion about
         * it. esp_netif posts IP_EVENT_STA_GOT_IP from esp_netif_set_ip_info()
         * whenever the interface happens to still be up, so apply_addressing()
         * can queue one before esp_wifi_connect() is even called — and ARPing
         * for fifteen seconds on a link that is going down would condemn a
         * configuration that was never given a network to prove itself on.
         */
        if (msg.kind == MSG_GOT_IP && associated && ipv4.state == SLATE_IPV4_STATIC_PENDING) {
            msg_t interrupt;
            trial_t verdict = run_trial(&ipv4, &interrupt);

            if (verdict == TRIAL_CONFIRMED) {
                esp_err_t store_err =
                    slate_store_ipv4_set_state(&ipv4, SLATE_IPV4_STATIC_CONFIRMED);
                ipv4.state = SLATE_IPV4_STATIC_CONFIRMED;
                if (store_err != ESP_OK) {
                    /* The address works; it is the record of that which did
                     * not get written. Trying again next boot costs one more
                     * trial, which is fifteen seconds, so this is not worth
                     * refusing a working network over. */
                    ESP_LOGE(TAG, "confirming the static address: %s",
                             esp_err_to_name(store_err));
                }
                ESP_LOGI(TAG, "the static address answered — keeping it");
                return OUTCOME_CONNECTED;
            }

            if (verdict != TRIAL_INTERRUPTED) {
                bool in_use = verdict == TRIAL_ADDRESS_IN_USE;
                slate_ipv4_state_t failed = in_use ? SLATE_IPV4_STATIC_ADDRESS_IN_USE
                                                   : SLATE_IPV4_STATIC_GATEWAY_UNREACHABLE;
                slate_wifi_error_t why = in_use ? SLATE_WIFI_ERR_ADDRESS_IN_USE
                                                : SLATE_WIFI_ERR_GATEWAY_UNREACHABLE;
                set_error(why);

                /* Kept and marked, not erased: the setup page pre-fills the
                 * form from it, and the recovery §9.6 describes is somebody
                 * correcting one field of what they already typed. */
                esp_err_t store_err = slate_store_ipv4_set_state(&ipv4, failed);
                if (store_err != ESP_OK) {
                    /* The record still says pending, so the next attempt will
                     * put this address back and spend another fifteen seconds
                     * proving it wrong again. Worth a line, because from the
                     * outside that looks like a panel that is slow to boot. */
                    ESP_LOGE(TAG, "marking the static address failed: %s",
                             esp_err_to_name(store_err));
                }
                ipv4.state = failed;

                ESP_LOGE(TAG, "the static address did not prove out (%s) — reverting to DHCP",
                         slate_wifi_error_str(why));

                LOCK();
                s_ip[0] = '\0';
                UNLOCK();

                /*
                 * On the live association rather than by disconnecting. Layer 2
                 * succeeded — the passphrase was right and the network is in
                 * range — and only the address was wrong, so this is a DHCP
                 * client starting on a working link. A lease that does not
                 * arrive lands on the timeout above as `no_ip`, which is §9.6's
                 * "if DHCP does not answer either" and needs no new state.
                 */
                if (apply_dhcp() != ESP_OK) {
                    return abandon_attempt() ? OUTCOME_RESTART : OUTCOME_FAILED;
                }
                deadline = xTaskGetTickCount() + pdMS_TO_TICKS(DHCP_TIMEOUT_MS);
                continue;
            }

            /* Something that outranks the trial arrived. It is handled below
             * exactly as it would have been had the trial not been running. */
            msg = interrupt;
        }

        if (is_restart(&msg)) {
            abandon_attempt();
            return OUTCOME_RESTART;
        }

        switch (msg.kind) {
        case MSG_ASSOCIATED:
            associated = true;
            deadline = xTaskGetTickCount() + pdMS_TO_TICKS(DHCP_TIMEOUT_MS);
            break;

        case MSG_GOT_IP:
            /* esp_netif_set_ip_info() can post this while a previous link is
             * still settling. It is not proof that the association started by
             * this attempt has an address, and for a pending static address it
             * must never bypass the ARP trial above. The connected event is
             * queued first on a real association, so a current GOT_IP always
             * arrives with `associated` already true. */
            if (associated) {
                return OUTCOME_CONNECTED;
            }
            break;

        case MSG_DISCONNECTED: {
            slate_wifi_error_t reason = error_from_reason(msg.reason);
            set_error(reason);
            ESP_LOGW(TAG, "disconnected, reason %u — %s", msg.reason,
                     slate_wifi_error_str(reason));
            return OUTCOME_FAILED;
        }

        default:
            break;
        }
    }
}

/**
 * Sit in SLATE_WIFI_STATE_CONNECTED until the address goes away.
 *
 * @return true if the credentials changed rather than the network going down —
 *         the caller must not treat that as the outage §9.4 counts five
 *         minutes from.
 */
static bool wait_for_loss(void)
{
    for (;;) {
        msg_t msg;
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (is_restart(&msg)) {
            abandon_attempt();
            return true;
        }
        if (msg.kind == MSG_DISCONNECTED) {
            slate_wifi_error_t reason = error_from_reason(msg.reason);
            set_error(reason);
            ESP_LOGW(TAG, "station lost, reason %u — %s", msg.reason,
                     slate_wifi_error_str(reason));
            return false;
        }
    }
}

/** Back to "nothing has worked yet", after a change of credentials. */
static void reset_attempts(void)
{
    LOCK();
    s_attempts = 0;
    s_last_error = SLATE_WIFI_ERR_NONE;
    s_ip[0] = '\0';
    UNLOCK();
}

static void station_task(void *arg)
{
    (void) arg;

    /*
     * §9.4's two branches differ only in whether this network has ever worked,
     * so that is the only thing carried across iterations. `lost_at` is
     * meaningless until it has.
     */
    bool ever_connected = false;
    int64_t lost_at = 0;

    for (;;) {
        if (!slate_store_wifi_is_configured()) {
            ESP_LOGI(TAG, "no credentials — setup access point");
            reset_attempts();

            /*
             * `DELETE /wifi` took the addressing with the credentials (§9.5), so
             * the interface must stop holding an address that belonged to a
             * network the panel has been told to forget. Without this the DHCP
             * client stays stopped for the whole setup session and
             * `/info.network.ipv4` reports `static` against a `static` object of
             * `null` — a shape §4.1 does not have.
             */
            apply_dhcp();
            set_state(SLATE_WIFI_STATE_UNCONFIGURED);
            request_setup(SLATE_WIFI_SETUP_NO_CREDENTIALS);
            ever_connected = false;

            /* Nothing to retry. The radio stays up regardless: #55 raises the
             * access point on it and `GET /wifi/scan` sweeps with it. */
            msg_t msg;
            while (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE || !is_restart(&msg)) {
            }
            continue;
        }

        set_state(ever_connected ? SLATE_WIFI_STATE_RETRYING : SLATE_WIFI_STATE_CONNECTING);

        outcome_t outcome = attempt_connect();

        if (outcome == OUTCOME_RESTART) {
            reset_attempts();
            ever_connected = false;
            continue;
        }

        if (outcome == OUTCOME_CONNECTED) {
            LOCK();
            s_attempts = 0;
            /*
             * §9.6's two errors survive the connection they caused; every other
             * one is cleared by it. The difference is what the word means. The
             * other four say "the station is not connected", so being connected
             * answers them. These two say "the address you typed was thrown
             * away", and that stays true on a panel which is connected — over
             * DHCP, at an address nobody asked for — until somebody submits
             * something else. Clearing them here would leave §9.3's table with
             * two rows nothing can ever put in it.
             */
            if (s_last_error != SLATE_WIFI_ERR_GATEWAY_UNREACHABLE &&
                s_last_error != SLATE_WIFI_ERR_ADDRESS_IN_USE) {
                s_last_error = SLATE_WIFI_ERR_NONE;
            }
            s_state = SLATE_WIFI_STATE_CONNECTED;
            UNLOCK();

            ever_connected = true;

            slate_wifi_status_t status;
            slate_wifi_status(&status);
            ESP_LOGI(TAG, "connected to \"%s\", address %s, rssi %d dBm", status.sta_ssid,
                     status.ip, status.rssi);
            post_event(SLATE_WIFI_EVENT_CONNECTED, &status, sizeof(status));

            /* Torn down "without ceremony" the moment the station is back
             * (§9.4) — after the connected event, so a subscriber redrawing on
             * both never paints a setup card over a working dashboard. */
            release_setup();

            bool restarted = wait_for_loss();

            LOCK();
            s_ip[0] = '\0';
            UNLOCK();

            if (restarted) {
                reset_attempts();
                ever_connected = false;
                continue;
            }

            set_state(SLATE_WIFI_STATE_RETRYING);
            lost_at = esp_timer_get_time();
            post_status(SLATE_WIFI_EVENT_DISCONNECTED);
            continue;
        }

        /* Failed. */
        LOCK();
        s_attempts++;
        unsigned attempts = s_attempts;
        UNLOCK();

        if (!ever_connected) {
            /* Cold boot: three attempts, then the access point — with the
             * credentials kept, so a corrected password is one field (§9.3). */
            if (attempts == COLD_BOOT_ATTEMPTS) {
                set_state(SLATE_WIFI_STATE_FAILED);
                post_status(SLATE_WIFI_EVENT_DISCONNECTED);
                request_setup(SLATE_WIFI_SETUP_CONNECT_FAILED);
            }
        } else if (esp_timer_get_time() - lost_at >= STATION_LOST_GRACE_US) {
            /* Lost while running: the access point appears alongside the
             * dashboard, not instead of it. */
            request_setup(SLATE_WIFI_SETUP_STATION_LOST);
        }

        size_t step = attempts - 1 < BACKOFF_STEPS ? attempts - 1 : BACKOFF_STEPS - 1;
        ESP_LOGI(TAG, "retrying in %" PRIu32 " ms", BACKOFF_MS[step]);

        if (sleep_or_restart(BACKOFF_MS[step])) {
            reset_attempts();
            ever_connected = false;
        }
    }
}

/* --- Lifecycle ---------------------------------------------------------- */

esp_err_t slate_wifi_init(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(MSG_QUEUE_DEPTH, sizeof(msg_t));
    if (s_lock == NULL || s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }

    /* Created here rather than in app_main, so this component composes with
     * #10 and #55 whichever of the three comes up first. */
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* §4.3 wants one panel called one thing everywhere, and the DHCP lease is
     * the first place that name appears — in the router's client list, before
     * mDNS or the editor URL QR exist. */
    err = esp_netif_set_hostname(s_netif, slate_store_device_name());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "setting the hostname: %s", esp_err_to_name(err));
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * The driver keeps its own copy of whatever esp_wifi_set_config() is given
     * in `nvs.net80211` unless told not to. §12 says the passphrase lives in
     * NVS and never leaves; a second copy in a namespace this project does not
     * own is one slate_store_wifi_clear() does not erase, which makes
     * `DELETE /wifi` a promise the device does not keep.
     */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL,
                                              NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event, NULL,
                                                  NULL);
    }
    if (err != ESP_OK) {
        return err;
    }

    /* Set once, and never again — see the header. #55 moves this to APSTA. */
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    /* The store's entropy source depends on knowing this and cannot infer it. */
    slate_store_set_rf_active(true);

    if (xTaskCreate(station_task, "slate_wifi", TASK_STACK, NULL, TASK_PRIORITY, &s_task) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t slate_wifi_connect(const char *ssid, const char *password,
                             const slate_ipv4_config_t *ipv4)
{
    /* Pending whatever the caller said, because §9.6's trial "covers only a
     * configuration that has just been submitted" and this function is the only
     * way one is. Forced here rather than asked of `POST /wifi`, so a second
     * caller cannot store a static address that skipped its proof. */
    slate_ipv4_config_t pending;
    if (ipv4 != NULL && ipv4->state == SLATE_IPV4_DHCP) {
        ipv4 = NULL; /* DHCP is the absence of a stored configuration, not one */
    }
    if (ipv4 != NULL) {
        pending = *ipv4;
        pending.state = SLATE_IPV4_STATIC_PENDING;
        ipv4 = &pending;
    }

    esp_err_t err = slate_store_wifi_set(ssid, password, ipv4);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "credentials for \"%s\" stored (%s) — applying", ssid,
             ipv4 != NULL ? "static address, on trial" : "dhcp");
    send_msg(MSG_APPLY, 0);
    return ESP_OK;
}

esp_err_t slate_wifi_forget(void)
{
    esp_err_t err = slate_store_wifi_clear();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGW(TAG, "credentials forgotten");
    send_msg(MSG_FORGET, 0);
    return ESP_OK;
}
