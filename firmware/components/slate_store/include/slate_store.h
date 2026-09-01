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

/* Optional numeric administrator PIN used to unlock the web editor. */
#define SLATE_ADMIN_PIN_MIN_LEN 4
#define SLATE_ADMIN_PIN_MAX_LEN 12

/* Named credentials for scripts and Node-RED using the External API. */
#define SLATE_INTEGRATION_KEY_MAX          4
#define SLATE_INTEGRATION_KEY_TOKEN_LEN    32
#define SLATE_INTEGRATION_KEY_ID_LEN       8
#define SLATE_INTEGRATION_KEY_NAME_MAX_LEN 32

typedef struct {
    char id[SLATE_INTEGRATION_KEY_ID_LEN + 1];
    char name[SLATE_INTEGRATION_KEY_NAME_MAX_LEN + 1];
} slate_integration_key_info_t;

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

/*
 * §9.2's optional WPA2 passphrase for the setup access point. Not one of the two
 * secret keys above it, and the difference is deliberate: this value is *printed
 * on the setup screen* next to the SSID (§9.2, §12), so a component that can
 * read it is not a leak — the screen is showing it to the room already. It is
 * still never serialised into an API response, for the reason §12 gives about
 * the station passphrase: the panel's own network is not something a client has
 * a use for.
 *
 * Absent means the access point is open, which is §9.2's default.
 */
#define SLATE_KEY_SETUP_AP_PASS "setup_ap_pass" /* #55, the setup access point */

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
 * — the browser session response is the obvious one — could otherwise read a
 * spliced old/new token and publish a credential the device never accepts.
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

/* --- Web editor access -------------------------------------------------- */

/** @brief Whether an administrator PIN protects new web-editor sessions. */
bool slate_store_admin_pin_is_set(void);

/**
 * @brief Persist a numeric administrator PIN as a salted PBKDF2 hash.
 *
 * The PIN must contain 4–12 ASCII digits. The plaintext is never persisted.
 */
esp_err_t slate_store_admin_pin_set(const char *pin);

/** @brief Remove web-editor PIN protection. */
esp_err_t slate_store_admin_pin_clear(void);

/** @brief Verify a candidate PIN in constant time. */
bool slate_store_admin_pin_matches(const char *candidate);

/* --- External API credentials ------------------------------------------ */

/** @brief Number of named External API keys currently stored. */
size_t slate_store_integration_key_count(void);

/** @brief Copy non-secret metadata for the key at `index`. */
esp_err_t slate_store_integration_key_at(size_t index,
                                         slate_integration_key_info_t *out);

/**
 * @brief Create and persist one named External API key.
 *
 * The plaintext token is returned exactly once and is never persisted. The
 * store keeps only its SHA-256 digest. `token_out_len` must be at least
 * `SLATE_INTEGRATION_KEY_TOKEN_LEN + 1`.
 */
esp_err_t slate_store_integration_key_create(const char *name,
                                              slate_integration_key_info_t *info_out,
                                              char *token_out,
                                              size_t token_out_len);

/** @brief Revoke a key by its public id. */
esp_err_t slate_store_integration_key_revoke(const char *id);

/** @brief Match a presented External API credential in constant time. */
bool slate_store_integration_key_matches(const char *candidate);

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

/* --- Station addressing (§9.6, §4.1) ------------------------------------ */

/* lwIP resolves against three servers and ignores the rest, so three is the
 * number `POST /wifi` accepts and the number this stores. */
#define SLATE_IPV4_DNS_MAX 3

/**
 * @brief Where the station's address comes from, and what became of it.
 *
 * §9.6 makes a freshly submitted static configuration *pending* rather than
 * committed, because a wrong gateway associates perfectly and answers nothing.
 * That is one fact with four outcomes, and it is one field rather than a mode
 * plus a pair of booleans so that no combination of them can describe a state
 * the design does not have.
 *
 * The two failed members carry their reason rather than pointing at a separate
 * one, for §9.3's rule that the screen and the API share a vocabulary: they map
 * onto `gateway_unreachable` and `address_in_use` exactly, and a reason stored
 * beside the state is a second thing to keep in step with it.
 *
 * A failed configuration is kept, not erased. It is what the setup page
 * pre-fills the form with, and the whole recovery path of §9.6 is somebody
 * correcting one field of what they already typed.
 */
typedef enum {
    SLATE_IPV4_DHCP = 0,                   /**< nothing stored — the default */
    SLATE_IPV4_STATIC_PENDING,             /**< submitted, not yet proven */
    SLATE_IPV4_STATIC_CONFIRMED,           /**< proven; sticky from here on */
    SLATE_IPV4_STATIC_GATEWAY_UNREACHABLE, /**< proven wrong, reverted to DHCP */
    SLATE_IPV4_STATIC_ADDRESS_IN_USE,      /**< proven wrong, reverted to DHCP */
} slate_ipv4_state_t;

/** @brief `dhcp`, `pending`, `confirmed`, `gateway_unreachable`, `address_in_use`. */
const char *slate_ipv4_state_str(slate_ipv4_state_t state);

/** @brief Whether this state means the static address is the one to apply. */
bool slate_ipv4_state_is_applied(slate_ipv4_state_t state);

/**
 * @brief §4.1's `ipv4` object, parsed.
 *
 * Numbers rather than strings, in host byte order, because the one place that
 * has to be strict about the text is `POST /wifi` — it is the front door for a
 * value that can make the panel unreachable — and a second parser reading these
 * back out of NVS would be a second chance to disagree with the first.
 *
 * `prefix` is the CIDR length, 8 to 30. Meaningless when `state` is
 * SLATE_IPV4_DHCP, and so is everything below it.
 *
 * `generation` is an opaque store identity, not part of the JSON contract.
 * Callers that read a configuration and later submit a trial verdict must copy
 * it unchanged. It distinguishes two identical `POST /wifi` requests, so the
 * first one's verdict can never confirm the second one's record.
 */
typedef struct {
    slate_ipv4_state_t state;
    uint32_t address;
    uint32_t gateway;
    uint32_t dns[SLATE_IPV4_DNS_MAX];
    uint32_t generation;
    uint8_t prefix;
    uint8_t dns_count;
} slate_ipv4_config_t;

/**
 * @brief Read the stored addressing. `state` is SLATE_IPV4_DHCP when there is none.
 *
 * Never fails in a way the caller has to handle: an unreadable or unrecognised
 * record reports DHCP, which is the addressing every panel can fall back to and
 * the one §9.4 already knows how to rescue.
 */
void slate_store_ipv4_get(slate_ipv4_config_t *out);

/**
 * @brief Record what the trial of §9.6 decided about `tried`.
 *
 * The only thing that moves a stored configuration between the pending, the
 * confirmed and the two failed states. It rewrites the record rather than
 * taking a second key beside it, so a reader can never see a confirmed state
 * against an address that was replaced under it.
 *
 * `tried` is the configuration the verdict is about, and it is checked against
 * what is stored rather than trusted. A `POST /wifi` landing in the last
 * milliseconds of a trial replaces the record between the verdict and this
 * call, and stamping the old verdict on the new address is the worst outcome
 * this component has: a configuration marked confirmed without ever having been
 * proven is applied on every boot and never trialled again, which is exactly
 * the unreachable panel §9.6 exists to prevent.
 *
 * ESP_ERR_NOT_FOUND if there is no stored configuration to mark, and
 * ESP_ERR_INVALID_STATE if the one stored is no longer `tried`. Neither is a
 * failure the caller can do anything about: both mean the verdict was about a
 * configuration nobody is using any more.
 */
esp_err_t slate_store_ipv4_set_state(const slate_ipv4_config_t *tried,
                                     slate_ipv4_state_t state);

/* --- Station credentials (§12) ------------------------------------------ */

/**
 * @brief Persist the station SSID, passphrase and addressing together.
 *
 * `POST /wifi` (#55) writes them; #8's state machine reads them at boot and
 * after a change. One NVS commit, for the reason slate_store_ha_set() gives:
 * a power cut must not leave an SSID with a stale passphrase, which is a
 * configured-looking panel that reports `bad_password` forever. The addressing
 * is in the same commit for the sharper version of the same reason — an address
 * left behind with somebody else's gateway is §9.6's failure arriving without
 * anybody having typed it.
 *
 * `password == NULL` preserves the stored passphrase when `ssid` is unchanged.
 * This is the setup page's recovery path: a static-address failure returns all
 * non-secret fields to the form, while the passphrase remains in NVS. NULL for
 * a different SSID means an open network because there is no matching secret
 * to preserve. An explicit empty string always erases the passphrase, including
 * when an SSID changed from protected to open.
 *
 * `ipv4` may be NULL, and NULL means DHCP: the stored addressing is erased
 * rather than left for the next association to pick up. §4.1's "an absent
 * `ipv4` means `dhcp`" is a statement about the request, and a request that
 * says DHCP is a request to stop using the address that is stored.
 *
 * ESP_ERR_INVALID_SIZE if either value exceeds what 802.11 allows; the caller
 * validates shape, not this.
 */
esp_err_t slate_store_wifi_set(const char *ssid, const char *password,
                               const slate_ipv4_config_t *ipv4);

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
 * @brief Forget the station credentials and the addressing — `DELETE /wifi` (§9.5).
 *
 * The passphrase is erased first, so an interrupted clear cannot leave the
 * secret behind an SSID that is already gone. The addressing goes with them:
 * §9.5 is "changing a router must never require reflashing", and a panel that
 * kept a static address belonging to the network it was just told to forget
 * would join the next one and be unreachable on it.
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
