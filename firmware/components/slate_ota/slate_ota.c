/*
 * Slate — development OTA. See include/slate_ota.h for what this is and is not.
 *
 * design.md §11.1, §4.1, §4.3.
 */

#include "slate_ota.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "slate_api.h"

static const char *TAG = "ota";

/* On the heap rather than the HTTP task's 4 KB stack, and large enough that a
 * 1.2 MB image is not a hundred thousand flash calls. */
#define CHUNK_BYTES 4096

/*
 * What has to be in hand before esp_ota_begin() is allowed to erase anything:
 * the image header, the first segment header, and the application description
 * that ESP-IDF places immediately after them.
 */
#define IMAGE_PREFIX_BYTES \
    (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

_Static_assert(CHUNK_BYTES >= IMAGE_PREFIX_BYTES,
               "the first read must be able to hold the whole image prefix");

/* Long enough for the response to leave the socket, short enough that the
 * uploader does not wonder whether anything happened. */
#define REBOOT_DELAY_MS 500

/*
 * httpd's receive timeout is 5 s and a slow uploader legitimately hits it. A
 * client that has gone away silently hits it forever, and it is holding the
 * only HTTP task while it does, so the stall is bounded: a minute of nothing.
 */
#define RECV_STALL_LIMIT 12

/* --- Reboot ------------------------------------------------------------- */

static void reboot_timer(void *arg)
{
    (void) arg;
    ESP_LOGW(TAG, "rebooting into the uploaded image");
    esp_restart();
}

/*
 * The reboot is deferred rather than immediate because the success response is
 * the only report the uploader gets — §11.1's flash has no serial console
 * attached by definition, and a device that reboots inside its own reply looks
 * from the desk exactly like one that crashed on the image it was given.
 */
static void reboot_after_response(void)
{
    const esp_timer_create_args_t args = {
        .callback = reboot_timer,
        .name = "ota_reboot",
    };

    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&args, &timer) == ESP_OK &&
        esp_timer_start_once(timer, REBOOT_DELAY_MS * 1000) == ESP_OK) {
        return;
    }

    ESP_LOGE(TAG, "no reboot timer — restarting now, the response may be lost");
    esp_restart();
}

/* --- Upload ------------------------------------------------------------- */

/*
 * One place where an esp_ota_* failure becomes an HTTP answer, so the two
 * cases a caller can do something about are named and the rest are honestly
 * one word. Anything that reaches `ota_failed` is a device-side fault, not a
 * bad request.
 */
static esp_err_t send_ota_error(httpd_req_t *req, const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "%s: %s", what, esp_err_to_name(err));

    switch (err) {
    case ESP_ERR_OTA_VALIDATE_FAILED:
        return slate_api_send_error(req, "400 Bad Request", "invalid_image");

    case ESP_ERR_OTA_ROLLBACK_INVALID_STATE:
        /* Reachable once #12 enables rollback: ESP-IDF refuses to write a new
         * image while the running one is still pending verification. Named so
         * the answer says what to wait for instead of looking like a fault. */
        return slate_api_send_error(req, "409 Conflict", "pending_verify");

    case ESP_ERR_NO_MEM:
        return slate_api_send_error(req, "503 Service Unavailable", "out_of_memory");

    default:
        return slate_api_send_error(req, "500 Internal Server Error", "ota_failed");
    }
}

/** Read what is available, retrying the timeout a merely slow client produces. */
static int receive_some(httpd_req_t *req, char *buffer, size_t len)
{
    for (int stalls = 0; stalls < RECV_STALL_LIMIT; stalls++) {
        int received = httpd_req_recv(req, buffer, len);
        if (received > 0) {
            return received;
        }
        if (received != HTTPD_SOCK_ERR_TIMEOUT) {
            return -1;
        }
    }
    return -1;
}

/*
 * The image is inspected before a sector is erased. esp_ota_end() would reject
 * a body that is not firmware anyway, but only after the target partition has
 * been erased and rewritten — so the panel that was told "wrong file" would
 * have lost the image it could have rolled back to. Checking first also makes
 * the log line say which version is arriving over which, which is the question
 * being asked when someone flashes twice in a minute and wonders which one is
 * running.
 */
static bool image_prefix_is_plausible(const char *prefix, const esp_app_desc_t **incoming)
{
    const esp_image_header_t *header = (const esp_image_header_t *) prefix;
    const esp_app_desc_t *description =
        (const esp_app_desc_t *) (prefix + sizeof(esp_image_header_t) +
                                  sizeof(esp_image_segment_header_t));

    if (header->magic != ESP_IMAGE_HEADER_MAGIC) {
        return false;
    }
    /* An ESP32 image on an ESP32-S3 is a boot loop, and it is a plain mistake
     * to make with two boards on one desk. */
    if (header->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return false;
    }
    if (description->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return false;
    }

    *incoming = description;
    return true;
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "no second application partition to write to");
        return slate_api_send_error(req, "500 Internal Server Error", "no_ota_partition");
    }

    /*
     * §6.3 gives the image a known size, and esp_ota_begin() is given it: with
     * OTA_SIZE_UNKNOWN it erases the whole 6 MB slot before the first byte
     * lands, which is several seconds of nothing on every development flash.
     * A body without a Content-Length — and a chunked one, which
     * esp_http_server does not decode — arrives here as zero and is refused.
     */
    size_t total = req->content_len;
    if (total == 0) {
        ESP_LOGW(TAG, "upload with no body");
        return slate_api_send_error(req, "400 Bad Request", "empty_body");
    }
    if (total > target->size) {
        ESP_LOGW(TAG, "upload of %u B does not fit %s (%" PRIu32 " B)", (unsigned) total,
                 target->label, target->size);
        return slate_api_send_error(req, "413 Payload Too Large", "too_large");
    }

    char *buffer = malloc(CHUNK_BYTES);
    if (buffer == NULL) {
        return slate_api_send_error(req, "503 Service Unavailable", "out_of_memory");
    }

    size_t received = 0;
    bool truncated = false;
    while (received < IMAGE_PREFIX_BYTES && received < total) {
        size_t want = (total < CHUNK_BYTES ? total : CHUNK_BYTES) - received;
        int chunk = receive_some(req, buffer + received, want);
        if (chunk < 0) {
            truncated = true;
            break;
        }
        received += chunk;
    }

    if (truncated) {
        ESP_LOGW(TAG, "upload stopped after %u of %u B", (unsigned) received, (unsigned) total);
        free(buffer);
        return slate_api_send_error(req, "400 Bad Request", "truncated");
    }

    const esp_app_desc_t *incoming = NULL;
    if (received < IMAGE_PREFIX_BYTES || !image_prefix_is_plausible(buffer, &incoming)) {
        ESP_LOGW(TAG, "body is not an %s application image", CONFIG_IDF_TARGET);
        free(buffer);
        return slate_api_send_error(req, "400 Bad Request", "not_an_image");
    }

    /* Copied out before the buffer is reused, and by length: esp_app_desc_t's
     * fields are fixed-size arrays that need not be NUL-terminated. */
    char version[sizeof(incoming->version) + 1];
    memcpy(version, incoming->version, sizeof(incoming->version));
    version[sizeof(incoming->version)] = '\0';

    const esp_app_desc_t *running = esp_app_get_description();
    ESP_LOGI(TAG, "receiving \"%s\" (%u B) into %s, over the running \"%s\"", version,
             (unsigned) total, target->label, running->version);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, total, &handle);
    if (err != ESP_OK) {
        free(buffer);
        return send_ota_error(req, "esp_ota_begin", err);
    }

    err = esp_ota_write(handle, buffer, received);
    while (err == ESP_OK && received < total) {
        size_t remaining = total - received;
        int chunk = receive_some(req, buffer, remaining < CHUNK_BYTES ? remaining : CHUNK_BYTES);
        if (chunk < 0) {
            truncated = true;
            break;
        }
        err = esp_ota_write(handle, buffer, chunk);
        received += chunk;
    }
    free(buffer);

    if (truncated) {
        esp_ota_abort(handle);
        ESP_LOGW(TAG, "upload stopped after %u of %u B", (unsigned) received, (unsigned) total);
        return slate_api_send_error(req, "400 Bad Request", "truncated");
    }
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        return send_ota_error(req, "esp_ota_write", err);
    }

    /* esp_ota_end() consumes the handle whether or not it succeeds, so there
     * is nothing left to abort below this line. */
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        return send_ota_error(req, "esp_ota_end", err);
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return send_ota_error(req, "esp_ota_set_boot_partition", err);
    }

    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        bool ok = cJSON_AddStringToObject(root, "status", "ok") != NULL &&
                  cJSON_AddStringToObject(root, "partition", target->label) != NULL &&
                  cJSON_AddNumberToObject(root, "bytes", total) != NULL &&
                  cJSON_AddStringToObject(root, "version", version) != NULL &&
                  cJSON_AddNumberToObject(root, "reboot_in_ms", REBOOT_DELAY_MS) != NULL;
        if (!ok) {
            cJSON_Delete(root);
            root = NULL;
        }
    }
    esp_err_t sent = slate_api_send_json(req, root);

    /*
     * The reboot happens even if the response did not: the boot partition has
     * already moved, and leaving the device running the previous image while
     * the flash says otherwise is the one outcome nobody could reason about.
     */
    ESP_LOGI(TAG, "%s written, booting it in %d ms", target->label, REBOOT_DELAY_MS);
    reboot_after_response();
    return sent;
}

esp_err_t slate_ota_init(void)
{
    /* §4.3 and §14: development OTA is a write, so it carries the token from
     * the first day it exists. The check is #10's wrapper, not a copy here. */
    static const httpd_uri_t upload = {
        .uri = SLATE_API_BASE_PATH "/ota/upload",
        .method = HTTP_POST,
        .handler = upload_handler,
    };

    esp_err_t err = slate_api_register_uri(&upload, SLATE_API_AUTH_DEVICE_TOKEN);
    if (err == ESP_OK) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        ESP_LOGI(TAG, "development OTA ready — running from %s", running->label);
    }
    return err;
}
