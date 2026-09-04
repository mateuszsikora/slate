/*
 * Slate — development OTA and boot health. See include/slate_ota.h for the
 * integration contract.
 *
 * DESIGN.md §11.1, §11.2, §4.1, §4.3.
 */

#include "slate_ota.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "slate_api.h"

static const char *TAG = "ota";

/*
 * Out of PSRAM rather than the HTTP task's 4 KB stack, and large enough that a
 * 1.2 MB image is not a hundred thousand flash calls. PSRAM because the buffer
 * only ever reaches httpd_req_recv() and esp_ota_write(), neither of which
 * needs DMA or internal memory — and §6.2's internal budget is the one #55's
 * access point has to fit into.
 */
#define CHUNK_BYTES 4096

/*
 * What has to be in hand before esp_ota_begin() is allowed to erase anything.
 * The number and the check that reads it are in slate_ota.h, because #37's
 * release channel has to refuse the same bytes for the same reason.
 */
#define IMAGE_PREFIX_BYTES SLATE_OTA_IMAGE_PREFIX_BYTES

_Static_assert(CHUNK_BYTES >= IMAGE_PREFIX_BYTES,
               "the first read must be able to hold the whole image prefix");

/* Long enough for the response to leave the socket, short enough that the
 * uploader does not wonder whether anything happened. */
#define REBOOT_DELAY_MS 500

/*
 * Two wall-clock bounds, because the upload holds the only HTTP task and a
 * counter of consecutive timeouts is not a bound at all: any single byte
 * resets it, so a client sending one byte inside every 5 s receive window
 * would own the server forever and hold an OTA handle open over an erased
 * partition while it did.
 *
 * STALL is silence — nothing arrived, the client is gone, give up quickly.
 * BUDGET is the whole request, and it is what makes a trickle terminate. It is
 * generous on purpose: it has to cover a 6 MB slot over bad WiFi, and it is
 * the same 300 s tools/ota/upload.sh gives curl.
 */
#define UPLOAD_STALL_US  (30 * 1000000LL)
#define UPLOAD_BUDGET_US (300 * 1000000LL)

/* --- Boot health -------------------------------------------------------- */

/* Measured from boot rather than from task creation: §11.2 gives the whole
 * image 60 seconds, not 60 seconds after whichever initializer reached it. */
#define HEALTH_DEADLINE_US   (60 * 1000000LL)
#define HEALTH_RETRY_MS      250
#define HEALTH_IO_TIMEOUT_MS 1000
#define HEALTH_TASK_STACK    4096
#define HEALTH_TASK_PRIORITY 4

typedef struct {
    bool found;
    esp_ip4_addr_t address;
    char ifkey[16];
} health_endpoint_t;

/* esp_netif pointers cannot safely escape the TCP/IP task: the setup component
 * destroys WIFI_AP_DEF as soon as the station returns. Copy the address and key
 * while iteration is serialised with that destruction. STA wins while both are
 * briefly up; AP remains the fallback when the router is absent. */
static esp_err_t snapshot_health_endpoint(void *ctx)
{
    health_endpoint_t *endpoint = ctx;
    health_endpoint_t ap = {0};

    for (esp_netif_t *netif = esp_netif_next_unsafe(NULL); netif != NULL;
        netif = esp_netif_next_unsafe(netif)) {
        const char *key = esp_netif_get_ifkey(netif);
        if (key == NULL) {
            continue;
        }
        bool station = strcmp(key, "WIFI_STA_DEF") == 0;
        bool setup_ap = strcmp(key, "WIFI_AP_DEF") == 0;
        if ((!station && !setup_ap) || !esp_netif_is_netif_up(netif)) {
            continue;
        }

        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
            continue;
        }

        health_endpoint_t candidate = {
            .found = true,
            .address = ip.ip,
        };
        strlcpy(candidate.ifkey, key, sizeof(candidate.ifkey));
        if (station) {
            *endpoint = candidate;
            return ESP_OK;
        }
        ap = candidate;
    }

    if (ap.found) {
        *endpoint = ap;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static bool wait_for_socket(int fd, bool writable, int64_t deadline_us)
{
    while (true) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return false;
        }

        fd_set read_fds;
        fd_set write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        if (writable) {
            FD_SET(fd, &write_fds);
        } else {
            FD_SET(fd, &read_fds);
        }
        struct timeval timeout = {
            .tv_sec = remaining_us / 1000000,
            .tv_usec = remaining_us % 1000000,
        };

        int ready = select(fd + 1, writable ? NULL : &read_fds,
                           writable ? &write_fds : NULL, NULL, &timeout);
        if (ready > 0) {
            return true;
        }
        if (ready == 0 || errno != EINTR) {
            return false;
        }
    }
}

static bool connect_until(int fd, const struct sockaddr_in *address, int64_t deadline_us)
{
    if (connect(fd, (const struct sockaddr *) address, sizeof(*address)) == 0) {
        return true;
    }
    if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
        return false;
    }
    if (!wait_for_socket(fd, true, deadline_us)) {
        return false;
    }

    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == 0 &&
           socket_error == 0;
}

static bool send_all_until(int fd, const char *data, size_t len, int64_t deadline_us)
{
    while (len > 0) {
        int sent = send(fd, data, len, 0);
        if (sent > 0) {
            data += sent;
            len -= sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            wait_for_socket(fd, true, deadline_us)) {
            continue;
        }
        return false;
    }
    return true;
}

static bool receive_response_until(int fd, int64_t deadline_us)
{
    char headers[512] = {0};
    size_t headers_used = 0;
    size_t header_bytes = 0;
    size_t content_length = 0;
    size_t total_received = 0;
    bool headers_complete = false;
    char response[256];

    while (esp_timer_get_time() < deadline_us) {
        int received = recv(fd, response, sizeof(response), 0);
        if (received > 0) {
            total_received += received;
            if (!headers_complete) {
                size_t copy = sizeof(headers) - headers_used - 1;
                if (copy > (size_t) received) {
                    copy = received;
                }
                memcpy(headers + headers_used, response, copy);
                headers_used += copy;

                char *headers_end = strstr(headers, "\r\n\r\n");
                if (headers_end == NULL) {
                    if (copy < (size_t) received || headers_used == sizeof(headers) - 1) {
                        return false;
                    }
                    continue;
                }

                bool status_ok = strncmp(headers, "HTTP/1.0 200 ", 13) == 0 ||
                                 strncmp(headers, "HTTP/1.1 200 ", 13) == 0;
                char *length_value = strstr(headers, "\r\nContent-Length: ");
                if (!status_ok || length_value == NULL) {
                    return false;
                }
                length_value += sizeof("\r\nContent-Length: ") - 1;
                char *length_end = NULL;
                unsigned long long parsed_length = strtoull(length_value, &length_end, 10);
                if (length_end == length_value || strncmp(length_end, "\r\n", 2) != 0 ||
                    parsed_length > SIZE_MAX) {
                    return false;
                }

                header_bytes = headers_end + 4 - headers;
                content_length = parsed_length;
                headers_complete = true;
            }
            if (headers_complete && total_received - header_bytes >= content_length) {
                return true;
            }
            continue;
        }
        if (received == 0) {
            return headers_complete && total_received - header_bytes >= content_length;
        }
        if (errno == EINTR) {
            continue;
        }
        if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
            wait_for_socket(fd, false, deadline_us)) {
            continue;
        }
        return false;
    }
    return false;
}

static bool make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    return true;
}

/* A flag that slate_api_init() returned would prove registration, not that the
 * HTTP task can answer. This traverses lwIP and the real public route on the
 * address a second machine would use, which is exactly §11.2's criterion. */
static bool info_answers(const health_endpoint_t *endpoint, int64_t deadline_us)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return false;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
        .sin_addr.s_addr = endpoint->address.addr,
    };
    static const char REQUEST[] =
        "GET " SLATE_API_BASE_PATH "/info HTTP/1.0\r\n"
        "Host: slate\r\nConnection: close\r\n\r\n";

    bool complete = make_nonblocking(fd) && connect_until(fd, &address, deadline_us) &&
                    send_all_until(fd, REQUEST, sizeof(REQUEST) - 1, deadline_us) &&
                    receive_response_until(fd, deadline_us);
    close(fd);

    /* Drain the whole response, but never past the attempt's absolute deadline.
     * Reading only the status would
     * make the server's send of the JSON body fail with ECONNRESET even though
     * the health check passed — noise that looks like a sick API in the exact
     * log this check exists to make trustworthy. Reading the announced body
     * length also proves the one HTTP task finished /info and is free to accept
     * the next OTA, without depending on the server closing a persistent TCP
     * connection. */
    return complete;
}

static void rollback_now(void)
{
    ESP_LOGE(TAG, "boot health deadline expired — rolling back");
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();

    /* Success never returns. If marking failed, a reset while PENDING_VERIFY is
     * still the bootloader's automatic rollback path; do not leave the bad
     * image running indefinitely merely because the explicit path failed. */
    ESP_LOGE(TAG, "marking the image invalid failed: %s — restarting pending image",
             esp_err_to_name(err));
    esp_restart();
}

static void boot_health_task(void *arg)
{
    (void) arg;
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGW(TAG, "%s is pending verification; GET /info must answer within 60 s",
             running->label);

    while (esp_timer_get_time() < HEALTH_DEADLINE_US) {
        health_endpoint_t endpoint = {0};
        int64_t attempt_deadline = esp_timer_get_time() + HEALTH_IO_TIMEOUT_MS * 1000LL;
        if (attempt_deadline > HEALTH_DEADLINE_US) {
            attempt_deadline = HEALTH_DEADLINE_US;
        }
        if (esp_netif_tcpip_exec(snapshot_health_endpoint, &endpoint) == ESP_OK &&
            info_answers(&endpoint, attempt_deadline) &&
            esp_timer_get_time() < HEALTH_DEADLINE_US) {
            int64_t elapsed_ms = esp_timer_get_time() / 1000;
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                char address[16];
                esp_ip4addr_ntoa(&endpoint.address, address, sizeof(address));
                ESP_LOGI(TAG, "boot health passed on %s at %s after %" PRId64
                              " ms — %s is valid",
                         endpoint.ifkey, address, elapsed_ms, running->label);
                vTaskDelete(NULL);
                return;
            }
            ESP_LOGE(TAG, "marking the healthy image valid: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(HEALTH_RETRY_MS));
    }

    rollback_now();
}

esp_err_t slate_ota_start_boot_health(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_ERR_NOT_FOUND || (err == ESP_OK && state != ESP_OTA_IMG_PENDING_VERIFY)) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading OTA state for %s: %s", running->label, esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(boot_health_task, "ota_health", HEALTH_TASK_STACK, NULL,
                    HEALTH_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the boot health task");
        rollback_now();
        return ESP_ERR_NO_MEM; /* rollback_now does not return on a working platform */
    }
    return ESP_OK;
}

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

/* One spelling of "the device is out of heap" for the whole API: #10's helper
 * answers 500 `out_of_memory`, and an OTA that invented its own status for the
 * same condition would make a client's retry rule depend on the route. */
static esp_err_t refuse_out_of_memory(httpd_req_t *req)
{
    return slate_api_refuse_and_close(req, "500 Internal Server Error", "out_of_memory");
}

/*
 * One place where an esp_ota_* failure becomes an HTTP answer, so the two
 * cases a caller can do something about are named and the rest are honestly
 * one word. Anything that reaches `ota_failed` is a device-side fault, not a
 * bad request.
 */
static esp_err_t refuse_ota_error(httpd_req_t *req, const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "%s: %s", what, esp_err_to_name(err));

    switch (err) {
    case ESP_ERR_OTA_VALIDATE_FAILED:
        return slate_api_refuse_and_close(req, "400 Bad Request", "invalid_image");

    case ESP_ERR_OTA_ROLLBACK_INVALID_STATE:
        /* Reachable once #12 enables rollback: ESP-IDF refuses to write a new
         * image while the running one is still pending verification. Named so
         * the answer says what to wait for instead of looking like a fault. */
        return slate_api_refuse_and_close(req, "409 Conflict", "pending_verify");

    case ESP_ERR_NO_MEM:
        return refuse_out_of_memory(req);

    default:
        return slate_api_refuse_and_close(req, "500 Internal Server Error", "ota_failed");
    }
}

/* --- Upload ------------------------------------------------------------- */

/** Read what is available, retrying the timeout a merely slow client produces. */
static int receive_some(httpd_req_t *req, char *buffer, size_t len, int64_t budget_ends)
{
    int64_t stall_ends = esp_timer_get_time() + UPLOAD_STALL_US;
    int64_t deadline = stall_ends < budget_ends ? stall_ends : budget_ends;

    while (esp_timer_get_time() < deadline) {
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

/** How much of the body to ask for next, never past the end of it. */
static size_t chunk_want(size_t total, size_t received)
{
    size_t remaining = total - received;
    return remaining < CHUNK_BYTES ? remaining : CHUNK_BYTES;
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
const esp_app_desc_t *slate_ota_image_prefix(const void *prefix, size_t len)
{
    if (len < IMAGE_PREFIX_BYTES) {
        return NULL;
    }

    const char *bytes = prefix;
    const esp_image_header_t *header = (const esp_image_header_t *) bytes;
    const esp_app_desc_t *description =
        (const esp_app_desc_t *) (bytes + sizeof(esp_image_header_t) +
                                  sizeof(esp_image_segment_header_t));

    if (header->magic != ESP_IMAGE_HEADER_MAGIC) {
        return NULL;
    }
    /* An ESP32 image on an ESP32-S3 is a boot loop, and it is a plain mistake
     * to make with two boards on one desk. */
    if (header->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return NULL;
    }
    if (description->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return NULL;
    }
    return description;
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "no second application partition to write to");
        return slate_api_refuse_and_close(req, "500 Internal Server Error",
                                          "no_ota_partition");
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
        return slate_api_refuse_and_close(req, "400 Bad Request", "empty_body");
    }
    if (total > target->size) {
        ESP_LOGW(TAG, "upload of %u B does not fit %s (%" PRIu32 " B)", (unsigned) total,
                 target->label, target->size);
        return slate_api_refuse_and_close(req, "413 Payload Too Large", "too_large");
    }

    char *buffer = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        buffer = malloc(CHUNK_BYTES);
    }
    if (buffer == NULL) {
        return refuse_out_of_memory(req);
    }

    const int64_t budget_ends = esp_timer_get_time() + UPLOAD_BUDGET_US;
    size_t received = 0;
    bool truncated = false;
    while (received < IMAGE_PREFIX_BYTES && received < total) {
        int chunk = receive_some(req, buffer + received, chunk_want(total, received), budget_ends);
        if (chunk < 0) {
            truncated = true;
            break;
        }
        received += chunk;
    }

    if (truncated) {
        ESP_LOGW(TAG, "upload stopped after %u of %u B", (unsigned) received, (unsigned) total);
        free(buffer);
        return slate_api_refuse_and_close(req, "400 Bad Request", "truncated");
    }

    const esp_app_desc_t *incoming = slate_ota_image_prefix(buffer, received);
    if (incoming == NULL) {
        ESP_LOGW(TAG, "body is not an %s application image", CONFIG_IDF_TARGET);
        free(buffer);
        return slate_api_refuse_and_close(req, "400 Bad Request", "not_an_image");
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
        return refuse_ota_error(req, "esp_ota_begin", err);
    }

    /*
     * Past this line the target slot is erased, so every failure below costs
     * the image that was in it — which is what slate_ota.h and §11.1 say, and
     * what #12's rollback has to know about an interrupted flash.
     */
    err = esp_ota_write(handle, buffer, received);
    while (err == ESP_OK && received < total) {
        int chunk = receive_some(req, buffer, chunk_want(total, received), budget_ends);
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
        ESP_LOGW(TAG, "upload stopped after %u of %u B, %s is now blank", (unsigned) received,
                 (unsigned) total, target->label);
        return slate_api_refuse_and_close(req, "400 Bad Request", "truncated");
    }
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        return refuse_ota_error(req, "esp_ota_write", err);
    }

    /* esp_ota_end() consumes the handle whether or not it succeeds, so there
     * is nothing left to abort below this line. */
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        return refuse_ota_error(req, "esp_ota_end", err);
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return refuse_ota_error(req, "esp_ota_set_boot_partition", err);
    }

    /*
     * 200 is what says the image was accepted and is about to boot; the body
     * carries only what the status cannot. There is deliberately no "status":
     * "ok" field duplicating the code — a client given two ways to ask the
     * same question ends up matching on the wrong one — and no announced
     * reboot delay, because REBOOT_DELAY_MS is this firmware's constant rather
     * than a schedule the device can promise: a timer that could not be
     * created restarts it immediately.
     */
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        bool ok = cJSON_AddStringToObject(root, "partition", target->label) != NULL &&
                  cJSON_AddNumberToObject(root, "bytes", total) != NULL &&
                  cJSON_AddStringToObject(root, "version", version) != NULL;
        if (!ok) {
            cJSON_Delete(root);
            root = NULL;
        }
    }

    esp_err_t sent;
    if (root != NULL) {
        sent = slate_api_send_json(req, root);
    } else {
        /*
         * The boot partition has already moved, so the one thing this must not
         * do is report a failure. slate_api_send_json(req, NULL) would answer
         * 500 `out_of_memory` — "nothing happened" everywhere else in the API
         * — about a device that is a moment away from booting the image it was
         * just given. An empty object keeps the 200 that carries the outcome
         * and drops the three fields that were only ever detail.
         */
        httpd_resp_set_type(req, "application/json");
        sent = httpd_resp_sendstr(req, "{}");
    }

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
