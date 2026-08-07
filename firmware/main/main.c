/*
 * Slate — firmware entry point.
 *
 * At this point in M1 the application is the store, the WiFi station and the
 * clock, the HTTP API, development OTA and the setup access point. The display
 * (#6) and OTA rollback (#12) each attach here as they land, in that order,
 * because each one depends on the one before it having somewhere to keep its
 * state.
 *
 * The boot report below is the only user interface the firmware currently has.
 * It exists to answer the questions the landed issues are judged on — is the
 * device token the same one as before the reboot, did the station come back on
 * its own — and it answers the first with a fingerprint rather than the token.
 * The token itself belongs in exactly one place, §4.3's pairing QR on the
 * screen; a serial log is a thing people paste into issues.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#include "slate_api.h"
#include "slate_ota.h"
#include "slate_setup.h"
#include "slate_store.h"
#include "slate_time.h"
#include "slate_wifi.h"

static const char *TAG = "slate";

#ifdef SLATE_STORE_SELFTEST
static int s_failures;

#define CHECK(cond, what)                                     \
    do {                                                      \
        bool ok_ = (cond);                                    \
        if (!ok_) {                                           \
            s_failures++;                                     \
        }                                                     \
        ESP_LOGI(TAG, "selftest: %-34s %s", what,             \
                 ok_ ? "PASS" : "FAIL");                      \
    } while (0)

/*
 * The LittleFS half of this issue's "Done when" — a configuration that survives
 * an application update — cannot be exercised from outside the device until
 * `PUT /config` exists (#24). Until then this knob is the only writer:
 *
 *     idf.py -DSLATE_STORE_SELFTEST=1 build flash monitor
 *
 * It writes a configuration on the first boot that finds none, and reports what
 * it reads back on every boot afterwards. Reflash the application partition
 * between the two and the report is the proof. It is off by default and comes
 * out when #24 lands.
 *
 * Every case states a verdict rather than printing a value. A check that prints
 * `refused with ESP_OK` and calls it a day is not a check.
 */
static void store_selftest(void)
{
    static const char SAMPLE[] =
        "{\"schema\":1,\"theme\":\"midnight\",\"home_page\":\"home\",\"pages\":[]}";
    const size_t SAMPLE_LEN = sizeof(SAMPLE) - 1;

    if (!slate_store_config_exists()) {
        esp_err_t err = slate_store_config_write(SAMPLE, SAMPLE_LEN);
        CHECK(err == ESP_OK, "write configuration");
    }

    char *json = NULL;
    size_t len = 0;
    esp_err_t err = slate_store_config_read(&json, &len);
    CHECK(err == ESP_OK, "read configuration");
    if (err == ESP_OK) {
        CHECK(len == SAMPLE_LEN && memcmp(json, SAMPLE, len) == 0, "configuration round-trips");
        free(json);
    }

    /* §3.1's cap, checked against a length one byte past the real buffer rather
     * than a fabricated 64 KB — the guard is what is under test, and handing it
     * a length that would overread if it ever moved is not worth the coverage. */
    CHECK(slate_store_config_write(SAMPLE, sizeof(SAMPLE)) == ESP_OK, "write at buffer length");
    CHECK(slate_store_config_write(SAMPLE, SLATE_CONFIG_MAX_BYTES + 1) == ESP_ERR_INVALID_SIZE,
          "over-size write refused");
    CHECK(slate_store_config_write(SAMPLE, 0) == ESP_ERR_INVALID_ARG, "empty write refused");

    /* Restore, so the next boot's round-trip check has the sample back. */
    slate_store_config_write(SAMPLE, SAMPLE_LEN);

    char token[SLATE_DEVICE_TOKEN_LEN + 1];
    CHECK(slate_store_device_token_copy(token, sizeof(token)) == ESP_OK, "token copy");
    CHECK(strlen(token) == SLATE_DEVICE_TOKEN_LEN, "token is 32 characters");
    CHECK(slate_store_device_token_matches(token), "correct token matches");
    CHECK(!slate_store_device_token_matches("0000000000000000000000000000000A"),
          "wrong token rejected");
    CHECK(!slate_store_device_token_matches(""), "empty token rejected");
    token[SLATE_DEVICE_TOKEN_LEN - 1] = '\0';
    CHECK(!slate_store_device_token_matches(token), "short token rejected");

    char small[4];
    CHECK(slate_store_device_token_copy(small, sizeof(small)) == ESP_ERR_INVALID_SIZE,
          "token copy refuses a small buffer");

    /* §12: the generic accessor must not be able to name the secret. */
    char leak[SLATE_HA_TOKEN_MAX_LEN];
    CHECK(slate_store_str_get("ha_token", leak, sizeof(leak)) == ESP_ERR_INVALID_ARG,
          "ha token unreachable via str_get");

    /* One vocabulary for "unset", whichever partition it lives on. */
    CHECK(slate_store_str_get("no_such_key", leak, sizeof(leak)) == ESP_ERR_NOT_FOUND,
          "missing key reports NOT_FOUND");

    size_t size = 0;
    CHECK(slate_store_str_size(SLATE_KEY_DEVICE_TOKEN, &size) == ESP_OK &&
              size == SLATE_DEVICE_TOKEN_LEN + 1,
          "str_size reports the required buffer");

    ESP_LOGI(TAG, "selftest: %d failure(s)", s_failures);
}
#endif

/*
 * The boot report's network half. slate_setup now acts on two of these four and
 * prints §6.5's setup card itself; this stays because it is the only place the
 * two sides of the hand-off are visible in one log — an address arriving and an
 * access point going away, in the order they happened, which is the question
 * asked of every §9.4 test until #6 has a screen to answer it on.
 */
static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;

    switch ((slate_wifi_event_id_t) id) {
    case SLATE_WIFI_EVENT_CONNECTED: {
        const slate_wifi_status_t *status = data;
        ESP_LOGI(TAG, "network: on \"%s\" at %s, rssi %d dBm", status->sta_ssid, status->ip,
                 status->rssi);
        break;
    }

    case SLATE_WIFI_EVENT_DISCONNECTED: {
        const slate_wifi_status_t *status = data;
        ESP_LOGW(TAG, "network: down after %u attempt(s), last_error %s", status->attempts,
                 slate_wifi_error_str(status->last_error));
        break;
    }

    case SLATE_WIFI_EVENT_SETUP_REQUESTED: {
        const slate_wifi_setup_reason_t *reason = data;
        static const char *WHY[] = {"no credentials", "could not connect", "station lost"};
        ESP_LOGW(TAG, "network: setup access point wanted — %s", WHY[*reason]);
        break;
    }

    case SLATE_WIFI_EVENT_SETUP_RELEASED:
        ESP_LOGI(TAG, "network: setup access point no longer needed");
        break;
    }
}

/*
 * §9.2's optional WPA2 passphrase for the setup access point, from the build:
 *
 *     idf.py -DSLATE_SETUP_AP_PASSWORD='"a good long one"' build flash
 *
 * The station credentials no longer arrive this way — `POST /wifi` is what does
 * that from #55 onwards, which is the whole point of the milestone. This one
 * knob remains because §9.2 puts the value in NVS and nothing in M1's scope
 * writes it: the setup page grows that field in #35, and until it does a knob is
 * the difference between a path that is exercised and a path that is asserted.
 *
 * Written only when it differs from what is stored, so a boot without the knob
 * does not undo it. Pass an empty string to go back to an open access point.
 * Never given a default: a passphrase in a build file is a passphrase in git
 * history (§12).
 */
static void provision_setup_ap(void)
{
#ifdef SLATE_SETUP_AP_PASSWORD
    char stored[SLATE_WIFI_PASSWORD_BUF_LEN] = {0};
    slate_store_str_get(SLATE_KEY_SETUP_AP_PASS, stored, sizeof(stored));
    if (strcmp(stored, SLATE_SETUP_AP_PASSWORD) == 0) {
        memset(stored, 0, sizeof(stored));
        return;
    }
    memset(stored, 0, sizeof(stored));

    esp_err_t err = SLATE_SETUP_AP_PASSWORD[0] == '\0'
                        ? slate_store_erase(SLATE_KEY_SETUP_AP_PASS)
                        : slate_store_str_set(SLATE_KEY_SETUP_AP_PASS, SLATE_SETUP_AP_PASSWORD);
    ESP_LOGW(TAG, "setup access point passphrase %s from the build: %s",
             SLATE_SETUP_AP_PASSWORD[0] == '\0' ? "cleared" : "set", esp_err_to_name(err));
#endif
}

static void start_network(void)
{
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s — no network this boot", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(SLATE_WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        network_event, NULL, NULL));

    /*
     * Not ESP_ERROR_CHECK, for the reason slate_store_init() is not: §9 has no
     * state in which a powered panel is unreachable, and a radio that would
     * not start is not made reachable by rebooting into the same failure.
     */
    err = slate_wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi degraded: %s — continuing", esp_err_to_name(err));
        return;
    }

    /*
     * The number #55 asks for. S-2 measured 104 167 B of internal DMA-capable
     * memory free with the station alone (§6.2), and the setup access point's
     * second interface has to come out of what is left after this line — so it
     * is printed on every boot rather than measured once and written down.
     */
    ESP_LOGI(TAG, "free internal DMA-capable memory with the station up: %u B",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

    err = slate_time_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "clock degraded: %s — continuing", esp_err_to_name(err));
        return;
    }

#ifdef SLATE_TIMEZONE
    /* §3.3 puts this in the configuration document, which #19 parses. The knob
     * stands in until it does, and goes away with it. */
    slate_time_set_timezone(SLATE_TIMEZONE);
#endif
}

static void start_api(void)
{
    /* An API failure is not made recoverable by rebooting into the same
     * allocation failure. Keep the device alive so #6's screen can report it
     * and the station can still reconnect; remote management is degraded for
     * this boot and the error remains on serial. */
    esp_err_t err = slate_api_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP API degraded: %s — continuing", esp_err_to_name(err));
        return;
    }

#ifdef SLATE_API_SELFTEST
    slate_api_selftest();
#endif

    /* §11.1 is one route on the server that just came up, so it attaches here
     * and not before: without the API there is nothing to register against,
     * and a device whose HTTP server did not start cannot be flashed over the
     * network by any means this milestone owns. */
    err = slate_ota_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "development OTA unavailable: %s — continuing", esp_err_to_name(err));
    }

    /*
     * §9's promise — no powered panel is unreachable — and the routes that carry
     * it, registered on the same server for the same reason. Before the station,
     * not after: the hand-off that raises the access point is posted once, within
     * milliseconds of the station finding NVS empty, and a subscriber that
     * arrives after it is a factory-fresh panel with no way in at all.
     */
    err = slate_setup_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup access point unavailable: %s — a panel that cannot join a network "
                      "will need a cable",
                 esp_err_to_name(err));
    }
}

void app_main(void)
{
    /*
     * Not ESP_ERROR_CHECK. §9 says there is no combination of circumstances in
     * which a powered panel is unreachable, and aborting here would reboot into
     * the identical failure forever — the one thing only a USB cable fixes. A
     * store that could not come up is exactly when the setup access point (#55)
     * and the error screen (#6) matter most, so the boot continues and says so.
     */
    esp_err_t store_err = slate_store_init();
    if (store_err != ESP_OK) {
        ESP_LOGE(TAG, "store degraded: %s — continuing", esp_err_to_name(store_err));
    }

    char fingerprint[SLATE_TOKEN_FINGERPRINT_LEN + 1] = "?";
    slate_store_device_token_fingerprint(fingerprint, sizeof(fingerprint));

    ESP_LOGI(TAG, "%s, boot #%" PRIu32 ", reset reason %d",
             slate_store_device_name(), slate_store_boot_count(), (int) esp_reset_reason());
    ESP_LOGI(TAG, "device token %s (fingerprint; the token itself is shown only in §4.3's QR)",
             fingerprint);

    size_t fs_total = 0;
    size_t fs_used = 0;
    if (slate_store_fs_usage(&fs_total, &fs_used) == ESP_OK) {
        ESP_LOGI(TAG, "littlefs %u B total, %u B used, configuration %s",
                 (unsigned) fs_total, (unsigned) fs_used,
                 slate_store_config_exists() ? "present" : "absent");
    } else {
        ESP_LOGW(TAG, "littlefs unavailable");
    }

    if (slate_store_storage_was_reset()) {
        ESP_LOGE(TAG, "storage was reset during boot — stored configuration and credentials "
                      "are gone");
    }

    ESP_LOGI(TAG, "home assistant %s",
             slate_store_ha_token_is_set() ? "configured" : "not configured");
    ESP_LOGI(TAG, "free heap %u B internal, %u B psram",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

#ifdef SLATE_STORE_SELFTEST
    store_selftest();
#endif

    provision_setup_ap();

    /* The API and the setup portal come up before the radio, so that the setup
     * access point has a page to serve and a subscriber in place by the time the
     * station has an opinion about whether one is needed. */
    start_api();
    start_network();
}
