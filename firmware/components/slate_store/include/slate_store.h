/*
 * Slate — persistent store.
 *
 * design.md §4.3 (device token), §6.3 (partition layout), §12 (secrets).
 *
 * Two kinds of storage, chosen by what the data is rather than by how big it
 * is:
 *
 *   NVS       secrets and settings — the device token, the Home Assistant
 *             credentials, and from #8/#55 the station credentials. Small,
 *             written rarely, must survive an update.
 *   LittleFS  the UI configuration and, from M6, the editor's static files.
 *
 * Both partitions sit outside `ota_0` and `ota_1` (§6.3), which is what lets
 * §11.4 promise that a firmware update does not take the configuration and the
 * tokens with it.
 *
 * This is a component rather than part of `main` because everything in M1
 * consumes it: #8 and #55 keep the station credentials here, #10 authenticates
 * against the device token, #11 refuses an upload without it.
 *
 * Threading: slate_store_init() must be called once, from app_main, before any
 * other function here and before WiFi comes up (see the note on entropy below).
 * Everything afterwards is safe to call from any task.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
/* The NVS error codes are part of this contract — ESP_ERR_NVS_NOT_FOUND is how
 * "unset" is spelled — so callers get them without a second include. */
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Layout ------------------------------------------------------------- */

/* LittleFS mount point. §10 will serve the editor bundle out of `www/`. */
#define SLATE_FS_BASE_PATH "/slate"
#define SLATE_CONFIG_PATH  SLATE_FS_BASE_PATH "/config.json"

/* §3.1: a configuration is capped at 64 KB. Enforced on write, here, so the
 * limit is one number in one place rather than a check every writer repeats. */
#define SLATE_CONFIG_MAX_BYTES (64 * 1024)

/* §4.3: a 32-character random device token. */
#define SLATE_DEVICE_TOKEN_LEN 32

/* `slate-a1b2c3` — §9.2's SSID, §4.3's mDNS name and §16's device name are all
 * this string, deliberately. */
#define SLATE_DEVICE_ID_LEN   6
#define SLATE_DEVICE_NAME_LEN (sizeof("slate-") - 1 + SLATE_DEVICE_ID_LEN)

/* A Home Assistant long-lived token is a JWT, typically ~180 characters. */
#define SLATE_HA_TOKEN_MAX_LEN 512
#define SLATE_HA_URL_MAX_LEN   128

/*
 * NVS keys live in one namespace and are listed here rather than spelled at
 * each call site, because a typo in a key name is a silent "not configured"
 * rather than an error. Keys the later M1 issues own are reserved now so two
 * of them cannot pick the same name for different things.
 */
#define SLATE_NVS_NAMESPACE "slate"

#define SLATE_KEY_DEVICE_TOKEN "dev_token" /* this issue */
#define SLATE_KEY_HA_URL       "ha_url"    /* M2, POST /ha */
#define SLATE_KEY_HA_TOKEN     "ha_token"  /* M2, POST /ha */
#define SLATE_KEY_WIFI_SSID    "wifi_ssid" /* #8/#55, POST /wifi */
#define SLATE_KEY_WIFI_PASS    "wifi_pass" /* #8/#55, POST /wifi */
#define SLATE_KEY_AP_PASS      "ap_pass"   /* #55, optional WPA2 on the setup AP */
#define SLATE_KEY_BOOT_COUNT   "boot_count" /* #10, GET /status */

/* --- Lifecycle ---------------------------------------------------------- */

/**
 * @brief Initialise NVS and mount LittleFS; mint the device token on first boot.
 *
 * Call once from app_main, before esp_wifi_init() and before any ADC use.
 * First-boot token generation needs an entropy source that the RF subsystem
 * has not started yet, and the documented way to get one — see slate_store.c —
 * conflicts with both.
 *
 * A corrupted NVS partition is erased and re-initialised rather than treated as
 * fatal: the alternative is a panel that will not boot until someone brings a
 * cable, which is the failure M1 exists to remove. The device token is
 * regenerated in that case, and the pairing QR on screen (§4.3) is how the
 * owner finds out.
 */
esp_err_t slate_store_init(void);

/**
 * @brief Wipe NVS and LittleFS, then mint a new device token (§4.3).
 *
 * The primitive behind `POST /factory_reset` (#36). It does not reboot; the
 * caller decides when, because an HTTP handler has a response to finish first.
 */
esp_err_t slate_store_factory_reset(void);

/* --- Device identity ---------------------------------------------------- */

/**
 * @brief `a1b2c3` — the last three bytes of the base MAC, lowercase hex.
 *
 * §9.2 requires the setup SSID, the mDNS name of §4.3 and the device name of
 * §16 to carry the same suffix, "so one panel is called one thing everywhere".
 * That is one function, not three spellings in three components.
 */
const char *slate_store_device_id(void);

/** @brief `slate-a1b2c3` — the device name reported by `GET /info`. */
const char *slate_store_device_name(void);

/* --- Device token (§4.3) ------------------------------------------------ */

/**
 * @brief The 32-character device token, NUL-terminated.
 *
 * Valid for the lifetime of the process and stable across reboots. Reissued
 * only by slate_store_device_token_reissue() or a factory reset.
 */
const char *slate_store_device_token(void);

/**
 * @brief Compare a presented token against the device token in constant time.
 *
 * #10's bearer middleware calls this. It is here rather than there so the
 * comparison happens in one place and cannot degrade into a strcmp that leaks
 * the token a character at a time to anyone who can measure a response.
 */
bool slate_store_device_token_matches(const char *candidate);

/** @brief Mint and persist a new device token (§4.3: "on explicit request"). */
esp_err_t slate_store_device_token_reissue(void);

/* --- Home Assistant credentials (§12) ----------------------------------- */

/**
 * @brief Persist the Home Assistant URL and long-lived token together.
 *
 * `POST /ha` (M2) tests the connection before calling this — the store does not
 * validate what it is given beyond length.
 */
esp_err_t slate_store_ha_set(const char *url, const char *token);

/** @brief Read the Home Assistant URL. ESP_ERR_NVS_NOT_FOUND if unconfigured. */
esp_err_t slate_store_ha_url_get(char *out, size_t out_len);

/**
 * @brief Whether a Home Assistant token is stored.
 *
 * §12 makes the HA token the one secret that genuinely matters, and §4.1 says
 * `GET /status` masks it. This predicate is what `/status` should read: a
 * serialiser that never receives the token cannot be made to print it by a
 * later refactor.
 */
bool slate_store_ha_token_is_set(void);

/**
 * @brief Read the Home Assistant token.
 *
 * For the Home Assistant client and nothing else. Every other caller wants
 * slate_store_ha_token_is_set(). The token must not reach an API response, a
 * log line or the configuration JSON — §12 and §10 both say why the last one
 * matters: configurations are exported, imported and shared.
 */
esp_err_t slate_store_ha_token_get(char *out, size_t out_len);

/** @brief Forget the Home Assistant URL and token. */
esp_err_t slate_store_ha_clear(void);

/* --- Settings ----------------------------------------------------------- */

/*
 * A small typed accessor pair over the `slate` namespace, so #8, #55, #10 and
 * #12 can persist what they need without opening their own NVS handle or
 * inventing a second namespace. Use the SLATE_KEY_* constants above.
 */

esp_err_t slate_store_str_set(const char *key, const char *value);

/**
 * @brief Read a string setting.
 *
 * ESP_ERR_NVS_NOT_FOUND if unset, ESP_ERR_NVS_INVALID_LENGTH if `out_len` is
 * too small — in which case `out` is left untouched.
 */
esp_err_t slate_store_str_get(const char *key, char *out, size_t out_len);

esp_err_t slate_store_u32_set(const char *key, uint32_t value);
esp_err_t slate_store_u32_get(const char *key, uint32_t *out);

/** @brief Remove a key. ESP_OK if it was not there — erasing is idempotent. */
esp_err_t slate_store_erase(const char *key);

/* --- UI configuration (LittleFS) ---------------------------------------- */

/** @brief Whether a stored configuration exists. */
bool slate_store_config_exists(void);

/**
 * @brief Read the stored configuration.
 *
 * On success `*out` is a NUL-terminated buffer the caller frees, and `*out_len`
 * is its length without the terminator. Allocated from PSRAM: 64 KB out of the
 * ~104 KB of internal DMA-capable memory S-2 measured free (§6.2) is not a
 * trade this can make on behalf of the display.
 *
 * ESP_ERR_NOT_FOUND if nothing is stored — the `error` mode of §6.5, not a
 * failure.
 */
esp_err_t slate_store_config_read(char **out, size_t *out_len);

/**
 * @brief Replace the stored configuration atomically.
 *
 * Writes a temporary file and renames it over the target, so a power cut
 * mid-write leaves the previous configuration intact rather than a truncated
 * one. A half-written config is an `error` mode panel (§6.5) that needs a
 * person, and LittleFS gives the atomic rename for free.
 *
 * ESP_ERR_INVALID_SIZE if `len` exceeds §3.1's 64 KB.
 */
esp_err_t slate_store_config_write(const char *json, size_t len);

/** @brief Delete the stored configuration. */
esp_err_t slate_store_config_erase(void);

/** @brief LittleFS usage, for `GET /status` and for the flash budget of §6.3. */
esp_err_t slate_store_fs_usage(size_t *total_bytes, size_t *used_bytes);

#ifdef __cplusplus
}
#endif
