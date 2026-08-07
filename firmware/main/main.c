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
 * token the same one as before the reboot — and it is written to the serial log
 * because §4.3's pairing QR needs a screen that #6 has not brought up yet. When
 * it does, the QR replaces this and the token stops being logged.
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

/*
 * §4.1 puts a reboot counter in `GET /status`, next to the reset reason,
 * because the two together are what distinguishes a panic from a power cut
 * without a cable attached. #10 reads it; the increment belongs at boot.
 */
static uint32_t bump_boot_count(void)
{
    uint32_t count = 0;

    esp_err_t err = slate_store_u32_get(SLATE_KEY_BOOT_COUNT, &count);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "reading boot count: %s", esp_err_to_name(err));
    }

    count++;
    err = slate_store_u32_set(SLATE_KEY_BOOT_COUNT, count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "writing boot count: %s", esp_err_to_name(err));
    }

    return count;
}

#ifdef SLATE_STORE_SELFTEST
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
 */
static void store_selftest(void)
{
    static const char SAMPLE[] =
        "{\"schema\":1,\"theme\":\"midnight\",\"home_page\":\"home\",\"pages\":[]}";

    if (!slate_store_config_exists()) {
        esp_err_t err = slate_store_config_write(SAMPLE, sizeof(SAMPLE) - 1);
        ESP_LOGI(TAG, "selftest: wrote %d B of configuration: %s",
                 (int) (sizeof(SAMPLE) - 1), esp_err_to_name(err));
    }

    char *json = NULL;
    size_t len = 0;
    esp_err_t err = slate_store_config_read(&json, &len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "selftest: reading configuration: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "selftest: read %u B, %s", (unsigned) len,
             (len == sizeof(SAMPLE) - 1 && memcmp(json, SAMPLE, len) == 0) ? "identical"
                                                                          : "DIFFERENT");
    free(json);

    /* Over-size writes are refused rather than truncated (§3.1). */
    err = slate_store_config_write(SAMPLE, SLATE_CONFIG_MAX_BYTES + 1);
    ESP_LOGI(TAG, "selftest: 64 KB + 1 write refused with %s", esp_err_to_name(err));

    ESP_LOGI(TAG, "selftest: token match %d / mismatch %d",
             slate_store_device_token_matches(slate_store_device_token()),
             slate_store_device_token_matches("0000000000000000000000000000000A"));
}
#endif

void app_main(void)
{
    ESP_ERROR_CHECK(slate_store_init());

    size_t fs_total = 0;
    size_t fs_used = 0;
    ESP_ERROR_CHECK(slate_store_fs_usage(&fs_total, &fs_used));

    /* Outside the ESP_LOGI below on purpose: at a log level under INFO the
     * macro compiles away, and with it would go the increment. */
    uint32_t boot_count = bump_boot_count();

    ESP_LOGI(TAG, "%s, boot #%" PRIu32 ", reset reason %d",
             slate_store_device_name(), boot_count, (int) esp_reset_reason());
    ESP_LOGI(TAG, "device token %s", slate_store_device_token());
    ESP_LOGI(TAG, "littlefs %u B total, %u B used, configuration %s",
             (unsigned) fs_total, (unsigned) fs_used,
             slate_store_config_exists() ? "present" : "absent");
    ESP_LOGI(TAG, "home assistant %s",
             slate_store_ha_token_is_set() ? "configured" : "not configured");
    ESP_LOGI(TAG, "free heap %u B internal, %u B psram",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

#ifdef SLATE_STORE_SELFTEST
    store_selftest();
#endif
}
