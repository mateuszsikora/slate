/*
 * Slate — firmware entry point.
 *
 * At this point in M1 the application is the store, the WiFi station and the
 * clock. The display (#6), the setup access point (#55), the HTTP API (#10)
 * and OTA (#11, #12) each attach here as they land, in that order, because
 * each one depends on the one before it having somewhere to keep its state.
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
 * The hand-off #55 and #6 will subscribe to, printed until they exist.
 *
 * It is here rather than inside slate_wifi so the component has at least one
 * external subscriber from the day it lands: an event nobody listens to is an
 * event whose payload is wrong in a way nothing notices.
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
        ESP_LOGW(TAG, "network: setup access point wanted — %s (#55 raises it)", WHY[*reason]);
        break;
    }

    case SLATE_WIFI_EVENT_SETUP_RELEASED:
        ESP_LOGI(TAG, "network: setup access point no longer needed (#55 tears it down)");
        break;
    }
}

/*
 * Credentials from the build, until #55's `POST /wifi` exists.
 *
 *     idf.py -DSLATE_WIFI_SSID='"my-network"' -DSLATE_WIFI_PASSWORD='"secret"' \
 *            build flash monitor
 *
 * Written only when they differ from what is stored, so a boot without the
 * knobs does not undo a `DELETE /wifi`, and so the §9.4 paths that depend on
 * NVS surviving a reboot can be exercised. `idf.py erase-flash` is how to get
 * back to a device with no credentials at all.
 *
 * This is a development knob and comes out with #55. It is also the reason the
 * repository must not carry a default: a passphrase in a build file is a
 * passphrase in git history (§12).
 */
static void provision_wifi(void)
{
#ifdef SLATE_WIFI_SSID
#ifndef SLATE_WIFI_PASSWORD
#define SLATE_WIFI_PASSWORD NULL
#endif
    char stored[SLATE_WIFI_SSID_BUF_LEN];
    if (slate_store_wifi_ssid_get(stored, sizeof(stored)) == ESP_OK &&
        strcmp(stored, SLATE_WIFI_SSID) == 0) {
        return;
    }

    esp_err_t err = slate_store_wifi_set(SLATE_WIFI_SSID, SLATE_WIFI_PASSWORD);
    ESP_LOGW(TAG, "provisioned \"%s\" from the build: %s", SLATE_WIFI_SSID,
             esp_err_to_name(err));
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

    provision_wifi();
    start_network();
}
