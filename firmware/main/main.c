/*
 * Slate — firmware entry point.
 *
 * At this point in M1 the application is the store, the WiFi station and the
 * clock, the HTTP API, development OTA with rollback, core dump retrieval and
 * the setup access point and the display.
 *
 * The boot report below is the only user interface the firmware currently has.
 * It exists to answer the questions the landed issues are judged on — is the
 * device token the same one as before the reboot, did the station come back on
 * its own — and it answers the first with a fingerprint rather than the token.
 * The token itself stays inside the API/session boundary; a serial log is a
 * thing people paste into issues.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#include "slate_api.h"
#include "slate_action.h"
#include "slate_brightness.h"
#include "slate_coredump.h"
#include "slate_config_api.h"
#include "slate_control_api.h"
#include "slate_direct.h"
#include "slate_display.h"
#include "slate_editor.h"
#include "slate_ha.h"
#include "slate_integrations.h"
#include "slate_mdns.h"
#include "slate_ota.h"
#include "slate_setup.h"
#include "slate_state.h"
#include "slate_store.h"
#include "slate_time.h"
#include "slate_ui.h"
#include "slate_wifi.h"
#include "slate_ws.h"

static const char *TAG = "slate";

#ifdef SLATE_TIME_SELFTEST
typedef struct {
    time_t epoch;
    int month;
    int day;
    int hour;
    int minute;
    int is_dst;
} time_case_t;

static esp_err_t time_selftest(void)
{
    static const time_case_t CASES[] = {
        /* Europe/Warsaw jumps 01:59 -> 03:00 at 01:00 UTC in spring. */
        {1774745940, 3, 29, 1, 59, 0},
        {1774746000, 3, 29, 3, 0, 1},
        /* And repeats 02:00 after 02:59 when summer time ends. */
        {1792889940, 10, 25, 2, 59, 1},
        {1792890000, 10, 25, 2, 0, 0},
    };

    char previous[SLATE_TIME_ZONE_MAX_LEN];
    strlcpy(previous, slate_time_timezone(), sizeof(previous));
    esp_err_t err = slate_time_set_timezone("Europe/Warsaw");
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); ++i) {
        struct tm local;
        if (!slate_time_localtime(CASES[i].epoch, &local) ||
            local.tm_mon + 1 != CASES[i].month || local.tm_mday != CASES[i].day ||
            local.tm_hour != CASES[i].hour || local.tm_min != CASES[i].minute ||
            local.tm_isdst != CASES[i].is_dst) {
            ESP_LOGE(TAG, "timezone selftest case %u failed", (unsigned) i);
            slate_time_set_timezone(previous);
            return ESP_FAIL;
        }
    }

    err = slate_time_set_timezone(previous);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "timezone selftest passed: Europe/Warsaw DST boundaries");
    }
    return err;
}
#endif

/*
 * cJSON represents a large Home Assistant catalog as thousands of small
 * allocations. ESP-IDF's default 16 KiB external-memory threshold puts every
 * one of those nodes in scarce internal RAM even though the assembled payload
 * itself lives in PSRAM. A real installation can therefore exhaust internal
 * memory while several megabytes of PSRAM remain free; the Wi-Fi PHY is then
 * the first subsystem to abort when it cannot allocate its tracking timer.
 *
 * Hooks are process-global, so install them once, before any component can use
 * cJSON or start another task. free() accepts pointers from either ESP heap,
 * which also makes the internal fallback safe if PSRAM is ever full.
 */
static void *json_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void configure_json_allocator(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = json_malloc,
        .free_fn = free,
    };
    cJSON_InitHooks(&hooks);
}

#ifdef SLATE_OTA_ROLLBACK_SELFTEST
/*
 * The honest negative control for §11.2: fail before the HTTP server or either
 * network interface exists, so a rollback cannot be credited to a convenient
 * network outage. Restricted to PENDING_VERIFY so a serial flash of the test
 * build does not create an unrecoverable boot loop.
 */
static void ota_rollback_selftest(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE(TAG, "rollback selftest: panicking before API and network startup");
        abort();
    }
    ESP_LOGW(TAG, "rollback selftest: %s is not pending verification; continuing",
             running->label);
}
#endif

#ifdef SLATE_COREDUMP_SELFTEST
/*
 * The forced panic §11.3 is measured with, and the only writer of the coredump
 * partition there will ever be — a dump is produced by crashing, and nothing in
 * M1's scope crashes on purpose.
 *
 * Two guards, and between them they are what keeps this from being the one knob
 * in the tree that can strand a panel. The neighbour above restricts itself to
 * PENDING_VERIFY for the same reason and says so in as many words.
 *
 * The first is "has anything ever been written here", not "is there a dump I can
 * read". Those differ exactly when the write failed — which S-3 observed
 * happening while the writer logged success — and a guard that re-arms on a
 * corrupt dump panics again on every boot forever. On a serial flash there is no
 * PENDING_VERIFY image and therefore no rollback to break that cycle, and no
 * endpoint erases the partition, so the way out would be a cable: precisely §9's
 * unreachable powered panel. It also keeps the failed write, which is evidence.
 *
 * The second is the reset reason, which closes the same hole from the other side:
 * a boot that is itself the reboot from a panic has already run the test, whatever
 * the panic managed to leave on flash.
 *
 * It panics before §11.2's health check is armed, so an OTA of this build
 * exercises the whole of §11.3's promise in one go: the panel crashes on a
 * pending image, the bootloader returns to the previous slot, and the dump
 * written by an image that is no longer running is still fetchable from the one
 * that is. That is the real shape of the problem — a panel that came back from a
 * crash is usually not running the firmware that crashed. Ordering, not timing:
 * the health task runs at a higher priority than app_main and would otherwise be
 * free to mark the image valid first, which made the promise a race this happened
 * to win by ten milliseconds.
 */
static void coredump_selftest(void)
{
    if (esp_reset_reason() == ESP_RST_PANIC) {
        ESP_LOGW(TAG, "coredump selftest: this boot follows a panic; not panicking again");
        return;
    }
    if (!slate_coredump_partition_is_blank()) {
        ESP_LOGW(TAG, "coredump selftest: the partition is not blank; not panicking again");
        return;
    }
    ESP_LOGE(TAG, "coredump selftest: panicking to write a core dump");
    abort();
}
#endif

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
 * Low-level LittleFS and token verifier, independent of the configuration API:
 *
 *     idf.py -DSLATE_STORE_SELFTEST=1 build flash monitor
 *
 * It writes a configuration on the first boot that finds none, and reports what
 * it reads back on every boot afterwards. Reflash the application partition
 * between the two and the report is the proof. It stays off by default; #24's
 * endpoint verification exercises the same store through its public boundary.
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

    size_t key_count = slate_store_integration_key_count();
    if (key_count < SLATE_INTEGRATION_KEY_MAX) {
        slate_integration_key_info_t key_info = {0};
        char integration_token[SLATE_INTEGRATION_KEY_TOKEN_LEN + 1] = {0};
        CHECK(slate_store_integration_key_create("selftest", &key_info,
                                                  integration_token,
                                                  sizeof(integration_token)) == ESP_OK,
              "create External API key");
        CHECK(strlen(integration_token) == SLATE_INTEGRATION_KEY_TOKEN_LEN,
              "External API key is 32 characters");
        CHECK(slate_store_integration_key_matches(integration_token),
              "External API key matches");
        CHECK(!slate_store_integration_key_matches(
                  "0000000000000000000000000000000A"),
              "wrong External API key rejected");
        CHECK(slate_store_integration_key_count() == key_count + 1,
              "External API key listed");
        CHECK(slate_store_integration_key_revoke(key_info.id) == ESP_OK,
              "revoke External API key");
        CHECK(!slate_store_integration_key_matches(integration_token),
              "revoked External API key rejected");
        CHECK(slate_store_integration_key_count() == key_count,
              "External API key count restored");
        explicit_bzero(integration_token, sizeof(integration_token));
    }

    char small[4];
    CHECK(slate_store_device_token_copy(small, sizeof(small)) == ESP_ERR_INVALID_SIZE,
          "token copy refuses a small buffer");

    /* §12: the generic accessor must not be able to name the secret. */
    char leak[SLATE_HA_TOKEN_MAX_LEN];
    CHECK(slate_store_str_get("ha_token", leak, sizeof(leak)) == ESP_ERR_INVALID_ARG,
          "ha token unreachable via str_get");
    CHECK(slate_store_str_get("int_keys", leak, sizeof(leak)) == ESP_ERR_INVALID_ARG,
          "integration key hashes unreachable via str_get");

    /* One vocabulary for "unset", whichever partition it lives on. */
    CHECK(slate_store_str_get("no_such_key", leak, sizeof(leak)) == ESP_ERR_NOT_FOUND,
          "missing key reports NOT_FOUND");

    size_t size = 0;
    CHECK(slate_store_str_size(SLATE_KEY_DEVICE_TOKEN, &size) == ESP_OK &&
              size == SLATE_DEVICE_TOKEN_LEN + 1,
          "str_size reports the required buffer");

    /* Preserve the real station tuple around the transactional WiFi checks.
     * This selftest runs before the station task starts, so no association can
     * observe the temporary values. */
    bool had_wifi = slate_store_wifi_is_configured();
    char saved_ssid[SLATE_WIFI_SSID_BUF_LEN] = {0};
    char saved_password[SLATE_WIFI_PASSWORD_BUF_LEN] = {0};
    bool had_password = had_wifi &&
                        slate_store_wifi_password_get(saved_password, sizeof(saved_password)) ==
                            ESP_OK;
    if (had_wifi) {
        CHECK(slate_store_wifi_ssid_get(saved_ssid, sizeof(saved_ssid)) == ESP_OK,
              "save station SSID");
    }
    slate_ipv4_config_t saved_ipv4;
    slate_store_ipv4_get(&saved_ipv4);

    const slate_ipv4_config_t test_ipv4 = {
        .state = SLATE_IPV4_STATIC_PENDING,
        .address = 0xC000022A, /* 192.0.2.42, TEST-NET-1 */
        .gateway = 0xC0000201,
        .dns = {0xC0000235},
        .prefix = 24,
        .dns_count = 1,
    };
    CHECK(slate_store_wifi_set("slate-selftest", "temporary-secret", &test_ipv4) == ESP_OK,
          "write static WiFi tuple");

    slate_ipv4_config_t first_trial;
    slate_store_ipv4_get(&first_trial);
    CHECK(first_trial.generation != 0, "static trial has identity");

    CHECK(slate_store_wifi_set("slate-selftest", NULL, NULL) == ESP_OK,
          "missing password keeps same SSID secret");
    char kept_password[SLATE_WIFI_PASSWORD_BUF_LEN] = {0};
    CHECK(slate_store_wifi_password_get(kept_password, sizeof(kept_password)) == ESP_OK &&
              strcmp(kept_password, "temporary-secret") == 0,
          "same SSID password preserved");
    explicit_bzero(kept_password, sizeof(kept_password));

    CHECK(slate_store_wifi_set("slate-selftest", NULL, &test_ipv4) == ESP_OK,
          "resubmit identical static tuple");
    slate_ipv4_config_t second_trial;
    slate_store_ipv4_get(&second_trial);
    CHECK(second_trial.generation != first_trial.generation,
          "identical trial gets new identity");
    CHECK(slate_store_ipv4_set_state(&first_trial, SLATE_IPV4_STATIC_CONFIRMED) ==
              ESP_ERR_INVALID_STATE,
          "stale trial verdict rejected");

    CHECK(slate_store_wifi_set("slate-selftest", "", NULL) == ESP_OK,
          "explicit empty password selects open");
    CHECK(slate_store_wifi_password_get(kept_password, sizeof(kept_password)) ==
              ESP_ERR_NOT_FOUND,
          "explicit empty password erased");

    CHECK(slate_store_wifi_set("slate-other-selftest", "temporary-secret", NULL) == ESP_OK &&
              slate_store_wifi_set("slate-new-selftest", NULL, NULL) == ESP_OK,
          "write a different SSID without password");
    CHECK(slate_store_wifi_password_get(kept_password, sizeof(kept_password)) ==
              ESP_ERR_NOT_FOUND,
          "password never crosses SSIDs");

    if (had_wifi) {
        const slate_ipv4_config_t *restore_ipv4 =
            saved_ipv4.state == SLATE_IPV4_DHCP ? NULL : &saved_ipv4;
        CHECK(slate_store_wifi_set(saved_ssid, had_password ? saved_password : "", restore_ipv4) ==
                  ESP_OK,
              "restore station tuple");
    } else {
        CHECK(slate_store_wifi_clear() == ESP_OK, "restore unconfigured station");
    }
    explicit_bzero(saved_password, sizeof(saved_password));

    ESP_LOGI(TAG, "selftest: %d failure(s)", s_failures);
}
#endif

/*
 * The boot report's network half. slate_setup now acts on two of these four and
 * presents §6.5's setup card on the screen; this stays because it is the only
 * place the two sides of the hand-off are visible in one log — an address
 * arriving and an access point going away, in the order they happened.
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
        slate_ui_network_connected();
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
 * knob remains as a development/provisioning override. #35 adds the ordinary
 * writer to the setup page; a build-time value is still useful for exercising
 * a secured access point from its very first boot.
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

    /* The clock explicitly supports initialization before the station. Doing
     * it here also installs the timezone/localtime lock before the API can
     * accept a configuration replacement on a live interface. */
    err = slate_time_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "clock degraded: %s — continuing", esp_err_to_name(err));
    }

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

    /* Direct publication and action delivery use this same station. Keep the
     * adapter's freshness status tied to it just as HA's connection is below:
     * a router outage must stale both providers, not only the one with its own
     * outbound WebSocket. */
    err = slate_direct_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "direct provider network lifecycle unavailable: %s — continuing",
                 esp_err_to_name(err));
    }

    /*
     * §4.3's `slate-<mac6>.local`, before the adapter below queries the
     * responder it starts. Here rather than in start_api() because mDNS needs
     * the default event loop and the interfaces, and both are this function's.
     * The responder attaches to the station and to the setup access point, so
     * the name works before the panel has ever joined a network.
     */
    err = slate_mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS degraded: %s — the panel answers at its address only",
                 esp_err_to_name(err));
    }

    /* The adapter registers its API routes before the radio starts, then joins
     * the Wi-Fi lifecycle here once the event loop and station exist. Reading
     * the current station snapshot inside slate_ha_start() closes the small
     * race between these adjacent initialisers. */
    err = slate_ha_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Home Assistant network lifecycle unavailable: %s — continuing",
                 esp_err_to_name(err));
    }

    /*
     * The number #55 asks for. S-2 measured 104 167 B of internal DMA-capable
     * memory free with the station alone (§6.2), and the setup access point's
     * second interface has to come out of what is left after this line — so it
     * is printed on every boot rather than measured once and written down.
     */
    ESP_LOGI(TAG, "free internal DMA-capable memory with the station up: %u B",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
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

    /* §4.2 shares this server but deliberately does not share HTTP bearer
     * authentication: browser WebSockets cannot set that header, so the token
     * is the first frame. Register it before other diagnostics so boot logs
     * from their initialisation enter the retained ring as well. */
    esp_err_t ws_err = slate_ws_init();
    if (ws_err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket diagnostics unavailable: %s — continuing",
                 esp_err_to_name(ws_err));
    }

    err = slate_control_api_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device controls unavailable: %s — continuing", esp_err_to_name(err));
    }

    err = slate_integrations_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "integration management unavailable: %s — continuing",
                 esp_err_to_name(err));
    }

    /*
     * §5.4's provider, and before the selftests below rather than after: both
     * report the provider table as they find it, and a check that ran while the
     * always-present provider had not registered yet would be describing the
     * order of these lines rather than the firmware.
     *
     * Degraded rather than fatal, like everything else here. A panel that cannot
     * accept published state is still a panel that must answer the API and take
     * the next firmware.
     */
    err = slate_direct_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "direct provider degraded: %s — continuing", esp_err_to_name(err));
    }

    /* §5.5's first production adapter. It registers even while unconfigured,
     * so an HA binding is a known provider with honest lifecycle status rather
     * than §3.3's missing-provider compatibility placeholder. */
    err = slate_ha_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Home Assistant provider degraded: %s — continuing",
                 esp_err_to_name(err));
    }

#ifdef SLATE_HA_SELFTEST
    slate_ha_selftest();
#endif

#ifdef SLATE_DIRECT_SELFTEST
    slate_direct_selftest();
#endif

#ifdef SLATE_WS_SELFTEST
    if (ws_err == ESP_OK) {
        slate_ws_selftest();
    }
#endif

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

    /* §11.3's other half of the same idea: OTA is how firmware gets onto a panel
     * that has no cable, and this is how the reason it crashed gets off one. The
     * boot line about what is on flash is not here — it belongs to the boot
     * report, and it has to survive an HTTP server that did not start. */
    err = slate_coredump_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "core dump retrieval unavailable: %s — continuing", esp_err_to_name(err));
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

    /*
     * §10's editor, and after the setup page rather than before it: the two
     * share `GET /` and slate_api decides between them by interface, so this
     * order only decides which of them is registered first — but a boot that
     * failed halfway should have left the panel with the page that puts it on
     * a network, not the one that arranges tiles on it.
     *
     * It also writes flash on the boots where the bundle changed, which is one
     * more reason for it to come after everything that has to be reachable.
     */
    err = slate_editor_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "editor unavailable: %s — the API is unaffected", esp_err_to_name(err));
    }
}

static void start_display(void)
{
    /* A display allocation or bus failure is not made recoverable by rebooting
     * into the same failure. Keep the API and setup access point alive so the
     * next firmware can still arrive without a cable; the backlight remains
     * off and the exact error stays in the boot log. */
    esp_err_t err = slate_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display degraded: %s — continuing headless", esp_err_to_name(err));
    }
}

static void start_brightness(void)
{
    esp_err_t err = slate_brightness_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "brightness policy degraded: %s — continuing",
                 esp_err_to_name(err));
    }
}

static void start_ui(void)
{
    esp_err_t err = slate_ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UI runtime degraded: %s — keeping the current screen",
                 esp_err_to_name(err));
    }

    /* The route composes the parser, runtime, store and WebSocket, so it is
     * registered only after all four boundaries exist. GET remains useful on
     * a degraded display, while PUT will report that activation failed. */
    err = slate_config_api_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configuration API degraded: %s — continuing",
                 esp_err_to_name(err));
    }

#ifdef SLATE_CONFIG_API_SELFTEST
    if (err == ESP_OK) {
        slate_config_api_selftest();
    }
#endif
}

void app_main(void)
{
    configure_json_allocator();

    /* §11.3's retained backlog starts before the boot report and board
     * bring-up, while its network transport still starts later with the API.
     * The capture half uses static storage, so it is safe before the store has
     * established whether this is a healthy or recovery boot. */
    esp_err_t capture_err = slate_ws_capture_init();
    if (capture_err != ESP_OK) {
        ESP_LOGE(TAG, "retained log capture unavailable: %s — continuing",
                 esp_err_to_name(capture_err));
    }

#ifdef SLATE_OTA_ROLLBACK_SELFTEST
    ota_rollback_selftest();
#endif

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

    /* §11.3, and here rather than beside the route it is fetched through: the
     * reset reason two lines above says a panic happened, and this says what
     * crashed and whether this firmware is the one that can explain it. Neither
     * needs a network, which is the point — the boot that cannot serve the dump
     * is the boot that most needs to describe it. */
    slate_coredump_report();

    ESP_LOGI(TAG, "home assistant %s",
             slate_store_ha_token_is_set() ? "configured" : "not configured");
    ESP_LOGI(TAG, "free heap %u B internal, %u B psram",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

#ifdef SLATE_STORE_SELFTEST
    store_selftest();
#endif

    provision_setup_ap();

    /*
     * §5.1's provider-neutral core, before anything that could register a
     * provider or bind a configuration. It allocates only its lock here: the
     * active configuration is what sizes the store (§5.1), and at this point in
     * the boot nothing has read one.
     *
     * Degraded rather than fatal, on the same reasoning as the store above it.
     * A panel whose dashboard cannot hold state is still a panel that must
     * answer the API and accept the next firmware.
     */
    esp_err_t state_err = slate_state_init();
    if (state_err != ESP_OK) {
        ESP_LOGE(TAG, "state store degraded: %s — continuing", esp_err_to_name(state_err));
    }

#ifdef SLATE_STATE_SELFTEST
    slate_state_selftest();
#endif

    /* §5.3's neutral bus observes provider publications and therefore comes
     * after the state store, but before adapters register in start_api(). */
    esp_err_t action_err = slate_action_init();
    if (action_err != ESP_OK) {
        ESP_LOGE(TAG, "action bus degraded: %s — continuing", esp_err_to_name(action_err));
    }

#ifdef SLATE_ACTION_SELFTEST
    if (action_err == ESP_OK) {
        slate_action_selftest();
    }
#endif

    /* Before the radio, so the task and its queue are already present when
     * slate_wifi begins posting the state changes that later screens consume.
     * S-2's done-when load is still real: the test pattern keeps animating
     * while the station or setup access point runs underneath it. */
    start_display();
    start_brightness();

    /* The API and the setup portal come up before the radio, so that the setup
     * access point has a page to serve and a subscriber in place by the time the
     * station has an opinion about whether one is needed. */
    start_api();
    start_ui();
    start_network();

#ifdef SLATE_TIME_SELFTEST
    esp_err_t time_test_err = time_selftest();
    if (time_test_err != ESP_OK) {
        ESP_LOGE(TAG, "timezone selftest failed: %s", esp_err_to_name(time_test_err));
    }
#endif

#ifdef SLATE_BRIGHTNESS_SELFTEST
    esp_err_t brightness_test_err = slate_brightness_selftest();
    if (brightness_test_err != ESP_OK) {
        ESP_LOGE(TAG, "brightness selftest failed: %s",
                 esp_err_to_name(brightness_test_err));
    }
#endif

    /* After start_network(), so a panel that genuinely came up in setup mode
     * has already raised its card and the verifier declines instead of
     * replacing §9.4's recovery address with a fixture. */
#ifdef SLATE_DISPLAY_SELFTEST
    esp_err_t setup_card_test_err = slate_display_selftest();
    if (setup_card_test_err == ESP_ERR_INVALID_STATE) {
        /* Declined rather than failed, and slate_display has already logged
         * which of the two reasons it was. */
        ESP_LOGW(TAG, "setup card selftest did not run");
    } else if (setup_card_test_err != ESP_OK) {
        ESP_LOGE(TAG, "setup card selftest failed: %s",
                 esp_err_to_name(setup_card_test_err));
    }
#endif

    /* Before the health check is armed, not after: the panic is supposed to
     * happen while the image is still unverified, and the health task runs at a
     * higher priority than this one — so leaving it to the scheduler would make
     * §11.2's rollback a race rather than the other half of the test. */
#ifdef SLATE_COREDUMP_SELFTEST
    coredump_selftest();
#endif

    /* Keep this independent of both initializers' return values. A pending
     * image whose HTTP server or radio failed to start is exactly the image the
     * 60-second health deadline must reject. */
    esp_err_t health_err = slate_ota_start_boot_health();
    if (health_err != ESP_OK) {
        ESP_LOGE(TAG, "OTA boot health unavailable: %s", esp_err_to_name(health_err));
    }

#ifdef SLATE_UI_SELFTEST
    if (slate_ui_ready()) {
        /* A full 500-cycle display test intentionally outlives an OTA health
         * budget. Let the independently scheduled network and health tasks
         * settle before taking heap baselines, while the API is already able
         * to prove this diagnostic image booted. */
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_err_t ui_test_err = slate_ui_selftest();
        if (ui_test_err != ESP_OK) {
            ESP_LOGE(TAG, "UI runtime selftest failed: %s", esp_err_to_name(ui_test_err));
        }
    }
#endif
}
