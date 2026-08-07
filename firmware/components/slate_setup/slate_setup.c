/*
 * Slate — the setup access point. See include/slate_setup.h for the contract.
 *
 * design.md §9.2, §9.4, §6.2, §12.
 *
 * The access point is raised and torn down on one task, fed by one queue, for
 * the reason slate_wifi.c gives for the same arrangement one component over: the
 * work is slow — a sweep of the band is seconds, bringing an interface up is not
 * instant — and doing it on the default event loop task would stall the
 * station's own events. §9.4's guarantee is that the station keeps trying while
 * the access point is up, and the cheapest way to keep that true is for the two
 * never to run on the same task.
 */

#include "slate_setup.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "slate_api.h"
#include "slate_setup_private.h"
#include "slate_store.h"
#include "slate_wifi.h"

static const char *TAG = "setup";

/* --- §9.2's numbers ----------------------------------------------------- */

/*
 * The channel the access point starts on, and only starts on. It does not stay
 * there: one radio means the access point must follow the station, so the
 * driver moves it the moment an association succeeds (§9.3). Anything clever
 * here — picking the quietest channel from the scan, say — would be undone by
 * the first attempt to connect.
 */
#define AP_CHANNEL 1

/* Enough for a phone, a laptop, and somebody else's phone. The panel is being
 * configured, not serving a network. */
#define AP_MAX_CLIENTS 4

/* §9.2: "The 8-character floor is the standard's, not ours." A shorter
 * passphrase in NVS is a configuration error, and the access point comes up open
 * rather than not at all — §9 has no state where the panel is unreachable. */
#define AP_WPA2_MIN_PASSWORD 8

/* --- Task plumbing ------------------------------------------------------ */

typedef enum {
    MSG_RAISE,   /* SLATE_WIFI_EVENT_SETUP_REQUESTED, `reason` set */
    MSG_RELEASE, /* SLATE_WIFI_EVENT_SETUP_RELEASED */
} msg_kind_t;

typedef struct {
    msg_kind_t kind;
    slate_wifi_setup_reason_t reason;
} msg_t;

#define MSG_QUEUE_DEPTH 4
#define TASK_STACK      4096
#define TASK_PRIORITY   4 /* below slate_wifi's 5: the station outranks the fallback */

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static esp_event_handler_instance_t s_subscription;

/* Task-local in everything but name — only setup_task() touches these. */
static esp_netif_t *s_ap_netif;
static bool s_ap_up;

/* --- The scan cache ----------------------------------------------------- */

static SemaphoreHandle_t s_scan_lock;
static slate_setup_network_t s_scan[SLATE_SETUP_SCAN_MAX];
static size_t s_scan_count;
static int64_t s_scan_at; /* esp_timer_get_time() of the sweep; 0 if never */

/** Insert one record into the signal-ordered cache, strongest duplicate wins. */
static void cache_insert(const wifi_ap_record_t *record)
{
    /* Hidden networks arrive with an empty SSID and cannot be picked from a
     * list. The manual field on the page is what reaches those. */
    if (record->ssid[0] == '\0') {
        return;
    }

    /* One SSID, one row. A mesh answers from every node and a dual-band router
     * answers twice, and a list with "home" in it four times reads as a
     * malfunction. The strongest of them is also the one the driver will pick. */
    for (size_t i = 0; i < s_scan_count; i++) {
        if (strncmp(s_scan[i].ssid, (const char *) record->ssid, sizeof(s_scan[i].ssid)) != 0) {
            continue;
        }
        if (record->rssi <= s_scan[i].rssi) {
            return;
        }
        memmove(&s_scan[i], &s_scan[i + 1], (s_scan_count - i - 1) * sizeof(s_scan[0]));
        s_scan_count--;
        break;
    }

    size_t at = 0;
    while (at < s_scan_count && s_scan[at].rssi >= record->rssi) {
        at++;
    }
    if (at >= SLATE_SETUP_SCAN_MAX) {
        return;
    }

    if (s_scan_count < SLATE_SETUP_SCAN_MAX) {
        s_scan_count++;
    }
    memmove(&s_scan[at + 1], &s_scan[at], (s_scan_count - at - 1) * sizeof(s_scan[0]));

    memset(&s_scan[at], 0, sizeof(s_scan[at]));
    /* An SSID fills esp_wifi's field exactly and need not be NUL-terminated
     * there, which is why this is a bounded copy into a buffer one byte larger
     * rather than a strlcpy of a string that may not be one. */
    memcpy(s_scan[at].ssid, record->ssid, strnlen((const char *) record->ssid,
                                                 sizeof(record->ssid)));
    s_scan[at].rssi = record->rssi;
    s_scan[at].channel = record->primary;
    s_scan[at].auth = record->authmode;
}

esp_err_t slate_setup_scan(void)
{
    if (s_scan_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * wifi_ap_record_t is the better part of a hundred bytes and this asks for
     * twenty of them, which is half the HTTP task's stack — and this function is
     * called from that task by the page's refresh button. PSRAM because nothing
     * here is touched by DMA, and §6.2's internal budget is what the second
     * interface has to fit into.
     */
    wifi_ap_record_t *records =
        heap_caps_malloc(SLATE_SETUP_SCAN_MAX * sizeof(*records), MALLOC_CAP_SPIRAM);
    if (records == NULL) {
        records = malloc(SLATE_SETUP_SCAN_MAX * sizeof(*records));
    }
    if (records == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_scan_lock, portMAX_DELAY);

    const wifi_scan_config_t config = {
        /* Active, and quick about it. The default per-channel dwell multiplied
         * by fourteen channels is long enough that the person holding the phone
         * concludes the panel has died — which §9.2 says is exactly what a sweep
         * looks like from a browser. */
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 60, .max = 150},
    };

    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) {
        /*
         * ESP_ERR_WIFI_STATE is the station being mid-association, and it is the
         * normal case rather than a fault: the access point is up precisely
         * because the station is trying. The caller answers from the cache.
         */
        ESP_LOGW(TAG, "scan refused: %s", esp_err_to_name(err));
        xSemaphoreGive(s_scan_lock);
        free(records);
        return err;
    }

    uint16_t found = SLATE_SETUP_SCAN_MAX;
    err = esp_wifi_scan_get_ap_records(&found, records);
    if (err == ESP_OK) {
        s_scan_count = 0;
        for (uint16_t i = 0; i < found; i++) {
            cache_insert(&records[i]);
        }
        s_scan_at = esp_timer_get_time();
        ESP_LOGI(TAG, "scan: %u network(s), %u kept", (unsigned) found, (unsigned) s_scan_count);
    } else {
        /* The driver holds the list until it is read out or cleared, and a
         * failed read that left it there would leak it for the rest of the
         * boot. */
        ESP_LOGW(TAG, "reading scan results: %s", esp_err_to_name(err));
        esp_wifi_clear_ap_list();
    }

    xSemaphoreGive(s_scan_lock);
    free(records);
    return err;
}

size_t slate_setup_scan_copy(slate_setup_network_t *out, int64_t *age_us)
{
    if (out == NULL || s_scan_lock == NULL) {
        if (age_us != NULL) {
            *age_us = -1;
        }
        return 0;
    }

    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    size_t count = s_scan_count;
    memcpy(out, s_scan, count * sizeof(*out));
    if (age_us != NULL) {
        *age_us = s_scan_at == 0 ? -1 : esp_timer_get_time() - s_scan_at;
    }
    xSemaphoreGive(s_scan_lock);
    return count;
}

/* --- The interface ------------------------------------------------------ */

/**
 * Create the access point's netif, with itself as gateway and DNS server.
 *
 * The DHCP options are set here rather than after the interface starts, because
 * esp_netif refuses to change them on a running server and the interface starts
 * the moment the mode is written. What the two of them buy:
 *
 *   DNS server      the address handed out with the lease, which is what makes
 *                   slate_setup_dns.c's answers get asked for at all
 *   option 114      RFC 8910, the portal's URL delivered in the lease. A client
 *                   that understands it opens the page without the probe
 *                   interception below ever being consulted, which is the
 *                   difference between a portal sheet that works and one that
 *                   appears with a "no internet" warning over it
 */
static esp_netif_t *create_ap_netif(void)
{
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (netif == NULL) {
        return NULL;
    }

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(SLATE_SETUP_AP_ADDRESS);

    uint8_t offer_dns = 1;
    esp_err_t err = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns,
                                           sizeof(offer_dns));
    if (err == ESP_OK) {
        err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    if (err != ESP_OK) {
        /* Not fatal. The page is still at an address that is printed on the
         * screen (§9.2: the captive portal "is best-effort and never the only
         * way in"). */
        ESP_LOGW(TAG, "no DNS server in the lease: %s", esp_err_to_name(err));
    }

    /* The pointer is handed to the DHCP server and kept, not copied — hence
     * static rather than a local. */
    static const char PORTAL_URI[] = "http://" SLATE_SETUP_AP_ADDRESS;
    err = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                 (void *) PORTAL_URI, strlen(PORTAL_URI));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no captive portal URI in the lease: %s", esp_err_to_name(err));
    }

    return netif;
}

/** Configure the access point from §9.2: `slate-<mac6>`, open unless NVS says otherwise. */
static esp_err_t configure_ap(void)
{
    wifi_config_t config = {0};
    const char *name = slate_store_device_name();

    /* §9.2 and §4.3 and §16 are the same string on purpose — "so one panel is
     * called one thing everywhere". slate_store owns it; this does not build a
     * second spelling out of the MAC. */
    size_t name_len = strnlen(name, sizeof(config.ap.ssid));
    memcpy(config.ap.ssid, name, name_len);
    config.ap.ssid_len = name_len;
    config.ap.channel = AP_CHANNEL;
    config.ap.max_connection = AP_MAX_CLIENTS;
    config.ap.authmode = WIFI_AUTH_OPEN;

    char password[SLATE_WIFI_PASSWORD_BUF_LEN] = {0};
    if (slate_store_str_get(SLATE_KEY_SETUP_AP_PASS, password, sizeof(password)) == ESP_OK) {
        size_t len = strlen(password);
        if (len >= AP_WPA2_MIN_PASSWORD) {
            memcpy(config.ap.password, password, strnlen(password, sizeof(config.ap.password)));
            config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else if (len > 0) {
            ESP_LOGE(TAG, "the stored access point passphrase is shorter than %d characters — "
                          "coming up open instead",
                     AP_WPA2_MIN_PASSWORD);
        }
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &config);

    /* §12's rule about the station passphrase applies here for the same reason:
     * a task stack is where §11.3's core dump would find it and put it in a file
     * somebody attaches to an issue. */
    memset(password, 0, sizeof(password));
    memset(config.ap.password, 0, sizeof(config.ap.password));
    return err;
}

/** What §6.5's setup mode would print, until #6 has a screen to print it on. */
static void log_setup_card(slate_wifi_setup_reason_t reason)
{
    static const char *WHY[] = {
        "no credentials are stored",
        "the stored network could not be joined",
        "the network went away and has not come back",
    };

    slate_wifi_status_t station;
    slate_wifi_status(&station);
    const char *last_error = slate_wifi_error_str(station.last_error);

    ESP_LOGW(TAG, "---- setup mode: %s ----", WHY[reason]);
    ESP_LOGW(TAG, "  join  %s", slate_store_device_name());
    ESP_LOGW(TAG, "  open  http://%s", SLATE_SETUP_AP_ADDRESS);
    if (station.sta_ssid[0] != '\0') {
        ESP_LOGW(TAG, "  was configured for \"%s\", last error %s", station.sta_ssid,
                 last_error != NULL ? last_error : "none");
    }
}

static void raise_ap(slate_wifi_setup_reason_t reason)
{
    if (s_ap_up) {
        /* slate_wifi re-posts when the reason changes, so this is the panel
         * whose credentials were just deleted while the access point was
         * already up: the interface stays, the card is reprinted. */
        log_setup_card(reason);
        return;
    }

    if (s_ap_netif == NULL) {
        s_ap_netif = create_ap_netif();
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "no access point interface — the panel is unreachable until the "
                          "station connects");
            return;
        }
    }

    /*
     * APSTA, and this is the only place the mode is written after
     * esp_wifi_start(). §9.4 depends on it: the station must keep trying while
     * the access point is up, so that a router which comes back at minute seven
     * finds the panel waiting for it with nobody in the room.
     *
     * The interface starts on this line, which is why configure_ap() cannot come
     * first: esp_wifi_set_config() refuses WIFI_IF_AP while the mode has no
     * access point in it. The window in between is one beacon interval of an
     * unnamed network that nothing can join.
     */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = configure_ap();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raising the access point: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        return;
    }

    s_ap_up = true;

    /*
     * §4.3's token exception and slate_setup_dns.c's bind are both keyed on this
     * address as a string, and it is esp_netif's default rather than something
     * this component sets. If a component upgrade ever moves it, the exception
     * stops matching and the setup page answers 401 to the one browser that has
     * no way to send a token — so it is checked rather than assumed.
     */
    esp_netif_ip_info_t ip = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        char address[16];
        esp_ip4addr_ntoa(&ip.ip, address, sizeof(address));
        if (strcmp(address, SLATE_SETUP_AP_ADDRESS) != 0) {
            ESP_LOGE(TAG, "the access point came up on %s, not %s — the setup page will ask for "
                          "a token nobody has",
                     address, SLATE_SETUP_AP_ADDRESS);
        }
    }

    log_setup_card(reason);

    /*
     * The number #55 asks for, against S-2's 104 167 B with the station alone
     * (§6.2). Logged on every raise rather than measured once and written into a
     * comment, because the interface that has to fit into what is left is
     * created here and the issues after this one keep adding to the other side
     * of the subtraction.
     */
    ESP_LOGW(TAG, "free internal DMA-capable memory with APSTA up: %u B",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

    esp_err_t dns_err = slate_setup_dns_start();
    if (dns_err != ESP_OK) {
        ESP_LOGW(TAG, "no captive DNS responder: %s — the address is still on the screen",
                 esp_err_to_name(dns_err));
    }

    /* Once, on the way up, per §9.2. The page's refresh button is the only other
     * sweep, and it warns that it takes a few seconds. */
    slate_setup_scan();

#ifdef SLATE_SETUP_SELFTEST
    /* Here rather than in app_main, because every case it checks needs an
     * interface that only exists between this line and release_ap(). */
    slate_setup_selftest();
#endif
}

static void release_ap(void)
{
    if (!s_ap_up) {
        return;
    }

    ESP_LOGI(TAG, "the station is back — tearing the access point down");
    slate_setup_dns_stop();

    /*
     * Back to station-only, and the interface destroyed rather than left
     * configured-but-idle: §6.2 says the access point is "raised on demand and
     * torn down as soon as the station associates, never left running as a
     * permanent second interface", and it says that because the memory it costs
     * is the internal kind the display has to come out of.
     */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "returning to station-only mode: %s", esp_err_to_name(err));
    }

    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    s_ap_up = false;

    ESP_LOGI(TAG, "free internal DMA-capable memory with the station alone: %u B",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}

static void setup_task(void *arg)
{
    (void) arg;

    for (;;) {
        msg_t msg;
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.kind == MSG_RAISE) {
            raise_ap(msg.reason);
        } else {
            release_ap();
        }
    }
}

/* --- §9.4's hand-off ---------------------------------------------------- */

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;

    msg_t msg;
    switch ((slate_wifi_event_id_t) id) {
    case SLATE_WIFI_EVENT_SETUP_REQUESTED:
        msg.kind = MSG_RAISE;
        msg.reason = *(const slate_wifi_setup_reason_t *) data;
        break;

    case SLATE_WIFI_EVENT_SETUP_RELEASED:
        msg.kind = MSG_RELEASE;
        msg.reason = SLATE_WIFI_SETUP_NO_CREDENTIALS; /* unused */
        break;

    default:
        return;
    }

    /* Never blocks: this runs on the event loop task, and blocking it would
     * hold up the station's own events — which is the thing §9.4 says has to
     * keep moving while the access point is being raised. */
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGE(TAG, "queue full — dropped %s",
                 msg.kind == MSG_RAISE ? "a request to raise the access point"
                                       : "a request to tear it down");
    }
}

esp_err_t slate_setup_init(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_queue = xQueueCreate(MSG_QUEUE_DEPTH, sizeof(msg_t));
    s_scan_lock = xSemaphoreCreateMutex();
    if (s_queue == NULL || s_scan_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Created here if nobody has yet, the same way slate_wifi does, so the three
     * components compose whichever order app_main brings them up in. */
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = slate_setup_api_init();
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Subscribed before the station exists, which is why app_main calls this
     * before slate_wifi_init(): SETUP_REQUESTED fires as soon as the station task
     * finds NVS empty — milliseconds into a factory-fresh boot — and it fires
     * once. An event posted before this line is not a delayed access point, it is
     * a panel that never raises one and sits there waiting for a message nothing
     * will send again.
     */
    err = esp_event_handler_instance_register(SLATE_WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL,
                                              &s_subscription);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(setup_task, "slate_setup", TASK_STACK, NULL, TASK_PRIORITY, &s_task) !=
        pdPASS) {
        esp_event_handler_instance_unregister(SLATE_WIFI_EVENT, ESP_EVENT_ANY_ID, s_subscription);
        s_subscription = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "watching for the station; the setup page is at http://%s when it is needed",
             SLATE_SETUP_AP_ADDRESS);
    return ESP_OK;
}
