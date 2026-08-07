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
 *
 * ERROR VOCABULARY
 *
 * One spelling per condition, whichever partition the value lives on — the
 * same principle §4.1 applies to the network error strings, for the same
 * reason. NVS's own error codes do not escape this header:
 *
 *   ESP_ERR_NOT_FOUND      the key or the file is not set. Usually not a
 *                          failure — a factory-fresh panel with no
 *                          configuration is §6.5's `error` mode, not a fault.
 *   ESP_ERR_INVALID_SIZE   the caller's buffer is too small, or the value
 *                          exceeds a documented limit. Use
 *                          slate_store_str_size() to size a retry.
 *   ESP_ERR_INVALID_ARG    a NULL or nonsensical argument.
 *
 *
 * THREADING
 *
 * slate_store_init() must be called once, from app_main, before any other
 * function here and before WiFi comes up (see slate_store_set_rf_active()).
 *
 * Afterwards every function here is safe to call from any task, with one
 * exception that cannot be fixed with a lock and so is stated rather than
 * hidden: slate_store_factory_reset() tears the filesystem down and wipes NVS
 * underneath the whole system. It serialises against this component, but it
 * invalidates NVS handles other ESP-IDF components cached long ago — the WiFi
 * driver's among them — so the device MUST be rebooted afterwards. See the
 * note on that function.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

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

/* A non-secret stand-in for the token in logs and diagnostics — see
 * slate_store_device_token_fingerprint(). */
#define SLATE_TOKEN_FINGERPRINT_LEN 8

/* `slate-a1b2c3` — §9.2's SSID, §4.3's mDNS name and §16's device name are all
 * this string, deliberately. */
#define SLATE_DEVICE_ID_LEN   6
#define SLATE_DEVICE_NAME_LEN (sizeof("slate-") - 1 + SLATE_DEVICE_ID_LEN)

/* A Home Assistant long-lived token is a JWT, typically ~180 characters. */
#define SLATE_HA_TOKEN_MAX_LEN 512
#define SLATE_HA_URL_MAX_LEN   128

/* 802.11: an SSID is at most 32 bytes and a WPA2 passphrase at most 63. Both
 * buffers include the terminator, so they are what `esp_wifi`'s own
 * wifi_sta_config_t fields hold. */
#define SLATE_WIFI_SSID_BUF_LEN     33
#define SLATE_WIFI_PASSWORD_BUF_LEN 64

/*
 * NVS keys live in one namespace and are listed here rather than spelled at
 * each call site, because a typo in a key name is a silent "not configured"
 * rather than an error. Keys the later M1 issues own are reserved now so two
 * of them cannot pick the same name for different things.
 *
 * NVS keys are limited to 15 characters; every name below is inside that.
 *
 * Two keys are deliberately absent: the Home Assistant token and the WiFi
 * passphrase. §12 makes both values that must not reach an API response, and a
 * key constant a serialiser can name is a key constant a serialiser can read —
 * so they are private to this component and reachable only through
 * slate_store_ha_token_get() and slate_store_wifi_password_get().
 * slate_store_str_get() refuses them by name.
 */
#define SLATE_NVS_NAMESPACE "slate"

#define SLATE_KEY_DEVICE_TOKEN "dev_token"  /* this issue */
#define SLATE_KEY_HA_URL       "ha_url"     /* M2, POST /ha */
#define SLATE_KEY_WIFI_SSID    "wifi_ssid"  /* #8/#55, POST /wifi */
#define SLATE_KEY_BOOT_COUNT   "boot_count" /* #10, GET /status */

/* --- Lifecycle ---------------------------------------------------------- */

/**
 * @brief Initialise NVS, mount LittleFS, mint the device token on first boot.
 *
 * Call once from app_main, before esp_wifi_init() and before any ADC use.
 * First-boot token generation needs an entropy source that the RF subsystem
 * has not started yet, and the documented way to get one — see slate_store.c —
 * conflicts with both.
 *
 * A corrupted NVS partition is erased and re-initialised rather than treated
 * as fatal, and a LittleFS partition that will not mount is reformatted: the
 * alternative is a panel that will not boot until someone brings a cable,
 * which is the failure M1 exists to remove (§9). Both are reported rather than
 * done silently — see slate_store_storage_was_reset().
 *
 * On failure the caller must NOT abort. §9 says there is no combination of
 * circumstances in which a powered panel is unreachable, and a store that
 * cannot come up is precisely when the setup access point matters most. The
 * device identity and the token accessors stay usable (the token will be
 * RAM-only and will not survive a reboot); the configuration accessors will
 * report ESP_ERR_NOT_FOUND.
 */
esp_err_t slate_store_init(void);

/**
 * @brief Tell the store whether the RF subsystem is running.
 *
 * The hardware RNG is a true RNG only while RF is up; before that it is a
 * PRNG, and a token drawn from a PRNG is indistinguishable from a good one.
 * When RF is down the store enables the SAR-ADC entropy source around the
 * draw instead — which is unsafe to do while RF or the ADC IS running, so the
 * store has to know rather than guess.
 *
 * #8 calls this with `true` after esp_wifi_start() and `false` before
 * esp_wifi_stop(). Until it is called the store assumes RF is down, which is
 * true at boot and is the safe direction to be wrong in.
 *
 * @warning Do not report RF as inactive while the ADC is in use — see
 *          bootloader_random.h. On this board the ADC is unused.
 */
void slate_store_set_rf_active(bool active);

/**
 * @brief Whether init had to erase NVS or reformat LittleFS to come up.
 *
 * A reformat is indistinguishable from a first boot in the log, and the
 * difference is "you have a new panel" versus "your dashboard is gone". #10's
 * `GET /status` should surface this, and §6.5's error mode should say so on
 * screen. Cleared by slate_store_factory_reset(), which is the one case where
 * losing everything was the point.
 */
bool slate_store_storage_was_reset(void);

/**
 * @brief Wipe NVS and LittleFS, then mint a new device token (§4.3).
 *
 * The primitive behind `POST /factory_reset` (#36).
 *
 * @warning The caller MUST reboot the device, and soon. Erasing the default
 *          NVS partition force-closes the handles other ESP-IDF components
 *          cached at their own init — the WiFi driver holds one for
 *          `nvs.net80211` — and NVS never re-issues a handle id, so from here
 *          on the driver's writes fail with ESP_ERR_NVS_INVALID_HANDLE and its
 *          calibration and configuration silently stop persisting. This
 *          function does not reboot only because an HTTP handler has a
 *          response to finish first.
 *
 * Best-effort: every step is attempted even if an earlier one failed, and the
 * first error is returned at the end. Secrets are erased before the
 * configuration, so an interrupted reset loses the sharable document rather
 * than leaving the Home Assistant token behind.
 */
esp_err_t slate_store_factory_reset(void);

/* --- Device identity ---------------------------------------------------- */

/**
 * @brief `a1b2c3` — the last three bytes of the base MAC, lowercase hex.
 *
 * §9.2 requires the setup SSID, the mDNS name of §4.3 and the device name of
 * §16 to carry the same suffix, "so one panel is called one thing everywhere".
 * That is one function, not three spellings in three components.
 *
 * Derived from the MAC and stored nowhere, so it is valid even when
 * slate_store_init() failed — which is exactly when §6.5's error screen and
 * §9.2's access point need a name to show.
 */
const char *slate_store_device_id(void);

/** @brief `slate-a1b2c3` — the device name reported by `GET /info`. */
const char *slate_store_device_name(void);

/* --- Device token (§4.3) ------------------------------------------------ */

/**
 * @brief Copy the 32-character device token into the caller's buffer.
 *
 * `out_len` must be at least SLATE_DEVICE_TOKEN_LEN + 1. The result is
 * NUL-terminated.
 *
 * This copies rather than returning a pointer into the store on purpose. A
 * reissue rewrites the token in place, and a caller holding a borrowed pointer
 * — §4.3's pairing QR renderer is the obvious one, and it holds it for as long
 * as the QR is on screen — would read a spliced old/new token and publish a
 * URL the device never accepts.
 */
esp_err_t slate_store_device_token_copy(char *out, size_t out_len);

/**
 * @brief A short non-secret fingerprint of the current token, hex, NUL-terminated.
 *
 * `out_len` must be at least SLATE_TOKEN_FINGERPRINT_LEN + 1. Truncated
 * SHA-256, so it identifies the token without carrying it.
 *
 * This is what belongs in a log line, an issue comment or a `GET /status`:
 * enough to answer "is this the same token as before the reboot" without
 * putting the credential somewhere it will be pasted. §4.3 delivers the real
 * token through a QR on the screen precisely so it never has to travel.
 */
esp_err_t slate_store_device_token_fingerprint(char *out, size_t out_len);

/**
 * @brief Compare a presented token against the device token in constant time.
 *
 * #10's bearer middleware calls this. It is here rather than there so the
 * comparison happens in one place and cannot degrade into a strcmp that leaks
 * the token a character at a time to anyone who can time a 401.
 */
bool slate_store_device_token_matches(const char *candidate);

/** @brief Mint and persist a new device token (§4.3: "on explicit request"). */
esp_err_t slate_store_device_token_reissue(void);

/* --- Home Assistant credentials (§12) ----------------------------------- */

/**
 * @brief Persist the Home Assistant URL and long-lived token together.
 *
 * `POST /ha` (M2) tests the connection before calling this — the store does
 * not validate what it is given beyond length. Both values are written under
 * one NVS commit, so a power cut cannot leave a URL with no token.
 */
esp_err_t slate_store_ha_set(const char *url, const char *token);

/** @brief Read the Home Assistant URL. ESP_ERR_NOT_FOUND if unconfigured. */
esp_err_t slate_store_ha_url_get(char *out, size_t out_len);

/**
 * @brief Whether a Home Assistant token is stored.
 *
 * §12 makes the HA token the one secret that genuinely matters, and §4.1 says
 * `GET /status` masks it. This predicate is what `/status` should read: a
 * serialiser that never receives the token cannot be made to print it. Cached,
 * so a polled endpoint does not touch flash.
 */
bool slate_store_ha_token_is_set(void);

/**
 * @brief Read the Home Assistant token.
 *
 * For the Home Assistant client and nothing else. Every other caller wants
 * slate_store_ha_token_is_set(). This is the only way to read the value —
 * slate_store_str_get() refuses the key — so the restriction is a mechanism
 * rather than a request.
 */
esp_err_t slate_store_ha_token_get(char *out, size_t out_len);

/** @brief Forget the Home Assistant URL and token. */
esp_err_t slate_store_ha_clear(void);

/* --- Station credentials (§12) ------------------------------------------ */

/**
 * @brief Persist the station SSID and passphrase together.
 *
 * `POST /wifi` (#55) writes them; #8's state machine reads them at boot and
 * after a change. One NVS commit, for the reason slate_store_ha_set() gives:
 * a power cut must not leave an SSID with a stale passphrase, which is a
 * configured-looking panel that reports `bad_password` forever.
 *
 * `password` may be NULL or empty for an open network — the distinction the
 * store keeps is "configured or not", and that is the SSID's job alone.
 *
 * ESP_ERR_INVALID_SIZE if either value exceeds what 802.11 allows; the caller
 * validates shape, not this.
 */
esp_err_t slate_store_wifi_set(const char *ssid, const char *password);

/**
 * @brief Read the configured station SSID. ESP_ERR_NOT_FOUND if unconfigured.
 *
 * `out_len` should be SLATE_WIFI_SSID_BUF_LEN. This is `sta_ssid` in
 * §4.1's `/info.network`, which exists even while the access point is up.
 */
esp_err_t slate_store_wifi_ssid_get(char *out, size_t out_len);

/**
 * @brief Whether station credentials are stored.
 *
 * §9.4's cold-boot branch turns on exactly this: no credentials raises the
 * setup access point immediately, credentials mean three association attempts
 * first. Cached, so the state machine does not read flash to make the
 * decision.
 */
bool slate_store_wifi_is_configured(void);

/**
 * @brief Read the station passphrase.
 *
 * For the WiFi station and nothing else — §12 keeps this out of the API and
 * out of the configuration document. slate_store_str_get() refuses the key, so
 * this is the only way to it. ESP_ERR_NOT_FOUND if the network is open.
 */
esp_err_t slate_store_wifi_password_get(char *out, size_t out_len);

/**
 * @brief Forget the station credentials — `DELETE /wifi` (§9.5).
 *
 * The passphrase is erased first, so an interrupted clear cannot leave the
 * secret behind an SSID that is already gone.
 */
esp_err_t slate_store_wifi_clear(void);

/* --- Settings ----------------------------------------------------------- */

/*
 * A small typed accessor pair over the `slate` namespace, so #8, #55, #10 and
 * #12 can persist what they need without opening their own NVS handle or
 * inventing a second namespace. Use the SLATE_KEY_* constants above.
 *
 * These refuse the two secret keys (ESP_ERR_INVALID_ARG); those have their own
 * accessors.
 */

esp_err_t slate_store_str_set(const char *key, const char *value);

/**
 * @brief Read a string setting.
 *
 * ESP_ERR_NOT_FOUND if unset. ESP_ERR_INVALID_SIZE if `out_len` is too small,
 * in which case `out` is left untouched and slate_store_str_size() will say
 * how much is needed.
 */
esp_err_t slate_store_str_get(const char *key, char *out, size_t out_len);

/**
 * @brief The buffer size a string setting needs, including its terminator.
 *
 * Exists because ESP-IDF hands this number back through the same argument it
 * uses for the buffer length, and a by-value wrapper would throw it away —
 * leaving a caller that got ESP_ERR_INVALID_SIZE unable to size a retry and
 * unable to tell "too big by one byte" from "too big by four hundred".
 */
esp_err_t slate_store_str_size(const char *key, size_t *out_size);

esp_err_t slate_store_u32_set(const char *key, uint32_t value);
esp_err_t slate_store_u32_get(const char *key, uint32_t *out);

/** @brief Remove a key. ESP_OK if it was not there — erasing is idempotent. */
esp_err_t slate_store_erase(const char *key);

/** @brief Boots since the last factory reset. §4.1 puts this in `GET /status`. */
uint32_t slate_store_boot_count(void);

/* --- UI configuration (LittleFS) ---------------------------------------- */

/** @brief Whether a stored configuration exists. */
bool slate_store_config_exists(void);

/**
 * @brief Read the stored configuration.
 *
 * On success `*out` is a NUL-terminated buffer the caller frees, and `*out_len`
 * is its length without the terminator. Allocated from PSRAM when it can be,
 * internal RAM otherwise: 64 KB out of the ~104 KB of internal DMA-capable
 * memory S-2 measured free (§6.2) is not a trade this should make on the
 * display's behalf, but failing a 2 KB read while internal RAM is free is
 * worse than making it.
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
 * ESP_ERR_INVALID_ARG if `len` is zero — an empty file is not a configuration,
 * and accepting one produces a state the readers disagree about.
 */
esp_err_t slate_store_config_write(const char *json, size_t len);

/** @brief Delete the stored configuration. */
esp_err_t slate_store_config_erase(void);

/** @brief LittleFS usage, for `GET /status` and for the flash budget of §6.3. */
esp_err_t slate_store_fs_usage(size_t *total_bytes, size_t *used_bytes);

#ifdef __cplusplus
}
#endif
