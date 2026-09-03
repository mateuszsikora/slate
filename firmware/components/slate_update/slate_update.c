/*
 * Slate — the release update channel. See include/slate_update.h for the
 * contract and for why the download is not esp_https_ota().
 *
 * design.md §11.4, §4.1, §6.3.
 */

#include "slate_update.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"

#include "slate_api.h"
#include "slate_config.h"
#include "slate_ota.h"
#include "slate_wifi.h"

static const char *TAG = "update";

/*
 * The channel. Compiled in rather than configured, because a manifest URL is
 * not a preference: it is where this firmware's own releases are published, and
 * a panel pointed at somebody else's manifest is a panel that installs somebody
 * else's firmware. §11.4 calls it "a stable URL" for the same reason.
 *
 *     idf.py -DSLATE_UPDATE_MANIFEST_URL='"https://example.invalid/m.json"' build
 *
 * builds a panel on a private channel; an empty string builds one with no
 * channel at all.
 */
#ifndef SLATE_UPDATE_MANIFEST_URL
#define SLATE_UPDATE_MANIFEST_URL "https://mateuszsikora.github.io/slate/ota/manifest.json"
#endif

#define HTTPS_PREFIX "https://"

/* A manifest is five short fields. Anything larger is not one, and refusing it
 * by length keeps a mistyped URL that answers with a web page from being parsed
 * as JSON on a device with 8 MB of PSRAM and no patience for it. */
#define MANIFEST_MAX_BYTES 2048

/* esp_app_desc_t.version is 32 bytes including its terminator, and the release
 * workflow refuses a tag that would not fit it. A manifest version that does
 * not fit here could never match the image it points at. */
#define VERSION_MAX 32
#define URL_MAX     256
#define SHA256_HEX  64
#define ERROR_MAX   24

/* PSRAM for the same reason slate_ota uses it: the buffer only ever reaches
 * esp_http_client_read() and esp_ota_write(), neither of which needs DMA or
 * internal memory, and §6.2's internal budget belongs to the radios. */
#define CHUNK_BYTES 4096

_Static_assert(CHUNK_BYTES >= SLATE_OTA_IMAGE_PREFIX_BYTES,
               "the first read must be able to hold the whole image prefix");

/* Per-operation socket timeout. A manifest is one small GET; an image is a
 * couple of megabytes over WiFi and gets the wall-clock budget below as well,
 * because a server that trickles one byte per timeout window would otherwise
 * hold the worker forever. */
#define HTTP_TIMEOUT_MS    15000
#define DOWNLOAD_BUDGET_US (600 * 1000000LL)
#define MAX_REDIRECTS      3

/*
 * §11.4's daily check. The first one is a minute after boot rather than
 * immediately: the station has to be up and the panel has more urgent things to
 * do with its first seconds. A failed check retries in an hour instead of a day
 * so that a panel which was merely offline at the wrong minute is not blind
 * until tomorrow.
 */
#define FIRST_CHECK_DELAY_US (60 * 1000000LL)
#define CHECK_PERIOD_US      (24 * 3600 * 1000000LL)
#define RETRY_PERIOD_US      (3600 * 1000000LL)

/* A panel that was not on a network yet is a different failure from a channel
 * that would not answer, and a much shorter one: §9.4 reconnects on its own, so
 * this is a boot that took longer than a minute, not an outage. */
#define OFFLINE_RETRY_US (5 * 60 * 1000000LL)

/* Long enough for one poll of GET /update to see `installed`, short enough that
 * nobody wonders whether the panel is going to reboot at all. */
#define REBOOT_DELAY_MS 3000

/* TLS handshakes are what size this: mbedtls keeps its buffers on the heap, but
 * the certificate parsing and the HTTP client's own frames are on the stack. */
#define TASK_STACK    8192
#define TASK_PRIORITY 4

#define JOB_CHECK   (1U << 0)
#define JOB_INSTALL (1U << 1)

typedef enum {
    UPDATE_IDLE = 0,
    UPDATE_CHECKING,
    UPDATE_DOWNLOADING,
    UPDATE_INSTALLED,
} update_state_t;

static const char *const STATE_NAMES[] = {"idle", "checking", "downloading", "installed"};

typedef struct {
    char version[VERSION_MAX];
    char url[URL_MAX];
    char sha256[SHA256_HEX + 1];
    int min_schema;
} release_t;

static struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool has_channel;
    update_state_t state;
    bool offered;
    release_t offer;
    char error[ERROR_MAX];
    int64_t checked_us; /* esp_timer time of the last completed check; 0 = never */
    size_t received;
    size_t total;
} s_update;

static void lock(void)
{
    xSemaphoreTake(s_update.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_update.lock);
}

/**
 * What the worker is doing, asserted by the worker itself.
 *
 * The route handlers set this too, so that the answer to the request that
 * started a job already reflects it. The worker repeats it because those two
 * can interleave: a POST arriving while the daily check is already running
 * would otherwise have its state overwritten by that check finishing.
 */
static void set_state(update_state_t state)
{
    lock();
    s_update.state = state;
    unlock();
}

/** Finish a job that did not get where it was going. */
static void fail(const char *code)
{
    ESP_LOGW(TAG, "update job failed: %s", code);
    lock();
    strlcpy(s_update.error, code, sizeof(s_update.error));
    s_update.state = UPDATE_IDLE;
    s_update.received = 0;
    s_update.total = 0;
    unlock();
}

/* --- Versions ----------------------------------------------------------- */

typedef struct {
    unsigned major;
    unsigned minor;
    unsigned patch;
    bool prerelease;
    bool valid;
} version_t;

/**
 * `MAJOR.MINOR.PATCH` with an optional `-suffix`, which is the shape the
 * release workflow enforces on a tag before it builds anything.
 */
static version_t parse_version(const char *text)
{
    version_t version = {0};
    if (text == NULL) {
        return version;
    }

    char *end = NULL;
    unsigned long parts[3];
    const char *cursor = text;
    for (int i = 0; i < 3; ++i) {
        if (*cursor < '0' || *cursor > '9') {
            return version;
        }
        parts[i] = strtoul(cursor, &end, 10);
        if (end == cursor) {
            return version;
        }
        cursor = end;
        if (i < 2) {
            if (*cursor != '.') {
                return version;
            }
            cursor++;
        }
    }

    /* `1.2.0` and `1.2.0-rc.1` are versions. `1.2.0.4` and `1.2.0rc1` are not,
     * and are treated as the development strings they resemble rather than
     * guessed at. */
    if (*cursor != '\0' && *cursor != '-') {
        return version;
    }

    version.major = (unsigned) parts[0];
    version.minor = (unsigned) parts[1];
    version.patch = (unsigned) parts[2];
    version.prerelease = *cursor == '-';
    version.valid = true;
    return version;
}

/**
 * Whether @p candidate should be offered over @p running.
 *
 * A running version that does not parse is a development build — `git
 * describe` output, or one of the selftest images — and every release is newer
 * than one of those. That is the answer a development panel wants: an offer
 * costs nothing, and §11.4 installs nothing without being asked.
 */
static bool is_newer(const version_t *candidate, const version_t *running)
{
    if (!candidate->valid) {
        return false;
    }
    if (!running->valid) {
        return true;
    }
    if (candidate->major != running->major) {
        return candidate->major > running->major;
    }
    if (candidate->minor != running->minor) {
        return candidate->minor > running->minor;
    }
    if (candidate->patch != running->patch) {
        return candidate->patch > running->patch;
    }
    /* Same numbers: 1.2.0 is newer than 1.2.0-rc.1, and nothing is newer than
     * the release itself. Two prereleases of one version are not ordered here,
     * because ordering them wrongly would offer a downgrade. */
    return running->prerelease && !candidate->prerelease;
}

/* --- Manifest ----------------------------------------------------------- */

static bool is_https(const char *url)
{
    return strncmp(url, HTTPS_PREFIX, sizeof(HTTPS_PREFIX) - 1) == 0;
}

static bool is_sha256_hex(const char *text)
{
    if (strlen(text) != SHA256_HEX) {
        return false;
    }
    for (const char *c = text; *c != '\0'; ++c) {
        bool hex = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

/** Copy a manifest string field, refusing one that could not fit its buffer. */
static bool copy_field(const cJSON *root, const char *name, char *out, size_t size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    return strlcpy(out, item->valuestring, size) < size;
}

/**
 * §11.4's five fields, and the four compatibility rules that decide whether
 * this panel may be offered what they describe.
 *
 * `board` and `min_schema` are the manifest's half of the contract: one release
 * channel may serve several boards and several configuration schemas, and a
 * panel that installed the wrong one would come back either unbootable or
 * unable to render the document it kept. `min_schema` is the oldest
 * configuration schema a panel must support to take this image, which is
 * exactly what §4.1's `schema_max` reports.
 */
static bool parse_manifest(const char *json, size_t len, release_t *out, const char **error)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        *error = "manifest_invalid";
        return false;
    }

    release_t release = {0};
    char board[48] = {0};
    const cJSON *min_schema = cJSON_GetObjectItemCaseSensitive(root, "min_schema");
    bool complete = copy_field(root, "version", release.version, sizeof(release.version)) &&
                    copy_field(root, "board", board, sizeof(board)) &&
                    copy_field(root, "url", release.url, sizeof(release.url)) &&
                    copy_field(root, "sha256", release.sha256, sizeof(release.sha256)) &&
                    cJSON_IsNumber(min_schema);
    if (complete) {
        release.min_schema = (int) min_schema->valuedouble;
    }
    cJSON_Delete(root);

    if (!complete || release.version[0] == '\0' || !is_sha256_hex(release.sha256) ||
        release.min_schema < 1) {
        *error = "manifest_invalid";
        return false;
    }
    if (strcmp(board, SLATE_API_MODEL_ID) != 0) {
        ESP_LOGW(TAG, "manifest is for board \"%s\", this panel is a %s", board,
                 SLATE_API_MODEL_ID);
        *error = "board_mismatch";
        return false;
    }
    if (release.min_schema > SLATE_CONFIG_SCHEMA_MAX) {
        ESP_LOGW(TAG, "release %s needs configuration schema %d; this panel supports %d",
                 release.version, release.min_schema, SLATE_CONFIG_SCHEMA_MAX);
        *error = "schema_too_new";
        return false;
    }
    /* The one rule that is about this transfer rather than about the release:
     * a checksum fetched over a channel anybody can rewrite is a checksum
     * anybody can rewrite, and so is the image URL beside it. */
    if (!is_https(release.url)) {
        *error = "insecure_url";
        return false;
    }

    if (!parse_version(release.version).valid) {
        *error = "manifest_invalid";
        return false;
    }

    *out = release;
    return true;
}

/* --- HTTP --------------------------------------------------------------- */

/**
 * Open a URL and follow its redirections, refusing to leave TLS on the way.
 *
 * A release asset is routinely a redirect to a storage host, so following them
 * is not optional. Re-checking the scheme after each hop is: a 302 to `http://`
 * would otherwise turn a verified channel into an unverified one without
 * anything in the log saying so.
 */
static esp_err_t open_with_redirects(esp_http_client_handle_t client, int64_t *length,
                                      const char **error)
{
    for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "connecting: %s", esp_err_to_name(err));
            *error = "unreachable";
            return err;
        }

        int64_t announced = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            *length = announced;
            return ESP_OK;
        }

        bool redirect = status == 301 || status == 302 || status == 303 || status == 307 ||
                        status == 308;
        if (!redirect) {
            ESP_LOGW(TAG, "server answered %d", status);
            *error = "unreachable";
            return ESP_FAIL;
        }

        err = esp_http_client_set_redirection(client);
        if (err != ESP_OK) {
            *error = "unreachable";
            return err;
        }

        char url[URL_MAX] = {0};
        if (esp_http_client_get_url(client, url, sizeof(url)) != ESP_OK || !is_https(url)) {
            ESP_LOGW(TAG, "redirected off TLS");
            *error = "insecure_url";
            return ESP_FAIL;
        }
        esp_http_client_close(client);
    }

    ESP_LOGW(TAG, "more than %d redirections", MAX_REDIRECTS);
    *error = "unreachable";
    return ESP_FAIL;
}

static esp_http_client_handle_t open_client(const char *url)
{
    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = CHUNK_BYTES,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* One request per connection. The panel talks to this host twice a day
         * at most, and a kept-alive TLS session is memory held for a day. */
        .keep_alive_enable = false,
    };
    return esp_http_client_init(&config);
}

/**
 * @param answered set when the channel replied at all, whatever it replied
 *                 with. The difference decides what happens to an existing
 *                 offer: a channel that answered is the authority on what it is
 *                 publishing, and a router that did not is not.
 */
static bool fetch_manifest(release_t *out, const char **error, bool *answered)
{
    *answered = false;
    esp_http_client_handle_t client = open_client(SLATE_UPDATE_MANIFEST_URL);
    if (client == NULL) {
        *error = "out_of_memory";
        return false;
    }

    char *body = malloc(MANIFEST_MAX_BYTES);
    if (body == NULL) {
        esp_http_client_cleanup(client);
        *error = "out_of_memory";
        return false;
    }

    bool parsed = false;
    int64_t announced = 0;
    if (open_with_redirects(client, &announced, error) == ESP_OK) {
        if (announced > MANIFEST_MAX_BYTES) {
            ESP_LOGW(TAG, "manifest announces %" PRId64 " B", announced);
            *error = "manifest_invalid";
        } else {
            /* A chunked manifest announces -1, which is not an error here: the
             * length that matters is what was read, bounded by the buffer. */
            size_t received = 0;
            int chunk = 0;
            while (received < MANIFEST_MAX_BYTES &&
                   (chunk = esp_http_client_read(client, body + received,
                                                 MANIFEST_MAX_BYTES - received)) > 0) {
                received += chunk;
            }
            if (chunk < 0 || received == 0) {
                *error = "unreachable";
            } else {
                *answered = true;
                parsed = parse_manifest(body, received, out, error);
            }
        }
    }

    free(body);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return parsed;
}

/* --- Check -------------------------------------------------------------- */

/** @return how long to wait before looking again. */
static int64_t check(void)
{
    set_state(UPDATE_CHECKING);

    slate_wifi_status_t network;
    slate_wifi_status(&network);
    if (!network.connected) {
        fail("offline");
        return OFFLINE_RETRY_US;
    }

    release_t candidate = {0};
    const char *error = NULL;
    bool answered = false;
    if (!fetch_manifest(&candidate, &error, &answered)) {
        /*
         * A previous offer survives a channel that could not be reached: the
         * release did not stop existing because a router did. It does not
         * survive one that answered and no longer publishes something this
         * panel may install — a stale offer for another board, or for a
         * manifest that has since been half-published, is an Install button
         * nothing on the channel stands behind any more.
         */
        if (answered) {
            lock();
            s_update.offered = false;
            unlock();
        }
        fail(error);
        return RETRY_PERIOD_US;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    version_t running_version = parse_version(running->version);
    version_t candidate_version = parse_version(candidate.version);
    bool newer = is_newer(&candidate_version, &running_version);

    lock();
    s_update.offered = newer;
    if (newer) {
        s_update.offer = candidate;
    }
    s_update.checked_us = esp_timer_get_time();
    s_update.error[0] = '\0';
    s_update.state = UPDATE_IDLE;
    unlock();

    if (newer) {
        ESP_LOGI(TAG, "release %s is available over the running %s", candidate.version,
                 running->version);
    } else {
        ESP_LOGI(TAG, "%s is current; the channel offers %s", running->version,
                 candidate.version);
    }
    return CHECK_PERIOD_US;
}

/* --- Install ------------------------------------------------------------ */

static void reboot_timer(void *arg)
{
    (void) arg;
    ESP_LOGW(TAG, "rebooting into the installed release");
    esp_restart();
}

static void schedule_reboot(void)
{
    const esp_timer_create_args_t args = {
        .callback = reboot_timer,
        .name = "update_reboot",
    };

    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&args, &timer) == ESP_OK &&
        esp_timer_start_once(timer, REBOOT_DELAY_MS * 1000) == ESP_OK) {
        return;
    }

    /* The boot partition has already moved. A device left running the previous
     * image while flash says otherwise is the one outcome nobody can reason
     * about, so the delay is what gets dropped, not the reboot. */
    ESP_LOGE(TAG, "no reboot timer — restarting now");
    esp_restart();
}

/** One place where an esp_ota_* failure becomes §4's stable vocabulary. */
static const char *ota_error_code(const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "%s: %s", what, esp_err_to_name(err));
    switch (err) {
    case ESP_ERR_OTA_VALIDATE_FAILED:
        return "invalid_image";
    case ESP_ERR_OTA_ROLLBACK_INVALID_STATE:
        return "pending_verify";
    case ESP_ERR_NO_MEM:
        return "out_of_memory";
    default:
        return "ota_failed";
    }
}

static void hex_encode(const uint8_t *digest, size_t len, char *out)
{
    static const char DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = DIGITS[digest[i] >> 4];
        out[i * 2 + 1] = DIGITS[digest[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static void progress(size_t received)
{
    lock();
    s_update.received = received;
    unlock();
}

/*
 * The download, and the order of its refusals.
 *
 * Everything that can be refused before esp_ota_begin() is refused before it,
 * because that call erases the slot holding the image this panel could
 * otherwise fall back to — the same line slate_ota.h draws for §11.1. What
 * survives past it is the checksum, which cannot be known before the last byte
 * and is therefore the one failure that costs the other slot. It still costs
 * nothing that is running: esp_ota_set_boot_partition() is below it.
 */
static void install(void)
{
    release_t target;
    lock();
    target = s_update.offer;
    s_update.state = UPDATE_DOWNLOADING;
    unlock();

    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (slot == NULL) {
        fail("no_ota_partition");
        return;
    }

    char *buffer = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        buffer = malloc(CHUNK_BYTES);
    }
    esp_http_client_handle_t client = buffer == NULL ? NULL : open_client(target.url);
    if (client == NULL) {
        free(buffer);
        fail("out_of_memory");
        return;
    }

    ESP_LOGI(TAG, "installing %s into %s", target.version, slot->label);

    const char *error = NULL;
    int64_t announced = 0;
    if (open_with_redirects(client, &announced, &error) != ESP_OK) {
        goto refuse;
    }
    /* esp_ota_begin() is given a length so it erases what the image needs
     * rather than the whole 6 MB slot, and a server that will not say how long
     * its image is cannot be checksummed against a manifest anyway. */
    if (announced <= 0) {
        ESP_LOGW(TAG, "image served without a Content-Length");
        error = "download_failed";
        goto refuse;
    }
    if ((size_t) announced > slot->size) {
        ESP_LOGW(TAG, "image of %" PRId64 " B does not fit %s (%" PRIu32 " B)", announced,
                 slot->label, slot->size);
        error = "too_large";
        goto refuse;
    }

    size_t total = (size_t) announced;
    lock();
    s_update.total = total;
    unlock();

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    const int64_t budget_ends = esp_timer_get_time() + DOWNLOAD_BUDGET_US;
    size_t received = 0;
    while (received < SLATE_OTA_IMAGE_PREFIX_BYTES && received < total) {
        int chunk = esp_http_client_read(client, buffer + received,
                                         (int) (SLATE_OTA_IMAGE_PREFIX_BYTES - received));
        if (chunk <= 0) {
            error = "download_failed";
            goto hashed;
        }
        received += chunk;
    }

    const esp_app_desc_t *incoming = slate_ota_image_prefix(buffer, received);
    if (incoming == NULL) {
        ESP_LOGW(TAG, "the served body is not an %s application image", CONFIG_IDF_TARGET);
        error = "not_an_image";
        goto hashed;
    }
    /* The manifest says which version its checksum belongs to, and the image
     * carries its own. A channel whose two halves disagree is a channel that
     * has been edited by hand or half-published, and installing either answer
     * would leave a panel reporting a version nobody released. */
    if (strncmp(incoming->version, target.version, sizeof(incoming->version)) != 0) {
        ESP_LOGW(TAG, "manifest promises %s, the image says %.*s", target.version,
                 (int) sizeof(incoming->version), incoming->version);
        error = "version_mismatch";
        goto hashed;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(slot, total, &handle);
    if (err != ESP_OK) {
        error = ota_error_code("esp_ota_begin", err);
        goto hashed;
    }

    mbedtls_sha256_update(&sha, (const unsigned char *) buffer, received);
    err = esp_ota_write(handle, buffer, received);
    progress(received);

    while (err == ESP_OK && received < total) {
        if (esp_timer_get_time() > budget_ends) {
            ESP_LOGW(TAG, "download budget expired after %u of %u B", (unsigned) received,
                     (unsigned) total);
            error = "download_failed";
            break;
        }
        size_t remaining = total - received;
        int chunk = esp_http_client_read(client, buffer,
                                         (int) (remaining < CHUNK_BYTES ? remaining : CHUNK_BYTES));
        if (chunk <= 0) {
            ESP_LOGW(TAG, "download stopped after %u of %u B", (unsigned) received,
                     (unsigned) total);
            error = "download_failed";
            break;
        }
        mbedtls_sha256_update(&sha, (const unsigned char *) buffer, chunk);
        err = esp_ota_write(handle, buffer, chunk);
        received += chunk;
        progress(received);
    }

    if (error == NULL && err != ESP_OK) {
        error = ota_error_code("esp_ota_write", err);
    }

    if (error == NULL) {
        uint8_t digest[32];
        char hex[SHA256_HEX + 1];
        mbedtls_sha256_finish(&sha, digest);
        hex_encode(digest, sizeof(digest), hex);
        if (strcasecmp(hex, target.sha256) != 0) {
            ESP_LOGE(TAG, "checksum mismatch: the manifest names %.16s…, the download is %.16s…",
                     target.sha256, hex);
            error = "checksum_mismatch";
        }
    }

    if (error != NULL) {
        esp_ota_abort(handle);
        ESP_LOGW(TAG, "%s is now blank", slot->label);
        goto hashed;
    }

    /* esp_ota_end() consumes the handle whether or not it succeeds, so there is
     * nothing left to abort below this line. */
    err = esp_ota_end(handle);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(slot);
        if (err != ESP_OK) {
            error = ota_error_code("esp_ota_set_boot_partition", err);
        }
    } else {
        error = ota_error_code("esp_ota_end", err);
    }

hashed:
    mbedtls_sha256_free(&sha);
refuse:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buffer);

    if (error != NULL) {
        fail(error);
        return;
    }

    lock();
    s_update.state = UPDATE_INSTALLED;
    s_update.offered = false;
    s_update.error[0] = '\0';
    unlock();

    /* The new image boots in PENDING_VERIFY and has to answer GET /info to keep
     * the slot, exactly as an uploaded one does (§11.2). A release that does
     * not boot on this panel therefore costs a reboot and nothing else. */
    ESP_LOGI(TAG, "%s installed into %s, rebooting in %d ms", target.version, slot->label,
             REBOOT_DELAY_MS);
    schedule_reboot();
}

/* --- Worker ------------------------------------------------------------- */

static void update_task(void *arg)
{
    (void) arg;
    int64_t next_check_us = esp_timer_get_time() + FIRST_CHECK_DELAY_US;

    for (;;) {
        int64_t now = esp_timer_get_time();
        TickType_t wait = next_check_us > now ? pdMS_TO_TICKS((next_check_us - now) / 1000)
                                              : (TickType_t) 0;

        uint32_t jobs = 0;
        xTaskNotifyWait(0, UINT32_MAX, &jobs, wait);

        if (jobs & JOB_INSTALL) {
            install();
            /* install() either reboots or leaves a panel that should keep
             * checking; either way the schedule is unchanged. */
            continue;
        }

        /* An explicit check and the daily one are the same work, and both
         * restart the clock: somebody who just pressed the button does not want
         * a second check thirty seconds later. How long that clock is depends
         * on what the check ran into, which is what check() returns. */
        next_check_us = esp_timer_get_time() + check();
    }
}

/* --- Routes ------------------------------------------------------------- */

static esp_err_t status_handler(httpd_req_t *req)
{
    const esp_app_desc_t *running = esp_app_get_description();

    lock();
    update_state_t state = s_update.state;
    bool offered = s_update.offered;
    release_t offer = s_update.offer;
    char error[ERROR_MAX];
    strlcpy(error, s_update.error, sizeof(error));
    int64_t checked_us = s_update.checked_us;
    size_t received = s_update.received;
    size_t total = s_update.total;
    unlock();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return slate_api_send_json(req, NULL);
    }

    bool ok = cJSON_AddStringToObject(root, "current", running->version) != NULL &&
              cJSON_AddStringToObject(root, "state", STATE_NAMES[state]) != NULL;
    ok = ok && (s_update.has_channel
                    ? cJSON_AddStringToObject(root, "manifest_url", SLATE_UPDATE_MANIFEST_URL) !=
                          NULL
                    : cJSON_AddNullToObject(root, "manifest_url") != NULL);
    ok = ok && (checked_us == 0
                    ? cJSON_AddNullToObject(root, "checked_s_ago") != NULL
                    : cJSON_AddNumberToObject(root, "checked_s_ago",
                                              (double) ((esp_timer_get_time() - checked_us) /
                                                        1000000LL)) != NULL);
    ok = ok && (error[0] == '\0' ? cJSON_AddNullToObject(root, "error") != NULL
                                 : cJSON_AddStringToObject(root, "error", error) != NULL);

    if (ok && offered) {
        cJSON *available = cJSON_AddObjectToObject(root, "available");
        ok = available != NULL &&
             cJSON_AddStringToObject(available, "version", offer.version) != NULL &&
             cJSON_AddStringToObject(available, "url", offer.url) != NULL &&
             cJSON_AddStringToObject(available, "sha256", offer.sha256) != NULL;
    } else if (ok) {
        ok = cJSON_AddNullToObject(root, "available") != NULL;
    }

    /* Progress exists only while there is something to be part-way through.
     * A client that reads `null` here is not looking at a stalled download. */
    if (ok && state == UPDATE_DOWNLOADING) {
        cJSON *download = cJSON_AddObjectToObject(root, "progress");
        ok = download != NULL &&
             cJSON_AddNumberToObject(download, "received", (double) received) != NULL &&
             cJSON_AddNumberToObject(download, "total", (double) total) != NULL;
    } else if (ok) {
        ok = cJSON_AddNullToObject(root, "progress") != NULL;
    }

    if (!ok) {
        cJSON_Delete(root);
        root = NULL;
    }
    return slate_api_send_json(req, root);
}

static esp_err_t accepted(httpd_req_t *req)
{
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

/**
 * Both POSTs are requests to start work, not reports that it finished. §11.4's
 * daily check and its install are seconds of TLS and minutes of flash writing,
 * and this device answers HTTP from one task (`slate_ota.h`).
 */
static esp_err_t start_job(httpd_req_t *req, uint32_t job)
{
    if (!s_update.has_channel) {
        return slate_api_refuse(req, "503 Service Unavailable", "no_channel");
    }

    lock();
    bool busy = s_update.state != UPDATE_IDLE;
    if (!busy) {
        s_update.state = job == JOB_INSTALL ? UPDATE_DOWNLOADING : UPDATE_CHECKING;
        s_update.error[0] = '\0';
        s_update.received = 0;
        s_update.total = 0;
    }
    unlock();

    if (busy) {
        return slate_api_refuse(req, "409 Conflict", "busy");
    }
    xTaskNotify(s_update.task, job, eSetBits);
    return accepted(req);
}

static esp_err_t check_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        return slate_api_refuse(req, "400 Bad Request", "unexpected_body");
    }
    return start_job(req, JOB_CHECK);
}

/**
 * §11.4's "installation only on explicit request", and the body is what makes
 * the request explicit: a client that names the version it is installing cannot
 * install a different one that arrived between the offer it displayed and the
 * button somebody pressed.
 */
static esp_err_t install_handler(httpd_req_t *req)
{
    char requested[VERSION_MAX] = {0};

    if (req->content_len > 0) {
        char body[128];
        if (req->content_len >= sizeof(body)) {
            return slate_api_refuse(req, "413 Payload Too Large", "too_large");
        }
        size_t received = 0;
        while (received < req->content_len) {
            int chunk = httpd_req_recv(req, body + received, req->content_len - received);
            if (chunk <= 0) {
                return slate_api_refuse(req, "400 Bad Request", "truncated");
            }
            received += chunk;
        }

        cJSON *root = cJSON_ParseWithLength(body, received);
        if (root == NULL || !cJSON_IsObject(root)) {
            cJSON_Delete(root);
            return slate_api_refuse(req, "400 Bad Request", "invalid_json");
        }
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        bool valid = version == NULL || cJSON_IsNull(version) ||
                     (cJSON_IsString(version) && version->valuestring != NULL &&
                      strlcpy(requested, version->valuestring, sizeof(requested)) <
                          sizeof(requested));
        cJSON_Delete(root);
        if (!valid) {
            return slate_api_refuse(req, "400 Bad Request", "invalid_version");
        }
    }

    if (!s_update.has_channel) {
        return slate_api_refuse(req, "503 Service Unavailable", "no_channel");
    }

    lock();
    bool busy = s_update.state != UPDATE_IDLE;
    bool offered = s_update.offered;
    bool matches = offered && (requested[0] == '\0' || strcmp(requested, s_update.offer.version) == 0);
    unlock();

    if (busy) {
        return slate_api_refuse(req, "409 Conflict", "busy");
    }
    if (!offered) {
        return slate_api_refuse(req, "409 Conflict", "no_update");
    }
    if (!matches) {
        return slate_api_refuse(req, "409 Conflict", "version_mismatch");
    }
    return start_job(req, JOB_INSTALL);
}

esp_err_t slate_update_init(void)
{
    if (s_update.lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_update.lock = xSemaphoreCreateMutex();
    if (s_update.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    static const httpd_uri_t status = {
        .uri = SLATE_API_BASE_PATH "/update",
        .method = HTTP_GET,
        .handler = status_handler,
    };
    static const httpd_uri_t check_now = {
        .uri = SLATE_API_BASE_PATH "/update/check",
        .method = HTTP_POST,
        .handler = check_handler,
    };
    static const httpd_uri_t install_now = {
        .uri = SLATE_API_BASE_PATH "/update/install",
        .method = HTTP_POST,
        .handler = install_handler,
    };

    /* §4.3: the two POSTs write flash and reboot the panel, and the GET says
     * which firmware is running and where it came from. All three are
     * administrator surface, so all three carry the token. */
    esp_err_t err = slate_api_register_uri(&status, SLATE_API_AUTH_DEVICE_TOKEN);
    if (err == ESP_OK) {
        err = slate_api_register_uri(&check_now, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (err == ESP_OK) {
        err = slate_api_register_uri(&install_now, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (err != ESP_OK) {
        vSemaphoreDelete(s_update.lock);
        s_update.lock = NULL;
        return err;
    }

    /* An empty URL is a panel with no channel, which is a supported build and
     * not a failure: the routes answer, `manifest_url` is null, and no task
     * exists to check anything. A non-TLS URL is a build mistake, and it
     * becomes the same state rather than a channel whose checksum means
     * nothing. */
    if (SLATE_UPDATE_MANIFEST_URL[0] == '\0') {
        ESP_LOGI(TAG, "no update channel in this build; %s/update answers anyway",
                 SLATE_API_BASE_PATH);
        return ESP_OK;
    }
    if (!is_https(SLATE_UPDATE_MANIFEST_URL)) {
        ESP_LOGE(TAG, "the manifest URL is not https; this panel has no update channel");
        return ESP_OK;
    }

    if (xTaskCreate(update_task, "slate_update", TASK_STACK, NULL, TASK_PRIORITY,
                    &s_update.task) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the update task; no checks will be made");
        return ESP_ERR_NO_MEM;
    }

    s_update.has_channel = true;
    ESP_LOGI(TAG, "release channel ready: %s", SLATE_UPDATE_MANIFEST_URL);
    return ESP_OK;
}
