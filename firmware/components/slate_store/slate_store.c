/*
 * Slate — persistent store. See include/slate_store.h for the contract.
 *
 * design.md §4.3, §6.3, §12.
 */

#include "slate_store.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "bootloader_random.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "store";

/* Must match the `littlefs` row of firmware/partitions.csv (§6.3). */
#define SLATE_FS_PARTITION_LABEL "littlefs"

/* slate_store_config_write() renames this over SLATE_CONFIG_PATH. */
#define SLATE_CONFIG_TMP_PATH SLATE_FS_BASE_PATH "/config.tmp"

/*
 * Private keys, deliberately not in the header.
 *
 * §12's Home Assistant token and station passphrase are private so that the
 * generic accessors cannot name them — see key_is_secret(). KEY_FS_READY is
 * private because it is this component's own bookkeeping.
 */
#define KEY_HA_TOKEN  "ha_token"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_WIFI_REV  "wifi_rev"
#define KEY_FS_READY  "fs_ready"
#define KEY_ADMIN_AUTH "admin_auth"

#define ADMIN_AUTH_VERSION 1
#define ADMIN_PIN_SALT_LEN 16
#define ADMIN_PIN_HASH_LEN 32
#define ADMIN_PIN_ITERATIONS 50000

typedef struct {
    uint8_t version;
    uint8_t salt[ADMIN_PIN_SALT_LEN];
    uint8_t hash[ADMIN_PIN_HASH_LEN];
} admin_auth_record_t;

/*
 * §9.6's addressing, as one blob rather than a key per field.
 *
 * Private because nothing outside this component names it: the accessors take
 * a slate_ipv4_config_t and the encoding never leaves this file. That is what
 * makes the record a single NVS write — five keys would be five writes and four
 * power cuts that leave an address with somebody else's gateway, which is
 * exactly the configuration §9.6 exists to stop the panel from applying.
 */
#define KEY_IPV4 "ipv4"

/*
 * The stored form, versioned because it is the one record here that is not a
 * string and cannot be read leniently. A blob written by a firmware that
 * numbered these states differently would otherwise be applied as an address:
 * an unrecognised version reports DHCP, which is the addressing every panel can
 * fall back to.
 */
#define IPV4_RECORD_VERSION 2

typedef struct {
    uint8_t version;
    uint8_t state;
    uint8_t prefix;
    uint8_t dns_count;
    uint32_t generation;
    uint32_t address;
    uint32_t gateway;
    uint32_t dns[SLATE_IPV4_DNS_MAX];
} ipv4_record_t;

/*
 * The device token alphabet, and the reason it is exactly 64 characters long.
 *
 * The token is represented in JSON and HTTP headers, so every character should
 * be transport-safe without escaping. That rules out the
 * standard base64 alphabet's `+` and `/`, which leaves the URL-safe variant:
 * 26 + 26 + 10 + 2 = 64.
 *
 * 64 is also what makes the draw unbiased. Six bits index the table exactly, so
 * each character is equally likely. An alphabet of any other size needs modulo
 * on a byte, and modulo on a byte skews the low characters — a bias that is
 * invisible in the output and shortens the token's real entropy.
 *
 * 32 characters × 6 bits = 192 bits.
 */
static const char TOKEN_ALPHABET[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static char s_device_token[SLATE_DEVICE_TOKEN_LEN + 1];
static char s_device_id[SLATE_DEVICE_ID_LEN + 1];
static char s_device_name[SLATE_DEVICE_NAME_LEN + 1];

static uint32_t s_boot_count;
static bool s_ha_token_set;
static bool s_wifi_configured;
static bool s_storage_was_reset;
static bool s_fs_mounted;
static bool s_admin_pin_set;

/* See slate_store_set_rf_active(). False is the safe default and is true at
 * boot, which is when the token is minted. */
static volatile bool s_rf_active;

/*
 * One recursive mutex for the whole component.
 *
 * Recursive because the composite operations legitimately re-enter: a factory
 * reset mints a token, which takes the same lock. NVS has its own internal
 * lock, but it cannot make a read/compare/write across two handles atomic. This
 * one therefore also serialises the WiFi tuple with a static-address verdict:
 * a completed trial must not stamp its state onto a replacement configuration
 * that `POST /wifi` committed underneath it. It still protects the token
 * buffer, cached predicates and filesystem lifecycle, none of which anything
 * else serialises.
 */
static SemaphoreHandle_t s_lock;

#define LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

/* -------------------------------------------------------------------------
 * Error vocabulary
 * ------------------------------------------------------------------------- */

/*
 * NVS's error codes stop here. The header promises one spelling per condition
 * whichever partition a value lives on, for the same reason §4.1 gives the
 * network errors one vocabulary: a caller should not have to learn which
 * storage backend a setting happens to use in order to tell "unset" from
 * "broken". Without this, `GET /config` on a factory-fresh panel and
 * `GET /status` on one report the same fact with two different numbers, and
 * the first-run path in #10 turns into a 500.
 */
static esp_err_t from_nvs(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NVS_NOT_FOUND:
    case ESP_ERR_NVS_TYPE_MISMATCH:
        return ESP_ERR_NOT_FOUND;
    case ESP_ERR_NVS_INVALID_LENGTH:
        return ESP_ERR_INVALID_SIZE;
    default:
        return err;
    }
}

/* §12's secret is unreachable through the generic accessors. The restriction
 * has to be a mechanism rather than a doc comment, because the thing it guards
 * against is a serialiser looping over key names. */
static bool key_is_secret(const char *key)
{
    return strcmp(key, KEY_HA_TOKEN) == 0 || strcmp(key, KEY_WIFI_PASS) == 0 ||
           strcmp(key, KEY_ADMIN_AUTH) == 0;
}

/* -------------------------------------------------------------------------
 * Entropy
 * ------------------------------------------------------------------------- */

/*
 * esp_random() is a true RNG only while the RF subsystem is running. Before
 * that it is a PRNG, and a 32-character token drawn from a PRNG looks exactly
 * like a good one — there is no symptom, only a weaker secret than the header
 * claims.
 *
 * ESP-IDF's answer when RF is down is bootloader_random_enable(), which mixes
 * SAR ADC noise into the hardware RNG and is documented as callable from app
 * code for exactly this case. It comes with a condition: it must not be
 * enabled while the RF subsystem or the ADC IS running.
 *
 * So the store has to know which world it is in, and it is told rather than
 * left to infer it from when it was called. An earlier version keyed this on
 * "am I inside slate_store_init()", which was wrong in both directions: a
 * reissue driven from the panel's own screen (§4.3, §9.5) while WiFi is down
 * silently got the PRNG, and an init that ever moved after esp_wifi_init()
 * would silently make the unsafe call.
 */
static void fill_random_strong(void *buf, size_t len)
{
    if (s_rf_active) {
        esp_fill_random(buf, len);
        return;
    }

    bootloader_random_enable();
    esp_fill_random(buf, len);
    bootloader_random_disable();
}

void slate_store_set_rf_active(bool active)
{
    s_rf_active = active;
}

/* Caller holds the lock. */
static void mint_token(void)
{
    uint8_t raw[SLATE_DEVICE_TOKEN_LEN];

    fill_random_strong(raw, sizeof(raw));
    for (size_t i = 0; i < SLATE_DEVICE_TOKEN_LEN; i++) {
        s_device_token[i] = TOKEN_ALPHABET[raw[i] & 0x3F];
    }
    s_device_token[SLATE_DEVICE_TOKEN_LEN] = '\0';

    /* The token is a secret; the raw draw it came from is the same secret in a
     * different encoding, and it is on the stack of whatever task called us. */
    memset(raw, 0, sizeof(raw));
}

/* -------------------------------------------------------------------------
 * NVS helpers
 * ------------------------------------------------------------------------- */

static esp_err_t nvs_open_ns(nvs_open_mode_t mode, nvs_handle_t *out)
{
    return nvs_open(SLATE_NVS_NAMESPACE, mode, out);
}

/*
 * The commit-and-close rule, written once. Every writer below is open / act /
 * nvs_finish, so a fourth one cannot be copied from whichever of the others
 * the next author happens to read and silently inherit a different rule.
 */
static esp_err_t nvs_finish(nvs_handle_t nvs, esp_err_t err)
{
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/* Unchecked by key_is_secret(); the secret accessors go through these. */
static esp_err_t str_set_raw(const char *key, const char *value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_finish(nvs, nvs_set_str(nvs, key, value));
}

static esp_err_t str_get_raw(const char *key, char *out, size_t out_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return from_nvs(err);
    }

    size_t len = out_len;
    err = nvs_get_str(nvs, key, out, &len);
    nvs_close(nvs);
    return from_nvs(err);
}

static esp_err_t erase_raw(const char *key)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; /* erasing what is not there is not a failure */
    }
    return nvs_finish(nvs, err);
}

static bool key_exists(const char *key)
{
    nvs_handle_t nvs;
    if (nvs_open_ns(NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    /* Asking for the length rather than the value: the predicate never has the
     * secret in a buffer to leak. */
    size_t len = 0;
    esp_err_t err = nvs_get_str(nvs, key, NULL, &len);
    nvs_close(nvs);

    return err == ESP_OK && len > 1; /* len counts the terminator */
}

static esp_err_t admin_auth_read(admin_auth_record_t *record)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return from_nvs(err);
    }

    size_t len = sizeof(*record);
    err = nvs_get_blob(nvs, KEY_ADMIN_AUTH, record, &len);
    nvs_close(nvs);
    err = from_nvs(err);
    if (err == ESP_OK && (len != sizeof(*record) || record->version != ADMIN_AUTH_VERSION)) {
        explicit_bzero(record, sizeof(*record));
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

static bool admin_pin_valid(const char *pin)
{
    if (pin == NULL) {
        return false;
    }
    size_t len = strnlen(pin, SLATE_ADMIN_PIN_MAX_LEN + 1);
    if (len < SLATE_ADMIN_PIN_MIN_LEN || len > SLATE_ADMIN_PIN_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            return false;
        }
    }
    return true;
}

static esp_err_t derive_admin_pin(const char *pin, const uint8_t *salt, uint8_t *hash)
{
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256, (const unsigned char *) pin, strlen(pin), salt,
        ADMIN_PIN_SALT_LEN, ADMIN_PIN_ITERATIONS, ADMIN_PIN_HASH_LEN, hash);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

/* --- public settings API --- */

esp_err_t slate_store_str_set(const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (key_is_secret(key)) {
        return ESP_ERR_INVALID_ARG;
    }
    return str_set_raw(key, value);
}

esp_err_t slate_store_str_get(const char *key, char *out, size_t out_len)
{
    /* A NULL buffer is ESP-IDF's size-query idiom, and this wrapper cannot
     * carry the answer back — slate_store_str_size() is that. Refusing beats
     * returning ESP_OK for a read that produced nothing. */
    if (key == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (key_is_secret(key)) {
        return ESP_ERR_INVALID_ARG;
    }
    return str_get_raw(key, out, out_len);
}

esp_err_t slate_store_str_size(const char *key, size_t *out_size)
{
    if (key == NULL || out_size == NULL || key_is_secret(key)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return from_nvs(err);
    }

    size_t len = 0;
    err = nvs_get_str(nvs, key, NULL, &len);
    nvs_close(nvs);

    if (err == ESP_OK) {
        *out_size = len;
    }
    return from_nvs(err);
}

esp_err_t slate_store_u32_set(const char *key, uint32_t value)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_finish(nvs, nvs_set_u32(nvs, key, value));
}

esp_err_t slate_store_u32_get(const char *key, uint32_t *out)
{
    if (key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return from_nvs(err);
    }

    err = nvs_get_u32(nvs, key, out);
    nvs_close(nvs);
    return from_nvs(err);
}

esp_err_t slate_store_erase(const char *key)
{
    if (key == NULL || key_is_secret(key)) {
        return ESP_ERR_INVALID_ARG;
    }
    return erase_raw(key);
}

uint32_t slate_store_boot_count(void)
{
    return s_boot_count;
}

/* -------------------------------------------------------------------------
 * Device identity
 * ------------------------------------------------------------------------- */

/*
 * Derived, never stored, and computed before anything touches flash — §6.5's
 * error screen and §9.2's access point need a name to show precisely when the
 * store is the thing that is broken, so this must not sit behind a successful
 * mount.
 */
static void derive_device_name(void)
{
    uint8_t mac[6];

    /* The base MAC, not an interface's: §9.2 wants the same suffix whether the
     * panel is on its own access point or on the router's network. */
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BASE);
    if (err != ESP_OK) {
        /* Nothing sensible to fall back to, and a panel called `slate-000000`
         * is at least a panel that boots and says so on screen. */
        ESP_LOGE(TAG, "esp_read_mac: %s", esp_err_to_name(err));
        memset(mac, 0, sizeof(mac));
    }

    snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_device_name, sizeof(s_device_name), "slate-%s", s_device_id);
}

const char *slate_store_device_id(void)
{
    return s_device_id;
}

const char *slate_store_device_name(void)
{
    return s_device_name;
}

/* -------------------------------------------------------------------------
 * Device token
 * ------------------------------------------------------------------------- */

/* Caller holds the lock. */
static esp_err_t load_or_mint_device_token(void)
{
    esp_err_t err = str_get_raw(SLATE_KEY_DEVICE_TOKEN, s_device_token, sizeof(s_device_token));

    if (err == ESP_OK && strlen(s_device_token) == SLATE_DEVICE_TOKEN_LEN) {
        return ESP_OK;
    }

    if (err == ESP_OK) {
        /* Stored but the wrong length: written by an older firmware, or a
         * truncated write. Either way it is not a token this firmware issued. */
        ESP_LOGW(TAG, "stored device token has unexpected length — minting a new one");
    } else if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "reading device token: %s — minting a new one", esp_err_to_name(err));
    }

    mint_token();

    err = str_set_raw(SLATE_KEY_DEVICE_TOKEN, s_device_token);
    if (err != ESP_OK) {
        /* The token is live in RAM either way, so the API still works this
         * boot; it just will not survive a reboot. Reporting that beats
         * refusing to boot (§9). */
        ESP_LOGE(TAG, "persisting device token: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "minted a new device token");
    return ESP_OK;
}

esp_err_t slate_store_device_token_copy(char *out, size_t out_len)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_len < SLATE_DEVICE_TOKEN_LEN + 1) {
        return ESP_ERR_INVALID_SIZE;
    }

    LOCK();
    memcpy(out, s_device_token, SLATE_DEVICE_TOKEN_LEN + 1);
    UNLOCK();
    return ESP_OK;
}

esp_err_t slate_store_device_token_fingerprint(char *out, size_t out_len)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_len < SLATE_TOKEN_FINGERPRINT_LEN + 1) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[32];

    LOCK();
    int rc = mbedtls_sha256((const unsigned char *) s_device_token, SLATE_DEVICE_TOKEN_LEN,
                            digest, 0);
    UNLOCK();

    if (rc != 0) {
        return ESP_FAIL;
    }

    for (size_t i = 0; i < SLATE_TOKEN_FINGERPRINT_LEN / 2; i++) {
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    }
    return ESP_OK;
}

bool slate_store_device_token_matches(const char *candidate)
{
    if (candidate == NULL) {
        return false;
    }

    /*
     * Constant time in the token's length, not the candidate's: the candidate
     * is copied into a fixed buffer first, then compared with mbedtls's
     * constant-time primitive rather than a hand-rolled loop, so the property
     * survives a toolchain that decides to vectorise or short-circuit.
     *
     * The length is folded into the same result, so a wrong token costs the
     * same regardless of how many leading characters it got right — the thing
     * a byte-at-a-time strcmp gives away to anyone who can time a 401.
     */
    char padded[SLATE_DEVICE_TOKEN_LEN];
    size_t candidate_len = strnlen(candidate, SLATE_DEVICE_TOKEN_LEN + 1);

    memset(padded, 0, sizeof(padded));
    memcpy(padded, candidate, candidate_len < sizeof(padded) ? candidate_len : sizeof(padded));

    LOCK();
    int diff = mbedtls_ct_memcmp(padded, s_device_token, sizeof(padded));
    UNLOCK();

    return diff == 0 && candidate_len == SLATE_DEVICE_TOKEN_LEN;
}

esp_err_t slate_store_device_token_reissue(void)
{
    LOCK();

    /*
     * RAM first, then flash. A reissue is a request to stop honouring the old
     * token, so if only one of the two can happen it must be the one that
     * takes effect now: a device that answers to the new token and forgets it
     * on reboot is recoverable, one that keeps answering to a token the owner
     * believes they revoked is not.
     */
    mint_token();
    esp_err_t err = str_set_raw(SLATE_KEY_DEVICE_TOKEN, s_device_token);

    UNLOCK();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persisting reissued token: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "device token reissued");
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Web editor PIN
 * ------------------------------------------------------------------------- */

bool slate_store_admin_pin_is_set(void)
{
    LOCK();
    bool set = s_admin_pin_set;
    UNLOCK();
    return set;
}

esp_err_t slate_store_admin_pin_set(const char *pin)
{
    if (!admin_pin_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    admin_auth_record_t record = {.version = ADMIN_AUTH_VERSION};
    LOCK();
    fill_random_strong(record.salt, sizeof(record.salt));
    esp_err_t err = derive_admin_pin(pin, record.salt, record.hash);
    if (err == ESP_OK) {
        nvs_handle_t nvs;
        err = nvs_open_ns(NVS_READWRITE, &nvs);
        if (err == ESP_OK) {
            err = nvs_finish(nvs, nvs_set_blob(nvs, KEY_ADMIN_AUTH, &record, sizeof(record)));
        }
    }
    if (err == ESP_OK) {
        s_admin_pin_set = true;
    }
    UNLOCK();
    explicit_bzero(&record, sizeof(record));
    return err;
}

esp_err_t slate_store_admin_pin_clear(void)
{
    LOCK();
    esp_err_t err = erase_raw(KEY_ADMIN_AUTH);
    if (err == ESP_OK) {
        s_admin_pin_set = false;
    }
    UNLOCK();
    return err;
}

bool slate_store_admin_pin_matches(const char *candidate)
{
    if (!admin_pin_valid(candidate)) {
        return false;
    }

    admin_auth_record_t record = {0};
    uint8_t candidate_hash[ADMIN_PIN_HASH_LEN] = {0};
    LOCK();
    esp_err_t err = admin_auth_read(&record);
    if (err == ESP_OK) {
        err = derive_admin_pin(candidate, record.salt, candidate_hash);
    }
    int diff = err == ESP_OK ? mbedtls_ct_memcmp(candidate_hash, record.hash,
                                                 sizeof(candidate_hash))
                             : 1;
    UNLOCK();

    explicit_bzero(candidate_hash, sizeof(candidate_hash));
    explicit_bzero(&record, sizeof(record));
    return diff == 0;
}

/* -------------------------------------------------------------------------
 * Secrets: Home Assistant credentials and the WiFi passphrase (§12)
 * ------------------------------------------------------------------------- */

esp_err_t slate_store_ha_set(const char *url, const char *token)
{
    if (url == NULL || token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(url) >= SLATE_HA_URL_MAX_LEN || strlen(token) >= SLATE_HA_TOKEN_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* One handle, one commit: a power cut must not leave a URL with no token,
     * which is a configured-looking device that cannot connect. */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, SLATE_KEY_HA_URL, url);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, KEY_HA_TOKEN, token);
    }
    err = nvs_finish(nvs, err);

    if (err == ESP_OK) {
        LOCK();
        s_ha_token_set = true;
        UNLOCK();
    }
    return err;
}

esp_err_t slate_store_ha_url_get(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return str_get_raw(SLATE_KEY_HA_URL, out, out_len);
}

bool slate_store_ha_token_is_set(void)
{
    /* Cached: §4.1 makes this a `GET /status` field, and /status is polled by
     * the editor's connection indicator. A predicate that opens NVS is an
     * allocation and a flash read every few seconds, forever, for a boolean
     * that only two functions in this file can change. */
    return s_ha_token_set;
}

esp_err_t slate_store_ha_token_get(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return str_get_raw(KEY_HA_TOKEN, out, out_len);
}

esp_err_t slate_store_ha_clear(void)
{
    /* Both attempted before either is reported, so a failure on the URL cannot
     * leave the token behind. */
    esp_err_t url_err = erase_raw(SLATE_KEY_HA_URL);
    esp_err_t token_err = erase_raw(KEY_HA_TOKEN);

    if (token_err == ESP_OK) {
        LOCK();
        s_ha_token_set = false;
        UNLOCK();
    }
    return url_err != ESP_OK ? url_err : token_err;
}

/* -------------------------------------------------------------------------
 * Station addressing (§9.6)
 * ------------------------------------------------------------------------- */

const char *slate_ipv4_state_str(slate_ipv4_state_t state)
{
    switch (state) {
    case SLATE_IPV4_DHCP:                   return "dhcp";
    case SLATE_IPV4_STATIC_PENDING:         return "pending";
    case SLATE_IPV4_STATIC_CONFIRMED:       return "confirmed";
    case SLATE_IPV4_STATIC_GATEWAY_UNREACHABLE: return "gateway_unreachable";
    case SLATE_IPV4_STATIC_ADDRESS_IN_USE:  return "address_in_use";
    }
    return "dhcp";
}

bool slate_ipv4_state_is_applied(slate_ipv4_state_t state)
{
    /* §9.6: a configuration that has failed its trial stays stored and stops
     * being used. Written as the affirmative list rather than "not one of the
     * failures", so a state added later has to be classified deliberately
     * instead of being applied to the network by default. */
    return state == SLATE_IPV4_STATIC_PENDING || state == SLATE_IPV4_STATIC_CONFIRMED;
}

/** Read the record, or report DHCP. NULL `out` is a valid existence check. */
static bool ipv4_record_read(ipv4_record_t *out)
{
    nvs_handle_t nvs;
    if (nvs_open_ns(NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    ipv4_record_t record = {0};
    size_t len = sizeof(record);
    esp_err_t err = nvs_get_blob(nvs, KEY_IPV4, &record, &len);
    nvs_close(nvs);

    if (err != ESP_OK || len != sizeof(record) || record.version != IPV4_RECORD_VERSION) {
        return false;
    }

    /*
     * A record that says DHCP is not one — the absence of a record is how DHCP
     * is stored, and anything claiming otherwise did not come from
     * slate_store_wifi_set(). Same for a prefix outside what `POST /wifi`
     * accepts: this is the value that decides what subnet the panel thinks it
     * is on, and a blob whose bytes moved must not be able to answer that.
     */
    if (!slate_ipv4_state_is_applied(record.state) &&
        record.state != SLATE_IPV4_STATIC_GATEWAY_UNREACHABLE &&
        record.state != SLATE_IPV4_STATIC_ADDRESS_IN_USE) {
        return false;
    }
    if (record.generation == 0 || record.prefix < 8 || record.prefix > 30 ||
        record.dns_count > SLATE_IPV4_DNS_MAX) {
        return false;
    }

    if (out != NULL) {
        *out = record;
    }
    return true;
}

void slate_store_ipv4_get(slate_ipv4_config_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    ipv4_record_t record;
    LOCK();
    bool found = ipv4_record_read(&record);
    UNLOCK();
    if (!found) {
        out->state = SLATE_IPV4_DHCP;
        return;
    }

    out->state = (slate_ipv4_state_t) record.state;
    out->address = record.address;
    out->gateway = record.gateway;
    out->generation = record.generation;
    out->prefix = record.prefix;
    out->dns_count = record.dns_count;
    memcpy(out->dns, record.dns, sizeof(out->dns));
}

/** Write the record, or erase it for DHCP. The caller holds the store lock. */
static esp_err_t ipv4_record_write(nvs_handle_t nvs, const slate_ipv4_config_t *ipv4,
                                   uint32_t generation)
{
    if (ipv4 == NULL || ipv4->state == SLATE_IPV4_DHCP) {
        esp_err_t err = nvs_erase_key(nvs, KEY_IPV4);
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }

    ipv4_record_t record = {
        .version = IPV4_RECORD_VERSION,
        .state = (uint8_t) ipv4->state,
        .prefix = ipv4->prefix,
        .dns_count = ipv4->dns_count > SLATE_IPV4_DNS_MAX ? SLATE_IPV4_DNS_MAX : ipv4->dns_count,
        .generation = generation,
        .address = ipv4->address,
        .gateway = ipv4->gateway,
    };
    memcpy(record.dns, ipv4->dns, sizeof(record.dns));

    return nvs_set_blob(nvs, KEY_IPV4, &record, sizeof(record));
}

esp_err_t slate_store_ipv4_set_state(const slate_ipv4_config_t *tried, slate_ipv4_state_t state)
{
    if (tried == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state != SLATE_IPV4_STATIC_CONFIRMED &&
        state != SLATE_IPV4_STATIC_GATEWAY_UNREACHABLE &&
        state != SLATE_IPV4_STATIC_ADDRESS_IN_USE) {
        /* Not a way to switch a panel to DHCP. §9.6 keeps a failed configuration
         * stored so the setup page can pre-fill it, and the only thing that
         * erases one is somebody asking for DHCP through `POST /wifi`. */
        return ESP_ERR_INVALID_ARG;
    }

    LOCK();

    ipv4_record_t record;
    if (!ipv4_record_read(&record)) {
        UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }

    /* The generation is the identity; the fields are checked as a defensive
     * invariant so a corrupted or partially copied snapshot is never stamped.
     * The state is deliberately excluded because changing it is this call's
     * whole purpose. */
    if (record.generation != tried->generation || record.address != tried->address ||
        record.gateway != tried->gateway ||
        record.prefix != tried->prefix || record.dns_count != tried->dns_count ||
        memcmp(record.dns, tried->dns, sizeof(record.dns)) != 0) {
        UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }

    if (record.state == (uint8_t) state) {
        /* The confirmed case on every reconnect of a sticky configuration.
         * Flash has a finite number of erases and this is the hot path. */
        UNLOCK();
        return ESP_OK;
    }
    record.state = (uint8_t) state;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        UNLOCK();
        return err;
    }
    err = nvs_finish(nvs, nvs_set_blob(nvs, KEY_IPV4, &record, sizeof(record)));
    UNLOCK();
    return err;
}

esp_err_t slate_store_wifi_set(const char *ssid, const char *password,
                               const slate_ipv4_config_t *ipv4)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) >= SLATE_WIFI_SSID_BUF_LEN ||
        (password != NULL && strlen(password) >= SLATE_WIFI_PASSWORD_BUF_LEN)) {
        return ESP_ERR_INVALID_SIZE;
    }

    LOCK();
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        UNLOCK();
        return err;
    }

    /* Identity for the whole WiFi tuple, not only for records that happen to be
     * static. It survives DHCP and DELETE /wifi, closing the ABA case where a
     * pending trial is erased and an identical static record is submitted
     * before its old verdict lands. The increment is in the tuple's one NVS
     * commit, so readers can never see the new generation with old fields. */
    uint32_t generation = 0;
    esp_err_t generation_err = nvs_get_u32(nvs, KEY_WIFI_REV, &generation);
    if (generation_err == ESP_ERR_NVS_NOT_FOUND) {
        generation = 0;
    } else if (generation_err != ESP_OK) {
        nvs_close(nvs);
        UNLOCK();
        return generation_err;
    }
    generation++;
    if (generation == 0) {
        generation = 1;
    }
    err = nvs_set_u32(nvs, KEY_WIFI_REV, generation);

    bool keep_password = false;
    if (password == NULL) {
        char current_ssid[SLATE_WIFI_SSID_BUF_LEN] = {0};
        size_t current_len = sizeof(current_ssid);
        keep_password = nvs_get_str(nvs, SLATE_KEY_WIFI_SSID, current_ssid, &current_len) ==
                            ESP_OK &&
                        strcmp(current_ssid, ssid) == 0;
    }

    if (err == ESP_OK) {
        err = nvs_set_str(nvs, SLATE_KEY_WIFI_SSID, ssid);
    }
    if (err == ESP_OK) {
        /* Missing and empty are deliberately different. Missing preserves the
         * secret only for this same SSID; empty is an explicit open network.
         * Both a new SSID with no password and an explicit empty string erase
         * the old key, so a passphrase can never leak across networks. */
        if (password == NULL && keep_password) {
            /* The key already in this transaction is exactly the wanted one. */
        } else if (password == NULL || password[0] == '\0') {
            err = nvs_erase_key(nvs, KEY_WIFI_PASS);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        } else {
            err = nvs_set_str(nvs, KEY_WIFI_PASS, password);
        }
    }
    if (err == ESP_OK) {
        err = ipv4_record_write(nvs, ipv4, generation);
    }
    err = nvs_finish(nvs, err);

    if (err == ESP_OK) {
        s_wifi_configured = true;
    }
    UNLOCK();
    return err;
}

esp_err_t slate_store_wifi_ssid_get(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return str_get_raw(SLATE_KEY_WIFI_SSID, out, out_len);
}

bool slate_store_wifi_is_configured(void)
{
    /* Cached for the reason slate_store_ha_token_is_set() is: §9.4 asks this
     * on every pass through the retry loop. */
    return s_wifi_configured;
}

esp_err_t slate_store_wifi_password_get(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return str_get_raw(KEY_WIFI_PASS, out, out_len);
}

esp_err_t slate_store_wifi_clear(void)
{
    /* Secret first, and all three attempted before any is reported — the order
     * slate_store_ha_clear() uses, for the same reason. */
    LOCK();
    esp_err_t pass_err = erase_raw(KEY_WIFI_PASS);
    esp_err_t ssid_err = erase_raw(SLATE_KEY_WIFI_SSID);
    esp_err_t ipv4_err = erase_raw(KEY_IPV4);

    if (ssid_err == ESP_OK) {
        s_wifi_configured = false;
    }
    UNLOCK();
    if (pass_err != ESP_OK) {
        return pass_err;
    }
    return ssid_err != ESP_OK ? ssid_err : ipv4_err;
}

/* -------------------------------------------------------------------------
 * UI configuration
 * ------------------------------------------------------------------------- */

bool slate_store_config_exists(void)
{
    struct stat st;
    return s_fs_mounted && stat(SLATE_CONFIG_PATH, &st) == 0 && st.st_size > 0;
}

esp_err_t slate_store_config_read(char **out, size_t *out_len)
{
    if (out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_fs_mounted) {
        return ESP_ERR_NOT_FOUND;
    }

    LOCK();

    esp_err_t result;
    char *buf = NULL;
    struct stat st;

    if (stat(SLATE_CONFIG_PATH, &st) != 0 || st.st_size == 0) {
        /* Zero bytes is "nothing stored", the same answer
         * slate_store_config_exists() gives — the two must not disagree, or a
         * factory-fresh panel is unconfigured to one caller and holding an
         * unparseable document to the next. */
        result = ESP_ERR_NOT_FOUND;
        goto done;
    }
    if (st.st_size > SLATE_CONFIG_MAX_BYTES) {
        /* Larger than §3.1 allows, so no writer of ours produced it. */
        ESP_LOGE(TAG, "stored configuration is %ld B, over the 64 KB limit", (long) st.st_size);
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    FILE *f = fopen(SLATE_CONFIG_PATH, "rb");
    if (f == NULL) {
        result = ESP_FAIL;
        goto done;
    }

    /* PSRAM when there is any: 64 KB is most of what S-2 measured free in
     * internal DMA-capable memory once WiFi is up (§6.2). Internal RAM when
     * there is not — failing a 2 KB read with 100 KB free would be a worse
     * trade than the one this is avoiding. */
    buf = heap_caps_malloc_prefer((size_t) st.st_size + 1, 2,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(f);
        result = ESP_ERR_NO_MEM;
        goto done;
    }

    size_t read = fread(buf, 1, (size_t) st.st_size, f);
    fclose(f);

    if (read != (size_t) st.st_size) {
        free(buf);
        buf = NULL;
        result = ESP_FAIL;
        goto done;
    }

    buf[read] = '\0';
    *out = buf;
    *out_len = read;
    result = ESP_OK;

done:
    UNLOCK();
    return result;
}

esp_err_t slate_store_config_write(const char *json, size_t len)
{
    if (json == NULL || len == 0) {
        /* An empty file is not a configuration. Accepting one produces a state
         * config_exists() and config_read() would have to agree about, and the
         * cheapest way to keep them in agreement is never to create it. */
        return ESP_ERR_INVALID_ARG;
    }
    if (len > SLATE_CONFIG_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_fs_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The temp path is a fixed name, so two concurrent writers would otherwise
     * interleave into one file and rename the mixture into place. */
    LOCK();

    esp_err_t result = ESP_OK;
    FILE *f = fopen(SLATE_CONFIG_TMP_PATH, "wb");
    if (f == NULL) {
        result = ESP_FAIL;
        goto done;
    }

    size_t written = fwrite(json, 1, len, f);
    /* fsync before the rename, or the rename can be durable while the bytes it
     * points at are not — which is the failure this whole dance exists for. */
    int flushed = fflush(f);
    int synced = (flushed == 0) ? fsync(fileno(f)) : -1;
    fclose(f);

    if (written != len || flushed != 0 || synced != 0) {
        unlink(SLATE_CONFIG_TMP_PATH);
        result = ESP_FAIL;
        goto done;
    }

    if (rename(SLATE_CONFIG_TMP_PATH, SLATE_CONFIG_PATH) != 0) {
        unlink(SLATE_CONFIG_TMP_PATH);
        result = ESP_FAIL;
    }

done:
    UNLOCK();
    return result;
}

esp_err_t slate_store_config_erase(void)
{
    if (!s_fs_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    LOCK();
    /* errno rather than a second stat(): "already gone" is what the syscall
     * just told us, and re-deriving it from a size heuristic reported a failed
     * unlink as success for a zero-byte file. */
    esp_err_t err = (unlink(SLATE_CONFIG_PATH) == 0 || errno == ENOENT) ? ESP_OK : ESP_FAIL;
    UNLOCK();
    return err;
}

esp_err_t slate_store_fs_usage(size_t *total_bytes, size_t *used_bytes)
{
    if (!s_fs_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_littlefs_info(SLATE_FS_PARTITION_LABEL, total_bytes, used_bytes);
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /*
         * A partition that cannot be opened is erased rather than treated as
         * fatal. The alternative is a panel that will not boot until someone
         * brings a USB cable, which is the failure M1 exists to remove.
         *
         * The erase is checked rather than ESP_ERROR_CHECK'd: aborting here
         * would produce exactly the boot loop this branch exists to prevent,
         * since the next boot takes the identical path.
         */
        ESP_LOGW(TAG, "NVS unusable (%s) — erasing", esp_err_to_name(err));
        s_storage_was_reset = true;

        esp_err_t erase_err = nvs_flash_erase();
        if (erase_err != ESP_OK) {
            ESP_LOGE(TAG, "erasing NVS: %s", esp_err_to_name(erase_err));
            return erase_err;
        }
        err = nvs_flash_init();
    }

    return err;
}

static esp_err_t register_fs(bool allow_format)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = SLATE_FS_BASE_PATH,
        .partition_label = SLATE_FS_PARTITION_LABEL,
        .format_if_mount_failed = allow_format,
        .dont_mount = false,
    };

    return esp_vfs_littlefs_register(&conf);
}

/*
 * Mount in two phases so the difference between "first boot" and "your
 * dashboard just got erased" is visible.
 *
 * Reformatting on a failed mount is the right policy — §9.2 already expects an
 * empty filesystem after a first flash or a bad OTA, and refusing to mount
 * would strand the panel. What was missing is the signal: esp_littlefs logs the
 * same "mount failed, formatting" line either way, so a firmware update that
 * changed the on-disk geometry would eat a configuration and read as a normal
 * first boot. A marker in NVS — which survives the format, being on the other
 * partition — is what tells the two apart.
 */
static esp_err_t mount_fs(void)
{
    bool was_provisioned = key_exists(KEY_FS_READY);

    esp_err_t err = register_fs(false);
    if (err != ESP_OK) {
        if (was_provisioned) {
            ESP_LOGE(TAG,
                     "LittleFS did not mount (%s) and this device had a filesystem — "
                     "reformatting, the stored configuration is lost",
                     esp_err_to_name(err));
            s_storage_was_reset = true;
        } else {
            ESP_LOGI(TAG, "no filesystem yet — formatting");
        }

        err = register_fs(true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mounting LittleFS: %s", esp_err_to_name(err));
            return err;
        }
    }

    s_fs_mounted = true;

    if (!was_provisioned) {
        esp_err_t marker_err = str_set_raw(KEY_FS_READY, "1");
        if (marker_err != ESP_OK) {
            ESP_LOGW(TAG, "recording the filesystem marker: %s", esp_err_to_name(marker_err));
        }
    }

    /* A crash between the write and the rename in config_write leaves this
     * behind; nothing else ever removes it. */
    unlink(SLATE_CONFIG_TMP_PATH);

    return ESP_OK;
}

/* §4.1 puts a reboot counter in `GET /status` next to the reset reason, because
 * the two together distinguish a panic from a power cut without a cable. It is
 * incremented here rather than in app_main so that it cannot be skipped by an
 * early return added above the call site later. */
static void bump_boot_count(void)
{
    uint32_t count = 0;

    esp_err_t err = slate_store_u32_get(SLATE_KEY_BOOT_COUNT, &count);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "reading boot count: %s", esp_err_to_name(err));
    }

    count++;
    err = slate_store_u32_set(SLATE_KEY_BOOT_COUNT, count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "writing boot count: %s", esp_err_to_name(err));
    }

    s_boot_count = count;
}

esp_err_t slate_store_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Before anything that can fail: §6.5's error screen and §9.2's access
     * point need a device name most when storage is what is broken. */
    derive_device_name();

    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable: %s", esp_err_to_name(err));
        /* Still give the API a token to authenticate against this boot. It
         * will not survive a reboot; open sessions still work for this boot. */
        LOCK();
        mint_token();
        UNLOCK();
        return err;
    }

    LOCK();
    esp_err_t token_err = load_or_mint_device_token();
    UNLOCK();

    s_ha_token_set = key_exists(KEY_HA_TOKEN);
    s_wifi_configured = key_exists(SLATE_KEY_WIFI_SSID);
    admin_auth_record_t admin_auth = {0};
    s_admin_pin_set = admin_auth_read(&admin_auth) == ESP_OK;
    explicit_bzero(&admin_auth, sizeof(admin_auth));

    bump_boot_count();

    esp_err_t fs_err = mount_fs();

    return token_err != ESP_OK ? token_err : fs_err;
}

bool slate_store_storage_was_reset(void)
{
    return s_storage_was_reset;
}

esp_err_t slate_store_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset");

    LOCK();

    esp_err_t first_err = ESP_OK;
    esp_err_t err;

/* Best effort: every step runs even if an earlier one failed, and the first
 * error is what comes back. An early return here leaves a half-reset device,
 * and the half that survives would be the secrets. */
#define STEP(expr, what)                                                     \
    do {                                                                     \
        err = (expr);                                                        \
        if (err != ESP_OK) {                                                 \
            ESP_LOGE(TAG, "factory reset, %s: %s", what, esp_err_to_name(err)); \
            if (first_err == ESP_OK) {                                       \
                first_err = err;                                             \
            }                                                                \
        }                                                                    \
    } while (0)

    /*
     * Secrets first. If a reset is interrupted, the thing that must already be
     * gone is the Home Assistant token and the WiFi passphrase (§12); the
     * configuration is a document §10 expects people to export and share, so
     * losing it later in the sequence is the cheaper half to get wrong.
     */
    err = nvs_flash_deinit();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_INITIALIZED) {
        STEP(err, "deinitialising NVS");
    }
    STEP(nvs_flash_erase(), "erasing NVS");
    STEP(init_nvs(), "reinitialising NVS");

    s_ha_token_set = false;
    s_wifi_configured = false;
    s_admin_pin_set = false;
    s_boot_count = 0;

    /* Then the filesystem. */
    if (s_fs_mounted) {
        err = esp_vfs_littlefs_unregister(SLATE_FS_PARTITION_LABEL);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            STEP(err, "unmounting LittleFS");
        }
        s_fs_mounted = false;
    }
    STEP(esp_littlefs_format(SLATE_FS_PARTITION_LABEL), "formatting LittleFS");

    /* §4.3: a new token is issued after a factory reset. Unconditional, and
     * before the remount, so the old token stops being accepted even if
     * everything below it fails. */
    STEP(slate_store_device_token_reissue(), "reissuing the device token");

    STEP(mount_fs(), "remounting LittleFS");

#undef STEP

    /* This reset was deliberate, so it is not the accident
     * slate_store_storage_was_reset() exists to report. */
    s_storage_was_reset = false;

    UNLOCK();

    ESP_LOGW(TAG, "factory reset complete — the device must be rebooted");
    return first_err;
}
