/*
 * Slate — firmware entry point.
 *
 * At this point in M1 the application is the store and nothing else. The
 * display (#6), WiFi and the setup access point (#8, #55), the HTTP API (#10)
 * and OTA (#11, #12) each attach here as they land, in that order, because each
 * one depends on the one before it having somewhere to keep its state.
 *
 * The boot report below is the only user interface the firmware currently has.
 * It exists to answer the question this issue is judged on — is the device
 * token the same one as before the reboot — and it answers it with a
 * fingerprint rather than the token. The token itself belongs in exactly one
 * place, §4.3's pairing QR on the screen; a serial log is a thing people paste
 * into issues.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#include "slate_store.h"

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
}
