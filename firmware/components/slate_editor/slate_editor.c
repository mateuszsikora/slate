/*
 * Slate — the editor bundle. See include/slate_editor.h for why one document
 * exists in two places.
 *
 * DESIGN.md §10, §4.3, §6.3.
 */

#include "slate_editor.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"

#include "slate_api.h"
#include "slate_store.h"

static const char *TAG = "editor";

/*
 * The bundle, gzipped by the build and linked in. The symbols come from
 * `editor.html.gz`; CMakeLists.txt is where §10's 400 KB budget is checked.
 */
extern const uint8_t _binary_editor_html_gz_start[];
extern const uint8_t _binary_editor_html_gz_end[];

/* §10's files, under the `www/` directory slate_store.h already reserves. */
#define WWW_DIR     SLATE_FS_BASE_PATH "/www"
#define BUNDLE_PATH WWW_DIR "/index.html.gz"
#define BUNDLE_TMP  WWW_DIR "/index.html.gz.new"

/*
 * The stamp is the SHA-256 of the bundle the file was written from, not of the
 * file. They are the same thing until something else writes the file — and
 * then the difference is the point: a replacement stays until the *image's*
 * bundle changes, rather than being overwritten on the next boot.
 */
#define STAMP_PATH   WWW_DIR "/index.sha256"
#define STAMP_TMP    WWW_DIR "/index.sha256.new"
#define HASH_HEX_LEN 64

/*
 * Read and sent in blocks, so a bundle that grows towards §10's budget is not
 * a 400 KB allocation on a panel that is also holding a 2 MiB LVGL pool. Two
 * kilobytes is a comfortable multiple of the 512 B LittleFS cache and well
 * inside what one `send()` will take.
 */
#define CHUNK_BYTES 2048

static char s_hash[HASH_HEX_LEN + 1];
static bool s_from_file;

static size_t bundle_len(void)
{
    return (size_t) (_binary_editor_html_gz_end - _binary_editor_html_gz_start);
}

/**
 * The bundle's SHA-256, hex.
 *
 * Checked rather than assumed: an unhashed bundle is one whose stamp cannot be
 * compared, and a stamp of zeroes would either rewrite the file on every boot
 * or — worse — match another failure's zeroes and keep a stale editor. The
 * caller answers a failure by not seeding at all.
 */
static bool bundle_hash(char out[HASH_HEX_LEN + 1])
{
    uint8_t digest[32];
    if (mbedtls_sha256(_binary_editor_html_gz_start, bundle_len(), digest, 0) != 0) {
        return false;
    }
    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    }
    return true;
}

/**
 * Whether the stored bundle is the one this image carries.
 *
 * The size is checked as well as the stamp: a write interrupted by a power cut
 * leaves a short file behind a stamp that describes what it was supposed to
 * be, and a truncated gzip stream is a blank page rather than an error
 * anybody could act on.
 */
static bool stored_bundle_is_current(void)
{
    struct stat info;
    if (stat(BUNDLE_PATH, &info) != 0 || (size_t) info.st_size != bundle_len()) {
        return false;
    }

    FILE *stamp = fopen(STAMP_PATH, "rb");
    if (stamp == NULL) {
        return false;
    }
    char stored[HASH_HEX_LEN + 1] = {0};
    size_t read = fread(stored, 1, HASH_HEX_LEN, stamp);
    fclose(stamp);
    return read == HASH_HEX_LEN && memcmp(stored, s_hash, HASH_HEX_LEN) == 0;
}

/**
 * Write `data` to `path` through `tmp`.
 *
 * The rename is what makes the seed safe to interrupt: slate_store writes the
 * configuration the same way and for the same reason (§6.3's LittleFS gives
 * the atomic rename for free), and a half-written editor would otherwise be
 * indistinguishable from a whole one until a browser asked for it.
 */
static esp_err_t write_through_temporary(const char *path, const char *tmp,
                                         const void *data, size_t len)
{
    FILE *file = fopen(tmp, "wb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    bool ok = fwrite(data, 1, len, file) == len;
    ok = fflush(file) == 0 && ok;
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(tmp);
        return ESP_FAIL;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** Put this image's bundle on LittleFS unless it is already there. */
static void seed(void)
{
    /* mkdir on an existing directory is the ordinary case, not a failure: the
     * filesystem outlives the firmware that made it (§6.3). */
    if (mkdir(WWW_DIR, 0777) != 0) {
        struct stat info;
        if (stat(WWW_DIR, &info) != 0) {
            ESP_LOGW(TAG, "no %s on the filesystem; serving the editor from the image", WWW_DIR);
            return;
        }
    }

    if (stored_bundle_is_current()) {
        s_from_file = true;
        return;
    }

    const int64_t started = esp_timer_get_time();
    esp_err_t err = write_through_temporary(BUNDLE_PATH, BUNDLE_TMP,
                                            _binary_editor_html_gz_start, bundle_len());
    if (err == ESP_OK) {
        err = write_through_temporary(STAMP_PATH, STAMP_TMP, s_hash, HASH_HEX_LEN);
    }
    if (err != ESP_OK) {
        /*
         * A full or unwritable partition is not a reason to have no editor:
         * this boot answers from the image, which is the copy this firmware
         * was built against either way.
         *
         * Nothing is cleaned up, and nothing needs to be. Each write is a
         * rename over its target, so a failure leaves either the previous
         * bundle and its stamp untouched, or the new bundle behind the old
         * stamp. The next boot resolves both the same way — the stamp is not
         * this image's, so it writes again — and neither is served in the
         * meantime.
         */
        ESP_LOGW(TAG, "storing the editor bundle failed (%s); serving it from the image",
                 esp_err_to_name(err));
        return;
    }

    s_from_file = true;
    ESP_LOGI(TAG, "seeded %u B onto LittleFS in %lld ms", (unsigned) bundle_len(),
             (esp_timer_get_time() - started) / 1000);
}

/* --- GET / -------------------------------------------------------------- */

static esp_err_t send_from_file(httpd_req_t *req)
{
    FILE *file = fopen(BUNDLE_PATH, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char *buffer = malloc(CHUNK_BYTES);
    if (buffer == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    bool sending = false;
    for (;;) {
        size_t read = fread(buffer, 1, CHUNK_BYTES, file);
        if (read == 0) {
            if (ferror(file) != 0) {
                err = ESP_FAIL;
            }
            break;
        }
        /* Nothing has been sent before the first successful read, so a
         * filesystem that fails on the first block still falls back to the
         * image below. After that the response is committed. */
        sending = true;
        if (httpd_resp_send_chunk(req, buffer, read) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }

    free(buffer);
    fclose(file);

    if (err == ESP_OK) {
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    if (sending) {
        /* Mid-response. Dropping the connection is the only way left to say
         * "this document is not complete"; a client that retries gets a whole
         * one, from the image if the file is now unreadable. */
        ESP_LOGE(TAG, "reading %s failed mid-response", BUNDLE_PATH);
        return ESP_FAIL;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t page_handler(httpd_req_t *req)
{
    /*
     * Served gzipped unconditionally, exactly as §9.2's setup page is: every
     * browser has sent `Accept-Encoding: gzip` for twenty years, carrying both
     * copies would double what §10 budgets, and the one client that does not
     * is curl — which is a `| gunzip` away.
     */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    if (s_from_file) {
        esp_err_t err = send_from_file(req);
        if (err == ESP_OK || err == ESP_FAIL) {
            return err;
        }
        /* The file went away or memory ran out between boot and now. §10 is
         * about where the editor lives, not about failing to serve it. */
        ESP_LOGW(TAG, "%s unreadable (%s); answering from the image", BUNDLE_PATH,
                 esp_err_to_name(err));
    }

    return httpd_resp_send(req, (const char *) _binary_editor_html_gz_start, bundle_len());
}

/* --- Lifecycle ---------------------------------------------------------- */

esp_err_t slate_editor_init(void)
{
    if (bundle_hash(s_hash)) {
        seed();
    } else {
        ESP_LOGW(TAG, "the editor bundle could not be hashed; serving it from the image");
    }

    esp_err_t err = slate_api_register_root(SLATE_API_ROOT_EDITOR, page_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "editor is %u B gzipped, served from %s (build %.8s)",
             (unsigned) bundle_len(), s_from_file ? "LittleFS" : "the image", s_hash);
    return ESP_OK;
}
