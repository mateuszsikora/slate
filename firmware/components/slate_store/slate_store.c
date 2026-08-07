/*
 * Slate — persistent store. See include/slate_store.h for the contract.
 *
 * design.md §4.3, §6.3, §12.
 */

#include "slate_store.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bootloader_random.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "store";

/* Must match the `littlefs` row of firmware/partitions.csv (§6.3). */
#define SLATE_FS_PARTITION_LABEL "littlefs"

/* slate_store_config_write() renames this over SLATE_CONFIG_PATH. */
#define SLATE_CONFIG_TMP_PATH SLATE_FS_BASE_PATH "/config.tmp"

/*
 * The device token alphabet, and the reason it is exactly 64 characters long.
 *
 * §4.3 delivers the token in a URL — `http://192.168.1.42/?t=Xk7p...` — so
 * every character has to survive a query string untouched. That rules out the
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

/*
 * True only while slate_store_init() runs, and it decides how the token is
 * drawn — see fill_random_strong().
 */
static bool s_in_early_boot;

/* -------------------------------------------------------------------------
 * Entropy
 * ------------------------------------------------------------------------- */

/*
 * esp_random() is a true RNG only once the RF subsystem is running. Before
 * that it is a PRNG, and a 32-character token drawn from a PRNG looks exactly
 * like a good one — there is no symptom, only a weaker secret than the header
 * claims.
 *
 * The device token is minted on first boot, which is precisely before WiFi
 * starts, so it lands in that window. ESP-IDF's answer is
 * bootloader_random_enable(), which mixes SAR ADC noise into the hardware RNG
 * and is documented as callable from app code for exactly this case. It comes
 * with a condition: it must be disabled again before the RF subsystem or the
 * ADC is initialised. slate_store_init() is specified to run before both, so
 * the bracket opens and closes inside this file and nobody else has to know.
 *
 * A reissue at runtime (§4.3, or after a factory reset) happens with the radio
 * already up — the request arrived over the network — so esp_random() is
 * already the true RNG there and enabling the bootloader source would be the
 * unsafe call the header warns about. Hence the flag rather than an
 * unconditional bracket.
 */
static void fill_random_strong(void *buf, size_t len)
{
    if (s_in_early_boot) {
        bootloader_random_enable();
        esp_fill_random(buf, len);
        bootloader_random_disable();
    } else {
        esp_fill_random(buf, len);
    }
}

static void generate_token(char out[SLATE_DEVICE_TOKEN_LEN + 1])
{
    uint8_t raw[SLATE_DEVICE_TOKEN_LEN];

    fill_random_strong(raw, sizeof(raw));
    for (size_t i = 0; i < SLATE_DEVICE_TOKEN_LEN; i++) {
        out[i] = TOKEN_ALPHABET[raw[i] & 0x3F];
    }
    out[SLATE_DEVICE_TOKEN_LEN] = '\0';

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

esp_err_t slate_store_str_set(const char *key, const char *value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t slate_store_str_get(const char *key, char *out, size_t out_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = out_len;
    err = nvs_get_str(nvs, key, out, &len);
    nvs_close(nvs);
    return err;
}

esp_err_t slate_store_u32_set(const char *key, uint32_t value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u32(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t slate_store_u32_get(const char *key, uint32_t *out)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_ns(NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u32(nvs, key, out);
    nvs_close(nvs);
    return err;
}

esp_err_t slate_store_erase(const char *key)
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
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/* -------------------------------------------------------------------------
 * Device identity
 * ------------------------------------------------------------------------- */

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

static esp_err_t load_or_mint_device_token(void)
{
    size_t len = sizeof(s_device_token);
    esp_err_t err = slate_store_str_get(SLATE_KEY_DEVICE_TOKEN, s_device_token, len);

    if (err == ESP_OK && strlen(s_device_token) == SLATE_DEVICE_TOKEN_LEN) {
        return ESP_OK;
    }

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "reading device token: %s — minting a new one", esp_err_to_name(err));
    } else if (err == ESP_OK) {
        /* Stored but the wrong length: written by an older firmware, or a
         * truncated write. Either way it is not a token this firmware issued. */
        ESP_LOGW(TAG, "stored device token has unexpected length — minting a new one");
    }

    generate_token(s_device_token);
    err = slate_store_str_set(SLATE_KEY_DEVICE_TOKEN, s_device_token);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persisting device token: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "minted a new device token");
    return ESP_OK;
}

const char *slate_store_device_token(void)
{
    return s_device_token;
}

bool slate_store_device_token_matches(const char *candidate)
{
    if (candidate == NULL) {
        return false;
    }

    /*
     * Constant time in the token's length, not the candidate's. Comparing up to
     * the fixed length and folding a length check into the same accumulator
     * means a wrong token costs the same regardless of how many leading
     * characters it got right — the property a byte-at-a-time strcmp gives away
     * to anyone who can time a 401.
     */
    size_t candidate_len = strnlen(candidate, SLATE_DEVICE_TOKEN_LEN + 1);
    uint8_t diff = (uint8_t) (candidate_len ^ (size_t) SLATE_DEVICE_TOKEN_LEN);

    for (size_t i = 0; i < SLATE_DEVICE_TOKEN_LEN; i++) {
        char c = (i < candidate_len) ? candidate[i] : '\0';
        diff |= (uint8_t) (c ^ s_device_token[i]);
    }

    return diff == 0;
}

esp_err_t slate_store_device_token_reissue(void)
{
    char token[SLATE_DEVICE_TOKEN_LEN + 1];

    generate_token(token);

    esp_err_t err = slate_store_str_set(SLATE_KEY_DEVICE_TOKEN, token);
    if (err != ESP_OK) {
        return err;
    }

    /* Only after the write succeeds: a reissue that fails to persist must not
     * leave a running device answering to a token no reboot will restore. */
    memcpy(s_device_token, token, sizeof(s_device_token));
    ESP_LOGI(TAG, "device token reissued");
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Home Assistant credentials
 * ------------------------------------------------------------------------- */

esp_err_t slate_store_ha_set(const char *url, const char *token)
{
    if (url == NULL || token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(url) >= SLATE_HA_URL_MAX_LEN || strlen(token) >= SLATE_HA_TOKEN_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = slate_store_str_set(SLATE_KEY_HA_URL, url);
    if (err != ESP_OK) {
        return err;
    }
    return slate_store_str_set(SLATE_KEY_HA_TOKEN, token);
}

esp_err_t slate_store_ha_url_get(char *out, size_t out_len)
{
    return slate_store_str_get(SLATE_KEY_HA_URL, out, out_len);
}

bool slate_store_ha_token_is_set(void)
{
    nvs_handle_t nvs;
    if (nvs_open_ns(NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    /* Asking for the length rather than the value: this is the function
     * `GET /status` calls, and it never has the secret in a buffer to leak. */
    size_t len = 0;
    esp_err_t err = nvs_get_str(nvs, SLATE_KEY_HA_TOKEN, NULL, &len);
    nvs_close(nvs);

    return err == ESP_OK && len > 1; /* len counts the terminator */
}

esp_err_t slate_store_ha_token_get(char *out, size_t out_len)
{
    return slate_store_str_get(SLATE_KEY_HA_TOKEN, out, out_len);
}

esp_err_t slate_store_ha_clear(void)
{
    esp_err_t err = slate_store_erase(SLATE_KEY_HA_URL);
    esp_err_t token_err = slate_store_erase(SLATE_KEY_HA_TOKEN);

    /* Both attempted before either is reported, so a failure on the URL cannot
     * leave the token behind. */
    return err != ESP_OK ? err : token_err;
}

/* -------------------------------------------------------------------------
 * UI configuration
 * ------------------------------------------------------------------------- */

bool slate_store_config_exists(void)
{
    struct stat st;
    return stat(SLATE_CONFIG_PATH, &st) == 0 && st.st_size > 0;
}

esp_err_t slate_store_config_read(char **out, size_t *out_len)
{
    if (out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(SLATE_CONFIG_PATH, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size > SLATE_CONFIG_MAX_BYTES) {
        /* Larger than §3.1 allows, so no writer of ours produced it. */
        ESP_LOGE(TAG, "stored configuration is %ld B, over the 64 KB limit", (long) st.st_size);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(SLATE_CONFIG_PATH, "rb");
    if (f == NULL) {
        return ESP_FAIL;
    }

    /* PSRAM: see the header. 64 KB is most of what S-2 measured free in
     * internal DMA-capable memory once WiFi is up (§6.2). */
    char *buf = heap_caps_malloc((size_t) st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buf, 1, (size_t) st.st_size, f);
    fclose(f);

    if (read != (size_t) st.st_size) {
        free(buf);
        return ESP_FAIL;
    }

    buf[read] = '\0';
    *out = buf;
    *out_len = read;
    return ESP_OK;
}

esp_err_t slate_store_config_write(const char *json, size_t len)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > SLATE_CONFIG_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(SLATE_CONFIG_TMP_PATH, "wb");
    if (f == NULL) {
        return ESP_FAIL;
    }

    size_t written = fwrite(json, 1, len, f);
    /* fsync before the rename, or the rename can be durable while the bytes it
     * points at are not — which is the failure this whole dance exists for. */
    int flushed = fflush(f);
    int synced = (flushed == 0) ? fsync(fileno(f)) : -1;
    fclose(f);

    if (written != len || flushed != 0 || synced != 0) {
        unlink(SLATE_CONFIG_TMP_PATH);
        return ESP_FAIL;
    }

    if (rename(SLATE_CONFIG_TMP_PATH, SLATE_CONFIG_PATH) != 0) {
        unlink(SLATE_CONFIG_TMP_PATH);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t slate_store_config_erase(void)
{
    if (unlink(SLATE_CONFIG_PATH) != 0 && slate_store_config_exists()) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t slate_store_fs_usage(size_t *total_bytes, size_t *used_bytes)
{
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
         * brings a USB cable, which is the failure M1 exists to remove. The
         * cost is the device token and the credentials, and §4.3 already has a
         * way to tell the owner: a new pairing QR on screen.
         */
        ESP_LOGW(TAG, "NVS unusable (%s) — erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    return err;
}

static esp_err_t mount_fs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = SLATE_FS_BASE_PATH,
        .partition_label = SLATE_FS_PARTITION_LABEL,
        /* First boot has no filesystem, and a bad OTA is the other way this
         * partition ends up unreadable (§9.2 makes the same point about why the
         * setup page is not served from here). Formatting is the only outcome
         * that keeps the panel reachable. */
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mounting LittleFS: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t slate_store_init(void)
{
    s_in_early_boot = true;

    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        s_in_early_boot = false;
        return err;
    }

    derive_device_name();

    err = load_or_mint_device_token();
    if (err != ESP_OK) {
        s_in_early_boot = false;
        return err;
    }

    err = mount_fs();
    s_in_early_boot = false;
    return err;
}

esp_err_t slate_store_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset");

    /* Unmount before formatting, so nothing is holding an open file over a
     * partition that is about to stop existing. */
    esp_err_t err = esp_vfs_littlefs_unregister(SLATE_FS_PARTITION_LABEL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_littlefs_format(SLATE_FS_PARTITION_LABEL);
    if (err != ESP_OK) {
        return err;
    }

    err = mount_fs();
    if (err != ESP_OK) {
        return err;
    }

    /* nvs_flash_erase() needs the partition deinitialised first. */
    err = nvs_flash_deinit();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_INITIALIZED) {
        return err;
    }
    err = nvs_flash_erase();
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_flash_init();
    if (err != ESP_OK) {
        return err;
    }

    /* §4.3: a new token is issued after a factory reset. */
    return slate_store_device_token_reissue();
}
