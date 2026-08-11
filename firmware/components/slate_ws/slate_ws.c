/*
 * Slate — authenticated WebSocket diagnostics and editor event channel.
 *
 * design.md §4.2 and §11.3. Log producers only format into an 8 KiB record
 * ring. All network writes are queued onto esp_http_server's own task, where a
 * session-generation check prevents an fd reused by a new, unauthenticated
 * client from receiving the previous client's backlog.
 */

#include "slate_ws.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "slate_api.h"
#include "slate_display.h"
#include "slate_state.h"
#include "slate_store.h"
#include "slate_wifi.h"

#ifdef SLATE_WS_SELFTEST
#include "lwip/inet.h"
#include "lwip/sockets.h"
#endif

static const char *TAG = "ws";

#define SLATE_WS_MAX_CLIENTS       4
/*
 * One, and sized from what §5.4 actually asks for rather than from the two
 * provider ids §5.1 names.
 *
 * Attachment is the direct provider's mechanism: §4.2 describes the client that
 * uses it as "an integration client that wants to receive direct-provider
 * actions", and the Home Assistant adapter is a WebSocket client of Home
 * Assistant rather than a consumer of this server's. A second entry would
 * therefore reserve a whole queue — measured at 1 192 B of internal .bss — for
 * something that by design never attaches, out of the 104 167 B §6.2 records as
 * free once the radio is up. That is the memory the design spends a section
 * defending, and speculative generality is a poor thing to spend it on.
 *
 * Raising this is one constant and costs that much again per entry. A provider
 * that cannot register says so at boot rather than failing quietly later.
 */
#define SLATE_WS_MAX_PROVIDERS     1
#define SLATE_WS_ACTION_BYTES      288
#define SLATE_WS_ACTION_QUEUE      4
#define SLATE_WS_LOG_RING_BYTES    8192
#define SLATE_WS_LOG_CAPTURE_BYTES 512
#define SLATE_WS_RX_BYTES          256
#define SLATE_WS_JSON_BYTES        (SLATE_WS_LOG_CAPTURE_BYTES * 6 + 96)
#define SLATE_WS_TASK_STACK        6144
#define SLATE_WS_TASK_PRIORITY     3
#define SLATE_WS_POLL_MS           50
#define SLATE_WS_AUTH_TIMEOUT_US   (5LL * 1000000)
#define SLATE_WS_STATUS_PERIOD_US  (15LL * 1000000)
#define SLATE_WS_EDIT_TIMEOUT_US   (60LL * 1000000)

typedef struct {
    bool used;
    bool authenticated;
    bool closing;
    bool send_pending;
    bool initial_status_pending;
    bool status_pending;
    bool reloaded_pending;
    int fd;
    uint32_t generation;
    int64_t connected_at_us;
    uint64_t log_cursor;
    uint64_t backlog_end;
    unsigned reloaded_schema;
    size_t reloaded_tiles;
} ws_client_t;

typedef enum {
    SEND_LOG,
    SEND_STATUS,
    SEND_RELOADED,
    SEND_ACTION,
    SEND_POLICY_CLOSE,
} send_kind_t;

/**
 * A provider's registration and its single attached action consumer (§5.4).
 *
 * The queue is short and static. An action that cannot be queued is refused at
 * the point of dispatch, which #18 turns into §5.3's immediate revert — a
 * consumer far enough behind to fill four frames is not one whose answer would
 * have arrived inside the three seconds anyway, and a queue that grew instead
 * would spend PSRAM on taps nobody is still waiting for.
 */
typedef struct {
    const char *id;
    slate_ws_attach_fn on_attach;
    slate_ws_result_fn on_result;
    void *ctx;
    ws_client_t *consumer;
    uint32_t consumer_generation;
    size_t head;
    size_t count;
    uint16_t frame_len[SLATE_WS_ACTION_QUEUE];
    char frame[SLATE_WS_ACTION_QUEUE][SLATE_WS_ACTION_BYTES];
} ws_provider_t;

typedef struct {
    ws_client_t *client;
    uint32_t generation;
    int fd;
    send_kind_t kind;
    uint64_t log_seq;
    size_t len;
    uint8_t payload[];
} send_work_t;

static httpd_handle_t s_server;
static TaskHandle_t s_task;
static ws_client_t s_clients[SLATE_WS_MAX_CLIENTS];
static uint32_t s_next_generation;
static slate_ws_mode_t s_mode;
static slate_ws_mode_observer_fn s_mode_observer;
static void *s_mode_observer_ctx;
static ws_provider_t s_providers[SLATE_WS_MAX_PROVIDERS];
static size_t s_provider_count;
static ws_client_t *s_edit_owner;
static uint32_t s_edit_owner_generation;
static int64_t s_edit_last_ping_us;
static int64_t s_last_status_us;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

/* Length-prefixed complete records. The byte array itself — not the metadata —
 * is the 8 KiB ring §11.3 requires. Popping always removes a whole record, so
 * a reconnect never begins in the middle of a UTF-8/log line. */
static uint8_t s_log_ring[SLATE_WS_LOG_RING_BYTES];
static size_t s_log_head;
static size_t s_log_tail;
static size_t s_log_used;
static uint64_t s_log_oldest_seq;
static uint64_t s_log_next_seq;
static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_previous_vprintf;
static SemaphoreHandle_t s_capture_mutex;
static StaticSemaphore_t s_capture_mutex_storage;
static portMUX_TYPE s_capture_init_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_capture_line[SLATE_WS_LOG_CAPTURE_BYTES];

static void ring_write_bytes(size_t offset, const void *data, size_t len)
{
    const uint8_t *source = data;
    while (len > 0) {
        size_t part = SLATE_WS_LOG_RING_BYTES - offset;
        if (part > len) {
            part = len;
        }
        memcpy(s_log_ring + offset, source, part);
        offset = (offset + part) % SLATE_WS_LOG_RING_BYTES;
        source += part;
        len -= part;
    }
}

static void ring_read_bytes(size_t offset, void *data, size_t len)
{
    uint8_t *destination = data;
    while (len > 0) {
        size_t part = SLATE_WS_LOG_RING_BYTES - offset;
        if (part > len) {
            part = len;
        }
        memcpy(destination, s_log_ring + offset, part);
        offset = (offset + part) % SLATE_WS_LOG_RING_BYTES;
        destination += part;
        len -= part;
    }
}

static uint16_t ring_record_len(size_t offset)
{
    uint8_t encoded[2];
    ring_read_bytes(offset, encoded, sizeof(encoded));
    return (uint16_t) encoded[0] | ((uint16_t) encoded[1] << 8);
}

static void log_ring_append(const char *text, size_t len)
{
    if (!text || len == 0) {
        return;
    }

    const size_t max_payload = SLATE_WS_LOG_RING_BYTES - 2;
    if (len > max_payload) {
        text += len - max_payload;
        len = max_payload;
    }
    const size_t record_size = len + 2;

    portENTER_CRITICAL(&s_log_lock);
    while (SLATE_WS_LOG_RING_BYTES - s_log_used < record_size) {
        uint16_t old_len = ring_record_len(s_log_tail);
        size_t old_size = (size_t) old_len + 2;
        s_log_tail = (s_log_tail + old_size) % SLATE_WS_LOG_RING_BYTES;
        s_log_used -= old_size;
        s_log_oldest_seq++;
    }

    if (s_log_used == 0) {
        s_log_tail = s_log_head;
        s_log_oldest_seq = s_log_next_seq;
    }

    const uint8_t encoded[2] = {(uint8_t) len, (uint8_t) (len >> 8)};
    ring_write_bytes(s_log_head, encoded, sizeof(encoded));
    s_log_head = (s_log_head + sizeof(encoded)) % SLATE_WS_LOG_RING_BYTES;
    ring_write_bytes(s_log_head, text, len);
    s_log_head = (s_log_head + len) % SLATE_WS_LOG_RING_BYTES;
    s_log_used += record_size;
    s_log_next_seq++;
    portEXIT_CRITICAL(&s_log_lock);
}

static void log_ring_bounds(uint64_t *oldest, uint64_t *next)
{
    portENTER_CRITICAL(&s_log_lock);
    *oldest = s_log_oldest_seq;
    *next = s_log_next_seq;
    portEXIT_CRITICAL(&s_log_lock);
}

static bool log_ring_copy(uint64_t requested, char *out, size_t out_size,
                          uint64_t *actual_seq)
{
    if (!out || out_size < 2) {
        return false;
    }

    portENTER_CRITICAL(&s_log_lock);
    uint64_t seq = requested < s_log_oldest_seq ? s_log_oldest_seq : requested;
    if (seq >= s_log_next_seq || s_log_used == 0) {
        portEXIT_CRITICAL(&s_log_lock);
        return false;
    }

    size_t offset = s_log_tail;
    for (uint64_t current = s_log_oldest_seq; current < seq; current++) {
        offset = (offset + 2 + ring_record_len(offset)) % SLATE_WS_LOG_RING_BYTES;
    }

    uint16_t stored_len = ring_record_len(offset);
    offset = (offset + 2) % SLATE_WS_LOG_RING_BYTES;
    size_t copy_len = stored_len < out_size - 1 ? stored_len : out_size - 1;
    ring_read_bytes(offset, out, copy_len);
    out[copy_len] = '\0';
    *actual_seq = seq;
    portEXIT_CRITICAL(&s_log_lock);
    return true;
}

static int capture_vprintf(const char *format, va_list args)
{
    va_list serial_args;
    va_list capture_args;
    va_copy(serial_args, args);
    va_copy(capture_args, args);

    int result = s_previous_vprintf ? s_previous_vprintf(format, serial_args) : 0;
    int wanted = 0;
    if (xSemaphoreTake(s_capture_mutex, portMAX_DELAY) == pdTRUE) {
        wanted = vsnprintf(s_capture_line, sizeof(s_capture_line), format, capture_args);
        if (wanted > 0) {
            size_t len = (size_t) wanted;
            if (len >= sizeof(s_capture_line)) {
                len = sizeof(s_capture_line) - 1;
                if (len >= 4) {
                    memcpy(s_capture_line + len - 4, "...\n", 4);
                }
            }
            log_ring_append(s_capture_line, len);
            explicit_bzero(s_capture_line, sizeof(s_capture_line));
        }
        xSemaphoreGive(s_capture_mutex);
    }

    va_end(capture_args);
    va_end(serial_args);
    return result;
}

static ws_client_t *client_for_session(httpd_handle_t server, int fd)
{
    return (ws_client_t *) httpd_sess_get_ctx(server, fd);
}

static void provider_release_and_notify(ws_client_t *client, uint32_t generation);

static void client_session_freed(void *ctx)
{
    ws_client_t *client = ctx;
    if (!client) {
        return;
    }

    /* §5.4's detach arrives as a socket closing far more often than as anything
     * a client asked for, and this is the one place that sees both. Before the
     * slot is recycled, so a provider is never told about an attachment that
     * has already been overwritten by the next connection. */
    provider_release_and_notify(client, client->generation);

    portENTER_CRITICAL(&s_state_lock);
    uint32_t generation = client->generation;
    memset(client, 0, sizeof(*client));
    client->fd = -1;
    client->generation = generation;
    portEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t ws_connected(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    ws_client_t *client = NULL;

    /* Publish the immutable server handle before making a client visible to
     * the dispatcher task. The state lock is the memory barrier between them. */
    s_server = req->handle;

    portENTER_CRITICAL(&s_state_lock);
    for (size_t i = 0; i < SLATE_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].used) {
            client = &s_clients[i];
            memset(client, 0, sizeof(*client));
            client->used = true;
            client->fd = fd;
            client->generation = ++s_next_generation;
            client->connected_at_us = esp_timer_get_time();
            break;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!client) {
        return ESP_FAIL;
    }

    httpd_sess_set_ctx(req->handle, fd, client, client_session_freed);
    return ESP_OK;
}

static esp_err_t send_text(httpd_req_t *req, const char *text)
{
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *) text,
        .len = strlen(text),
    };
    return httpd_ws_send_frame(req, &frame);
}

static void send_policy_close(httpd_req_t *req)
{
    uint8_t payload[2] = {0x03, 0xF0}; /* RFC 6455 status 1008, network order. */
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_CLOSE,
        .payload = payload,
        .len = sizeof(payload),
    };
    httpd_ws_send_frame(req, &frame);
}

static bool parse_message(uint8_t *payload, size_t len, cJSON **out)
{
    payload[len] = '\0';
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts((char *) payload, &end, true);
    bool ok = root && cJSON_IsObject(root) && end == (char *) payload + len;
    if (!ok) {
        cJSON_Delete(root);
        root = NULL;
    }
    *out = root;
    return ok;
}

static void set_mode(slate_ws_mode_t mode, ws_client_t *owner, int64_t now)
{
    bool changed;
    slate_ws_mode_observer_fn observer;
    void *observer_ctx;
    portENTER_CRITICAL(&s_state_lock);
    changed = s_mode != mode;
    s_mode = mode;
    if (mode == SLATE_WS_MODE_EDIT && owner) {
        s_edit_owner = owner;
        s_edit_owner_generation = owner->generation;
        s_edit_last_ping_us = now;
    } else {
        s_edit_owner = NULL;
        s_edit_owner_generation = 0;
        s_edit_last_ping_us = 0;
    }
    observer = changed ? s_mode_observer : NULL;
    observer_ctx = s_mode_observer_ctx;
    portEXIT_CRITICAL(&s_state_lock);

    if (changed) {
        ESP_LOGI(TAG, "mode changed to %s", mode == SLATE_WS_MODE_EDIT ? "edit" : "normal");
    }
    if (observer != NULL) {
        observer(observer_ctx, mode);
    }
}

/* --- Provider action consumers (§4.2, §5.4) ------------------------------ */

static void mark_status_immediately(void);

static ws_provider_t *provider_find(const char *id)
{
    for (size_t i = 0; i < s_provider_count; i++) {
        if (strcmp(s_providers[i].id, id) == 0) {
            return &s_providers[i];
        }
    }
    return NULL;
}

/**
 * Release every attachment held by a client, and say who has to be told.
 *
 * The callbacks are collected rather than called here because the one caller
 * that matters runs inside esp_http_server's session teardown and the state
 * lock is a spinlock: a provider is entitled to do real work — change its
 * status, fail its outstanding actions — on being told its consumer is gone.
 */
static size_t provider_release(ws_client_t *client, uint32_t generation,
                               ws_provider_t **released, size_t capacity)
{
    size_t count = 0;

    portENTER_CRITICAL(&s_state_lock);
    for (size_t i = 0; i < s_provider_count; i++) {
        ws_provider_t *provider = &s_providers[i];
        if (provider->consumer != client || provider->consumer_generation != generation) {
            continue;
        }
        provider->consumer = NULL;
        provider->consumer_generation = 0;
        provider->head = 0;
        provider->count = 0;
        if (count < capacity) {
            released[count++] = provider;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    return count;
}

static void provider_release_and_notify(ws_client_t *client, uint32_t generation)
{
    /* Zeroed for the compiler rather than for the logic: provider_release()
     * writes every element it counts, but at -Os the whole chain down to
     * client_session_freed() inlines and GCC stops being able to see that the
     * returned count bounds the writes. -Og does not inline that far, so the
     * release build is the one that catches it. */
    ws_provider_t *released[SLATE_WS_MAX_PROVIDERS] = {0};
    size_t count = provider_release(client, generation, released, SLATE_WS_MAX_PROVIDERS);

    for (size_t i = 0; i < count; i++) {
        ESP_LOGI(TAG, "provider %s lost its action consumer", released[i]->id);
        if (released[i]->on_attach) {
            released[i]->on_attach(released[i]->ctx, false);
        }
    }
    if (count > 0) {
        mark_status_immediately();
    }
}

/**
 * §5.4's attachment, and the two answers it can receive.
 *
 * Success is deliberately silent, because §4.2 has no frame for it and ADR-4
 * makes that vocabulary a contract rather than a convenience. What a client
 * gets instead is the `status` frame the attachment itself makes stale — the
 * provider it just attached to reads `online` there, which is the fact it was
 * asking about rather than an acknowledgement that it asked.
 */
static void handle_provider_attach(httpd_req_t *req, ws_client_t *client, int fd, cJSON *root)
{
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "provider");
    if (!cJSON_IsString(name)) {
        send_text(req, "{\"type\":\"error\",\"error\":\"provider_required\"}");
        return;
    }

    ws_provider_t *provider = provider_find(name->valuestring);
    if (!provider) {
        send_text(req, "{\"type\":\"error\",\"error\":\"provider_not_found\"}");
        return;
    }

    bool attached = false;
    bool busy = false;
    portENTER_CRITICAL(&s_state_lock);
    if (client->used && client->fd == fd && client->authenticated) {
        if (provider->consumer == NULL) {
            provider->consumer = client;
            provider->consumer_generation = client->generation;
            provider->head = 0;
            provider->count = 0;
            attached = true;
        } else {
            /* Re-attaching is not an error for the client that is already the
             * consumer: §5.4's rule is that two processes cannot both operate
             * the same light, and a repeat from the one that holds it is not a
             * second process. */
            busy = provider->consumer != client ||
                   provider->consumer_generation != client->generation;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (busy) {
        ESP_LOGW(TAG, "provider %s already has an action consumer", provider->id);
        send_text(req, "{\"type\":\"error\",\"error\":\"provider_busy\"}");
        return;
    }
    if (attached) {
        ESP_LOGI(TAG, "provider %s gained an action consumer", provider->id);
        if (provider->on_attach) {
            provider->on_attach(provider->ctx, true);
        }
        mark_status_immediately();
    }

    /* Neither, and deliberately silent: the client stopped being the current
     * authenticated session between the dispatch above and the lock, which
     * happens when a send failed and the session is already being torn down.
     * There is nothing left to answer on — a frame queued to a closing session
     * is one nobody reads — and the close itself is the answer. */
}

/**
 * §4.2's `action_result`, from the consumer that was sent the action.
 *
 * Accepted only from the attached client. A result from anyone else is not a
 * late answer to route on but a second process operating the same resource,
 * which is the thing the single attachment exists to prevent.
 */
static void handle_action_result(ws_client_t *client, cJSON *root)
{
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *success = cJSON_GetObjectItemCaseSensitive(root, "success");
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (!cJSON_IsNumber(id) || id->valuedouble < 0 || id->valuedouble > UINT32_MAX ||
        !cJSON_IsBool(success)) {
        return;
    }

    ws_provider_t *provider = NULL;
    portENTER_CRITICAL(&s_state_lock);
    for (size_t i = 0; i < s_provider_count; i++) {
        if (s_providers[i].consumer == client &&
            s_providers[i].consumer_generation == client->generation) {
            provider = &s_providers[i];
            break;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (provider && provider->on_result) {
        provider->on_result(provider->ctx, (uint32_t) id->valuedouble,
                            cJSON_IsTrue(success),
                            cJSON_IsString(error) ? error->valuestring : NULL);
    }
}

esp_err_t slate_ws_provider_register(const slate_ws_provider_t *provider)
{
    if (!provider || !provider->id || provider->id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (provider_find(provider->id)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_provider_count >= SLATE_WS_MAX_PROVIDERS) {
        return ESP_ERR_NO_MEM;
    }

    ws_provider_t *entry = &s_providers[s_provider_count];
    memset(entry, 0, sizeof(*entry));
    entry->id = provider->id;
    entry->on_attach = provider->on_attach;
    entry->on_result = provider->on_result;
    entry->ctx = provider->ctx;

    /* Published last: the dispatcher task walks the table by count, so an entry
     * has to be complete before the count can reach it. */
    portENTER_CRITICAL(&s_state_lock);
    s_provider_count++;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

bool slate_ws_provider_is_attached(const char *id)
{
    ws_provider_t *provider = id ? provider_find(id) : NULL;
    if (!provider) {
        return false;
    }

    portENTER_CRITICAL(&s_state_lock);
    bool attached = provider->consumer != NULL;
    portEXIT_CRITICAL(&s_state_lock);
    return attached;
}

esp_err_t slate_ws_provider_send(const char *id, const char *text, size_t len)
{
    ws_provider_t *provider = id ? provider_find(id) : NULL;
    if (!provider || !text) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0 || len > SLATE_WS_ACTION_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&s_state_lock);
    if (provider->consumer == NULL) {
        err = ESP_ERR_INVALID_STATE;
    } else if (provider->count >= SLATE_WS_ACTION_QUEUE) {
        err = ESP_ERR_NO_MEM;
    } else {
        size_t slot = (provider->head + provider->count) % SLATE_WS_ACTION_QUEUE;
        memcpy(provider->frame[slot], text, len);
        provider->frame_len[slot] = (uint16_t) len;
        provider->count++;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return err;
}

/** Copy this client's next queued frame, if it is a consumer with one waiting. */
static ws_provider_t *provider_peek(const ws_client_t *client, uint32_t generation, char *out,
                                    size_t *len)
{
    ws_provider_t *found = NULL;

    portENTER_CRITICAL(&s_state_lock);
    for (size_t i = 0; i < s_provider_count; i++) {
        ws_provider_t *provider = &s_providers[i];
        if (provider->consumer != client || provider->consumer_generation != generation ||
            provider->count == 0) {
            continue;
        }
        *len = provider->frame_len[provider->head];
        memcpy(out, provider->frame[provider->head], *len);
        found = provider;
        break;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return found;
}

/**
 * Drop the frame provider_peek() returned, once it is on its way.
 *
 * Handed over when the send is queued rather than when it lands, because the
 * only failure after that point closes the session — and a consumer that has
 * gone is one §5.3's timeout answers for, not one a retry would reach.
 */
static void provider_drop(ws_provider_t *provider)
{
    portENTER_CRITICAL(&s_state_lock);
    if (provider->count > 0) {
        provider->head = (provider->head + 1) % SLATE_WS_ACTION_QUEUE;
        provider->count--;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t handle_authenticated(httpd_req_t *req, ws_client_t *client, int fd, cJSON *root)
{
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        return ESP_OK;
    }

    const int64_t now = esp_timer_get_time();
    if (strcmp(type->valuestring, "ping") == 0) {
        portENTER_CRITICAL(&s_state_lock);
        if (client->used && client->fd == fd && client->authenticated) {
            if (s_mode == SLATE_WS_MODE_EDIT && s_edit_owner == client &&
                s_edit_owner_generation == client->generation) {
                s_edit_last_ping_us = now;
            }
        }
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }

    if (strcmp(type->valuestring, "mode") == 0) {
        cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        if (cJSON_IsString(mode) && strcmp(mode->valuestring, "edit") == 0) {
            set_mode(SLATE_WS_MODE_EDIT, client, now);
        } else if (cJSON_IsString(mode) && strcmp(mode->valuestring, "normal") == 0) {
            set_mode(SLATE_WS_MODE_NORMAL, NULL, now);
        }
        return ESP_OK;
    }

    if (strcmp(type->valuestring, "provider_attach") == 0) {
        handle_provider_attach(req, client, fd, root);
        return ESP_OK;
    }

    if (strcmp(type->valuestring, "action_result") == 0) {
        handle_action_result(client, root);
    }
    return ESP_OK;
}

static esp_err_t handle_first_frame(httpd_req_t *req, ws_client_t *client, int fd,
                                    cJSON *root)
{
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "token");
    bool token_is_string = cJSON_IsString(token);
    bool valid = cJSON_IsString(type) && strcmp(type->valuestring, "auth") == 0 &&
                 token_is_string && slate_store_device_token_matches(token->valuestring);
    if (token_is_string) {
        explicit_bzero(token->valuestring, strlen(token->valuestring));
    }
    if (!valid) {
        send_text(req, "{\"type\":\"auth_invalid\"}");
        send_policy_close(req);
        return ESP_FAIL;
    }

    uint64_t oldest;
    uint64_t next;
    log_ring_bounds(&oldest, &next);
    const int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_state_lock);
    bool current = client->used && client->fd == fd && !client->authenticated &&
                   now - client->connected_at_us < SLATE_WS_AUTH_TIMEOUT_US;
    if (current) {
        client->authenticated = true;
        client->log_cursor = oldest;
        client->backlog_end = next;
        client->initial_status_pending = true;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!current) {
        send_policy_close(req);
        return ESP_FAIL;
    }
    if (send_text(req, "{\"type\":\"auth_ok\"}") != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    ws_client_t *client = client_for_session(req->handle, fd);
    if (!client) {
        return ESP_FAIL;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }

    if (frame.type == HTTPD_WS_TYPE_PONG) {
        uint8_t pong[126];
        frame.payload = pong;
        return httpd_ws_recv_frame(req, &frame, sizeof(pong));
    }

    portENTER_CRITICAL(&s_state_lock);
    bool authenticated = client->used && client->fd == fd && client->authenticated;
    portEXIT_CRITICAL(&s_state_lock);

    if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len >= SLATE_WS_RX_BYTES) {
        if (!authenticated) {
            send_text(req, "{\"type\":\"auth_invalid\"}");
            send_policy_close(req);
        }
        return ESP_FAIL;
    }

    uint8_t payload[SLATE_WS_RX_BYTES];
    frame.payload = payload;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = NULL;
    bool parsed = parse_message(payload, frame.len, &root);

    if (!authenticated) {
        if (!parsed) {
            send_text(req, "{\"type\":\"auth_invalid\"}");
            send_policy_close(req);
            return ESP_FAIL;
        }
        err = handle_first_frame(req, client, fd, root);
    } else if (parsed) {
        err = handle_authenticated(req, client, fd, root);
    } else {
        err = ESP_OK; /* Unknown future application frames are ignored. */
    }

    cJSON_Delete(root);
    explicit_bzero(payload, sizeof(payload));
    return err;
}

static bool client_is_current(const send_work_t *work, bool must_be_authenticated)
{
    bool current;
    portENTER_CRITICAL(&s_state_lock);
    current = work->client->used && work->client->generation == work->generation &&
              work->client->fd == work->fd &&
              (!must_be_authenticated || work->client->authenticated);
    portEXIT_CRITICAL(&s_state_lock);
    return current && httpd_sess_get_ctx(s_server, work->fd) == work->client;
}

static void finish_send(send_work_t *work, esp_err_t result, bool was_current)
{
    bool close_session = was_current &&
                         (result != ESP_OK || work->kind == SEND_POLICY_CLOSE);

    portENTER_CRITICAL(&s_state_lock);
    if (was_current && work->client->used &&
        work->client->generation == work->generation && work->client->fd == work->fd) {
        work->client->send_pending = false;
        if (result == ESP_OK) {
            if (work->kind == SEND_LOG && work->client->log_cursor <= work->log_seq) {
                work->client->log_cursor = work->log_seq + 1;
            } else if (work->kind == SEND_STATUS) {
                work->client->status_pending = false;
                work->client->initial_status_pending = false;
            } else if (work->kind == SEND_RELOADED) {
                work->client->reloaded_pending = false;
            }
        }
        if (close_session) {
            work->client->closing = true;
            work->client->authenticated = false;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (close_session) {
        httpd_sess_trigger_close(s_server, work->fd);
    }
}

static void send_on_httpd(void *ctx)
{
    send_work_t *work = ctx;
    const bool auth_required = work->kind != SEND_POLICY_CLOSE;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool current = client_is_current(work, auth_required);

    if (current) {
        httpd_ws_frame_t frame = {
            .type = work->kind == SEND_POLICY_CLOSE ? HTTPD_WS_TYPE_CLOSE
                                                    : HTTPD_WS_TYPE_TEXT,
            .payload = work->payload,
            .len = work->len,
        };
        result = httpd_ws_send_frame_async(s_server, work->fd, &frame);
    }

    finish_send(work, result, current);
    explicit_bzero(work->payload, work->len);
    free(work);
}

static esp_err_t queue_send(ws_client_t *client, uint32_t generation, send_kind_t kind,
                            uint64_t log_seq, const void *payload, size_t len)
{
    send_work_t *work = malloc(sizeof(*work) + len);
    if (!work) {
        return ESP_ERR_NO_MEM;
    }

    *work = (send_work_t) {
        .client = client,
        .generation = generation,
        .fd = client->fd,
        .kind = kind,
        .log_seq = log_seq,
        .len = len,
    };
    memcpy(work->payload, payload, len);

    portENTER_CRITICAL(&s_state_lock);
    bool current = client->used && client->generation == generation && !client->send_pending;
    if (current) {
        client->send_pending = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (!current) {
        free(work);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = httpd_queue_work(s_server, send_on_httpd, work);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        if (client->used && client->generation == generation) {
            client->send_pending = false;
        }
        portEXIT_CRITICAL(&s_state_lock);
        free(work);
    }
    return err;
}

static const char *log_level(const char *line)
{
    while ((unsigned char) *line == 0x1B) {
        line++;
        if (*line == '[') {
            while (*line && *line != 'm') {
                line++;
            }
            if (*line == 'm') {
                line++;
            }
        }
    }
    switch (*line) {
    case 'E': return "error";
    case 'W': return "warn";
    case 'D': return "debug";
    case 'V': return "verbose";
    default:  return "info";
    }
}

static bool append_json_char(char *out, size_t out_size, size_t *used, char value)
{
    if (*used + 1 >= out_size) {
        return false;
    }
    out[(*used)++] = value;
    return true;
}

static size_t format_log_json(const char *line, char *out, size_t out_size)
{
    int prefix = snprintf(out, out_size, "{\"type\":\"log\",\"level\":\"%s\",\"msg\":\"",
                          log_level(line));
    if (prefix < 0 || (size_t) prefix >= out_size) {
        return 0;
    }
    size_t used = (size_t) prefix;

    for (const unsigned char *p = (const unsigned char *) line; *p; p++) {
        if (*p == 0x1B && p[1] == '[') {
            p += 2;
            while (*p && *p != 'm') {
                p++;
            }
            if (!*p) {
                break;
            }
            continue;
        }
        if ((*p == '\n' || *p == '\r') && p[1] == '\0') {
            continue;
        }

        const char *escape = NULL;
        switch (*p) {
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default: break;
        }
        if (escape) {
            while (*escape) {
                if (!append_json_char(out, out_size, &used, *escape++)) {
                    return 0;
                }
            }
        } else if (*p < 0x20) {
            int wrote = snprintf(out + used, out_size - used, "\\u%04x", *p);
            if (wrote != 6 || used + 6 >= out_size) {
                return 0;
            }
            used += 6;
        } else if (!append_json_char(out, out_size, &used, (char) *p)) {
            return 0;
        }
    }

    if (used + 3 > out_size) {
        return 0;
    }
    out[used++] = '"';
    out[used++] = '}';
    out[used] = '\0';
    return used;
}

static size_t format_status_json(char *out, size_t out_size)
{
    slate_wifi_status_t wifi;
    slate_wifi_status(&wifi);
    slate_display_heap_metrics_t lvgl;
    slate_display_heap_metrics(&lvgl);

    char wifi_value[16];
    char lvgl_free[24];
    char lvgl_frag[16];
    if (wifi.connected) {
        snprintf(wifi_value, sizeof(wifi_value), "%d", wifi.rssi);
    } else {
        snprintf(wifi_value, sizeof(wifi_value), "null");
    }
    if (lvgl.available) {
        snprintf(lvgl_free, sizeof(lvgl_free), "%u", (unsigned) lvgl.free_size);
        snprintf(lvgl_frag, sizeof(lvgl_frag), "%u", (unsigned) lvgl.frag_pct);
    } else {
        snprintf(lvgl_free, sizeof(lvgl_free), "null");
        snprintf(lvgl_frag, sizeof(lvgl_frag), "null");
    }

    /*
     * `direct` is read out of the store rather than stated here, which is what
     * makes §5.4's distinction visible to a client: the provider is `degraded`
     * while it can accept state with no action consumer attached and `online`
     * while one is. Attaching therefore changes this frame, and that change is
     * the acknowledgement §4.2 does not spell a frame for.
     *
     * `ha` keeps its stand-in until M3 registers the real one. §5.1 makes it a
     * provider that is always present and `unconfigured` until it has
     * credentials, so dropping the entry until its adapter exists would report
     * something less true than this does.
     */
    int len = snprintf(out, out_size,
                       "{\"type\":\"status\",\"providers\":{\"direct\":\"%s\","
                       "\"ha\":\"%s\"},\"wifi\":%s,"
                       "\"heap_free\":%u,\"lvgl_heap_free\":%s,\"lvgl_frag_pct\":%s}",
                       slate_provider_status_str(slate_state_provider_status("direct")),
                       slate_store_ha_token_is_set() ? "offline" : "unconfigured",
                       wifi_value, (unsigned) esp_get_free_heap_size(), lvgl_free, lvgl_frag);
    return len > 0 && (size_t) len < out_size ? (size_t) len : 0;
}

static void mark_status_due(int64_t now)
{
    portENTER_CRITICAL(&s_state_lock);
    if (now - s_last_status_us < SLATE_WS_STATUS_PERIOD_US) {
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }
    s_last_status_us = now;

    for (size_t i = 0; i < SLATE_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].used && s_clients[i].authenticated &&
            !s_clients[i].initial_status_pending) {
            s_clients[i].status_pending = true;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
}

/**
 * Make the next `status` frame due now rather than at the next 15 s heartbeat.
 *
 * Used where a provider's lifecycle changed, which is the one thing in that
 * frame a client is likely to be waiting on: §5.4's attach and detach move
 * `direct` between `degraded` and `online`, and a consumer that had to wait out
 * a heartbeat to learn whether it holds the attachment would be reading a
 * fifteen-second-old answer to a question it asked once.
 *
 * The heartbeat's own clock is deliberately left alone, so an attach storm
 * cannot silence the periodic frame that follows it.
 */
static void mark_status_immediately(void)
{
    portENTER_CRITICAL(&s_state_lock);
    for (size_t i = 0; i < SLATE_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].used && s_clients[i].authenticated &&
            !s_clients[i].initial_status_pending) {
            s_clients[i].status_pending = true;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static void expire_edit_mode(int64_t now)
{
    bool expired;
    portENTER_CRITICAL(&s_state_lock);
    expired = s_mode == SLATE_WS_MODE_EDIT &&
              now - s_edit_last_ping_us >= SLATE_WS_EDIT_TIMEOUT_US;
    portEXIT_CRITICAL(&s_state_lock);
    if (expired) {
        set_mode(SLATE_WS_MODE_NORMAL, NULL, now);
        ESP_LOGW(TAG, "edit mode expired after 60 seconds without a client ping");
    }
}

static void service_client(ws_client_t *client, int64_t now, char *line, char *json)
{
    uint32_t generation;
    int fd;
    bool authenticated;
    bool closing;
    bool pending;
    bool initial;
    bool status;
    bool reloaded;
    int64_t connected;
    uint64_t cursor;
    uint64_t backlog_end;
    unsigned schema;
    size_t tiles;

    portENTER_CRITICAL(&s_state_lock);
    if (!client->used) {
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }
    generation = client->generation;
    fd = client->fd;
    authenticated = client->authenticated;
    closing = client->closing;
    pending = client->send_pending;
    initial = client->initial_status_pending;
    status = client->status_pending;
    reloaded = client->reloaded_pending;
    connected = client->connected_at_us;
    cursor = client->log_cursor;
    backlog_end = client->backlog_end;
    schema = client->reloaded_schema;
    tiles = client->reloaded_tiles;
    portEXIT_CRITICAL(&s_state_lock);

    if (closing || pending) {
        return;
    }
    if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET ||
        client_for_session(s_server, fd) != client) {
        return;
    }
    if (!authenticated) {
        if (now - connected >= SLATE_WS_AUTH_TIMEOUT_US) {
            const uint8_t close_code[2] = {0x03, 0xF0};
            queue_send(client, generation, SEND_POLICY_CLOSE, 0,
                       close_code, sizeof(close_code));
        }
        return;
    }

    /* Ahead of the retained backlog and the heartbeat, because §5.3 gives an
     * action three seconds before the bus reverts it and a tap queued behind
     * 8 KiB of replayed log lines would spend them on the wrong thing. */
    size_t action_len = 0;
    ws_provider_t *provider = provider_peek(client, generation, json, &action_len);
    if (provider) {
        if (queue_send(client, generation, SEND_ACTION, 0, json, action_len) == ESP_OK) {
            provider_drop(provider);
        }
        return;
    }

    if (initial && cursor < backlog_end) {
        uint64_t seq;
        if (log_ring_copy(cursor, line, SLATE_WS_LOG_CAPTURE_BYTES, &seq)) {
            size_t len = format_log_json(line, json, SLATE_WS_JSON_BYTES);
            if (len > 0) {
                queue_send(client, generation, SEND_LOG, seq, json, len);
            }
            return;
        }
    }

    if (initial || status) {
        size_t len = format_status_json(json, SLATE_WS_JSON_BYTES);
        if (len > 0) {
            queue_send(client, generation, SEND_STATUS, 0, json, len);
        }
        return;
    }

    if (reloaded) {
        int len = snprintf(json, SLATE_WS_JSON_BYTES,
                           "{\"type\":\"reloaded\",\"schema\":%u,\"tiles\":%u}",
                           schema, (unsigned) tiles);
        if (len > 0 && len < SLATE_WS_JSON_BYTES) {
            queue_send(client, generation, SEND_RELOADED, 0, json, (size_t) len);
        }
        return;
    }

    uint64_t seq;
    if (log_ring_copy(cursor, line, SLATE_WS_LOG_CAPTURE_BYTES, &seq)) {
        size_t len = format_log_json(line, json, SLATE_WS_JSON_BYTES);
        if (len > 0) {
            queue_send(client, generation, SEND_LOG, seq, json, len);
        }
    }
}

static void ws_task(void *ctx)
{
    (void) ctx;
    char line[SLATE_WS_LOG_CAPTURE_BYTES];
    char json[SLATE_WS_JSON_BYTES];

    while (true) {
        int64_t now = esp_timer_get_time();
        mark_status_due(now);
        expire_edit_mode(now);
        for (size_t i = 0; i < SLATE_WS_MAX_CLIENTS; i++) {
            service_client(&s_clients[i], now, line, json);
        }
        vTaskDelay(pdMS_TO_TICKS(SLATE_WS_POLL_MS));
    }
}

esp_err_t slate_ws_capture_init(void)
{
    bool installed = false;

    /* Capture starts before the store is known to be healthy. Keep that step
     * independent of heap state: the ring, line buffer and mutex all live in
     * static storage, and transport/task allocation remains in slate_ws_init.
     * The separate lock makes the check-and-install one operation: installing
     * twice would make capture_vprintf its own previous handler. */
    portENTER_CRITICAL(&s_capture_init_lock);
    if (!s_capture_mutex) {
        s_capture_mutex = xSemaphoreCreateMutexStatic(&s_capture_mutex_storage);
        if (s_capture_mutex) {
            s_previous_vprintf = esp_log_set_vprintf(capture_vprintf);
            installed = true;
        }
    }
    bool ready = s_capture_mutex != NULL;
    portEXIT_CRITICAL(&s_capture_init_lock);

    if (!ready) {
        return ESP_ERR_NO_MEM;
    }

    if (installed) {
        ESP_LOGI(TAG, "retaining logs in %u B ring", SLATE_WS_LOG_RING_BYTES);
    }
    return ESP_OK;
}

esp_err_t slate_ws_init(void)
{
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = slate_ws_capture_init();
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < SLATE_WS_MAX_CLIENTS; i++) {
        s_clients[i].fd = -1;
    }
    s_last_status_us = esp_timer_get_time();

    if (xTaskCreate(ws_task, "slate_ws", SLATE_WS_TASK_STACK, NULL,
                    SLATE_WS_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    const httpd_uri_t ws = {
        .uri = SLATE_API_BASE_PATH "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .ws_post_handshake_cb = ws_connected,
    };
    err = slate_api_register_uri(&ws, SLATE_API_AUTH_WS_FIRST_FRAME);
    if (err != ESP_OK) {
        vTaskDelete(s_task);
        s_task = NULL;
        return err;
    }

    ESP_LOGI(TAG, "WebSocket channel ready");
    return ESP_OK;
}

slate_ws_mode_t slate_ws_mode(void)
{
    portENTER_CRITICAL(&s_state_lock);
    slate_ws_mode_t mode = s_mode;
    portEXIT_CRITICAL(&s_state_lock);
    return mode;
}

esp_err_t slate_ws_mode_observer_set(slate_ws_mode_observer_fn observer, void *ctx)
{
    if (!s_task) {
        return ESP_ERR_NOT_FOUND;
    }

    portENTER_CRITICAL(&s_state_lock);
    esp_err_t err = observer != NULL && s_mode_observer != NULL
                        ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) {
        s_mode_observer = observer;
        s_mode_observer_ctx = observer != NULL ? ctx : NULL;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return err;
}

esp_err_t slate_ws_publish_reloaded(unsigned schema, size_t tiles)
{
    if (!s_task) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_state_lock);
    for (size_t i = 0; i < SLATE_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].used && s_clients[i].authenticated) {
            s_clients[i].reloaded_schema = schema;
            s_clients[i].reloaded_tiles = tiles;
            s_clients[i].reloaded_pending = true;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

#ifdef SLATE_WS_SELFTEST

static volatile unsigned s_selftest_mode_changes;
static volatile slate_ws_mode_t s_selftest_last_mode;

static void selftest_mode_observer(void *ctx, slate_ws_mode_t mode)
{
    (void) ctx;
    s_selftest_last_mode = mode;
    s_selftest_mode_changes++;
}

static bool selftest_send_all(int fd, const void *data, size_t len)
{
    const uint8_t *cursor = data;
    while (len > 0) {
        int sent = send(fd, cursor, len, 0);
        if (sent <= 0) {
            return false;
        }
        cursor += sent;
        len -= (size_t) sent;
    }
    return true;
}

static bool selftest_recv_all(int fd, void *data, size_t len)
{
    uint8_t *cursor = data;
    while (len > 0) {
        int received = recv(fd, cursor, len, 0);
        if (received <= 0) {
            return false;
        }
        cursor += received;
        len -= (size_t) received;
    }
    return true;
}

static int selftest_connect(void)
{
    static const char UPGRADE[] =
        "GET /api/v1/ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return -1;
    }
    struct timeval timeout = {.tv_sec = 7};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (connect(fd, (const struct sockaddr *) &address, sizeof(address)) != 0 ||
        !selftest_send_all(fd, UPGRADE, sizeof(UPGRADE) - 1)) {
        close(fd);
        return -1;
    }

    char response[512];
    size_t used = 0;
    while (used + 1 < sizeof(response)) {
        int received = recv(fd, response + used, 1, 0);
        if (received != 1) {
            close(fd);
            return -1;
        }
        used++;
        response[used] = '\0';
        if (used >= 4 && memcmp(response + used - 4, "\r\n\r\n", 4) == 0) {
            break;
        }
    }
    bool upgraded = strstr(response, " 101 ") != NULL &&
                    strstr(response, "Upgrade: websocket") != NULL;
    if (!upgraded) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool selftest_send_text(int fd, const char *text)
{
    const size_t len = strlen(text);
    if (len > 125) {
        return false;
    }

    static const uint8_t MASK[4] = {0x13, 0x57, 0x9B, 0xDF};
    uint8_t frame[2 + sizeof(MASK) + 126];
    frame[0] = 0x81; /* FIN + text */
    frame[1] = 0x80 | (uint8_t) len;
    memcpy(frame + 2, MASK, sizeof(MASK));
    for (size_t i = 0; i < len; i++) {
        frame[6 + i] = (uint8_t) text[i] ^ MASK[i % sizeof(MASK)];
    }
    return selftest_send_all(fd, frame, 6 + len);
}

static int selftest_recv_frame(int fd, httpd_ws_type_t *type, uint8_t *payload,
                               size_t payload_size)
{
    uint8_t header[2];
    if (!selftest_recv_all(fd, header, sizeof(header))) {
        return -1;
    }
    *type = (httpd_ws_type_t) (header[0] & 0x0F);
    uint64_t len = header[1] & 0x7F;
    if (header[1] & 0x80) {
        return -1; /* Servers must not mask RFC 6455 frames. */
    }
    if (len == 126) {
        uint8_t extended[2];
        if (!selftest_recv_all(fd, extended, sizeof(extended))) {
            return -1;
        }
        len = ((uint16_t) extended[0] << 8) | extended[1];
    } else if (len == 127) {
        return -1; /* No Slate frame is large enough to need 64-bit length. */
    }
    if (len + 1 > payload_size || !selftest_recv_all(fd, payload, (size_t) len)) {
        return -1;
    }
    payload[len] = '\0';
    return (int) len;
}

static bool selftest_wait_for_text(int fd, const char *needle, bool *saw_marker,
                                   bool *saw_status)
{
    uint8_t payload[SLATE_WS_JSON_BYTES];
    for (unsigned i = 0; i < 96; i++) {
        httpd_ws_type_t type;
        int len = selftest_recv_frame(fd, &type, payload, sizeof(payload));
        if (len < 0) {
            return false;
        }
        if (type != HTTPD_WS_TYPE_TEXT) {
            continue;
        }
        if (saw_marker && strstr((char *) payload, "ws-selftest-marker")) {
            *saw_marker = true;
        }
        if (saw_status && strstr((char *) payload, "\"type\":\"status\"") &&
            strstr((char *) payload, "\"providers\":{\"direct\":\"") &&
            strstr((char *) payload, "\"ha\":") &&
            strstr((char *) payload, "\"lvgl_heap_free\":")) {
            *saw_status = true;
        }
        if (needle && strstr((char *) payload, needle)) {
            return true;
        }
        if (!needle && saw_marker && saw_status && *saw_marker && *saw_status) {
            return true;
        }
    }
    return false;
}

static bool selftest_authenticate(int fd)
{
    char token[SLATE_DEVICE_TOKEN_LEN + 1];
    char auth[96];
    bool copied = slate_store_device_token_copy(token, sizeof(token)) == ESP_OK;
    int len = copied ? snprintf(auth, sizeof(auth),
                                "{\"type\":\"auth\",\"token\":\"%s\"}", token)
                     : -1;
    explicit_bzero(token, sizeof(token));
    bool sent = len > 0 && (size_t) len < sizeof(auth) && selftest_send_text(fd, auth);
    explicit_bzero(auth, sizeof(auth));
    return sent && selftest_wait_for_text(fd, "\"type\":\"auth_ok\"", NULL, NULL);
}

esp_err_t slate_ws_selftest(void)
{
    int failures = 0;
#define SELFTEST_CHECK(condition, name)                                      \
    do {                                                                     \
        bool passed_ = (condition);                                          \
        failures += !passed_;                                                \
        ESP_LOGI(TAG, "selftest: %-34s %s", name, passed_ ? "PASS" : "FAIL"); \
    } while (0)

    s_selftest_mode_changes = 0;
    s_selftest_last_mode = SLATE_WS_MODE_NORMAL;
    SELFTEST_CHECK(slate_ws_mode_observer_set(selftest_mode_observer, NULL) == ESP_OK,
                   "mode observer registered");

    /* Fill past capacity first: the real protocol check below then proves that
     * replay begins on a complete retained record rather than in raw bytes. */
    static const char FIRST[] = "ws-selftest-first\n";
    static const char LAST[] = "ws-selftest-last\n";
    uint64_t ignored_oldest;
    uint64_t first_seq;
    log_ring_bounds(&ignored_oldest, &first_seq);
    log_ring_append(FIRST, sizeof(FIRST) - 1);
    for (unsigned i = 0; i < 40; i++) {
        char fill[256];
        memset(fill, 'a' + i % 26, sizeof(fill));
        fill[sizeof(fill) - 1] = '\n';
        log_ring_append(fill, sizeof(fill));
    }
    log_ring_append(LAST, sizeof(LAST) - 1);

    uint64_t oldest;
    uint64_t next;
    log_ring_bounds(&oldest, &next);
    char value[SLATE_WS_LOG_CAPTURE_BYTES];
    uint64_t seq;
    bool ok = oldest > first_seq && next > oldest &&
              log_ring_copy(next - 1, value, sizeof(value), &seq) &&
              seq == next - 1 && strcmp(value, LAST) == 0;
    SELFTEST_CHECK(ok, "retained ring evicts whole records");

    ESP_LOGW(TAG, "ws-selftest-marker");
    int fd = selftest_connect();
    SELFTEST_CHECK(fd >= 0, "HTTP upgrade");
    bool authenticated = fd >= 0 && selftest_authenticate(fd);
    SELFTEST_CHECK(authenticated, "valid first-frame authentication");

    bool saw_marker = false;
    bool saw_status = false;
    bool initial = authenticated &&
                   selftest_wait_for_text(fd, NULL, &saw_marker, &saw_status);
    SELFTEST_CHECK(initial && saw_marker, "retained logs replay after auth");
    SELFTEST_CHECK(initial && saw_status, "initial status follows backlog");

    mark_status_due(esp_timer_get_time() + SLATE_WS_STATUS_PERIOD_US);
    bool heartbeat = selftest_wait_for_text(fd, "\"type\":\"status\"", NULL, NULL);
    portENTER_CRITICAL(&s_state_lock);
    s_last_status_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_lock);
    SELFTEST_CHECK(heartbeat, "15 s status heartbeat delivered");

    bool edit_sent = authenticated &&
                     selftest_send_text(fd, "{\"type\":\"mode\",\"mode\":\"edit\"}");
    vTaskDelay(pdMS_TO_TICKS(100));
    SELFTEST_CHECK(edit_sent && slate_ws_mode() == SLATE_WS_MODE_EDIT &&
                       s_selftest_mode_changes == 1 &&
                       s_selftest_last_mode == SLATE_WS_MODE_EDIT,
                   "mode edit notifies observer");

    int other_fd = selftest_connect();
    bool other_authenticated = other_fd >= 0 && selftest_authenticate(other_fd);
    SELFTEST_CHECK(other_authenticated, "second client authentication");
    portENTER_CRITICAL(&s_state_lock);
    s_edit_last_ping_us = esp_timer_get_time() - SLATE_WS_EDIT_TIMEOUT_US + 2000000;
    int64_t before_other_ping = s_edit_last_ping_us;
    portEXIT_CRITICAL(&s_state_lock);
    bool other_ping = other_authenticated &&
                      selftest_send_text(other_fd, "{\"type\":\"ping\"}");
    vTaskDelay(pdMS_TO_TICKS(100));
    portENTER_CRITICAL(&s_state_lock);
    bool other_ignored = s_edit_last_ping_us == before_other_ping;
    portEXIT_CRITICAL(&s_state_lock);
    SELFTEST_CHECK(other_ping && other_ignored, "non-owner ping leaves edit deadline");
    if (other_fd >= 0) {
        close(other_fd);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    portENTER_CRITICAL(&s_state_lock);
    s_edit_last_ping_us = esp_timer_get_time() - SLATE_WS_EDIT_TIMEOUT_US + 1000000;
    int64_t before_ping = s_edit_last_ping_us;
    portEXIT_CRITICAL(&s_state_lock);
    bool ping_sent = selftest_send_text(fd, "{\"type\":\"ping\"}");
    vTaskDelay(pdMS_TO_TICKS(100));
    portENTER_CRITICAL(&s_state_lock);
    bool ping_refreshed = s_edit_last_ping_us > before_ping;
    portEXIT_CRITICAL(&s_state_lock);
    SELFTEST_CHECK(ping_sent && ping_refreshed, "application ping refreshes edit mode");

    SELFTEST_CHECK(slate_ws_publish_reloaded(1, 7) == ESP_OK &&
                       selftest_wait_for_text(fd, "\"type\":\"reloaded\"", NULL, NULL),
                   "reloaded frame delivered");

    portENTER_CRITICAL(&s_state_lock);
    s_edit_last_ping_us = esp_timer_get_time() - SLATE_WS_EDIT_TIMEOUT_US;
    portEXIT_CRITICAL(&s_state_lock);
    expire_edit_mode(esp_timer_get_time());
    SELFTEST_CHECK(slate_ws_mode() == SLATE_WS_MODE_NORMAL &&
                       s_selftest_mode_changes == 2 &&
                       s_selftest_last_mode == SLATE_WS_MODE_NORMAL,
                   "edit timeout notifies observer");

    bool edit_again = selftest_send_text(fd, "{\"type\":\"mode\",\"mode\":\"edit\"}");
    vTaskDelay(pdMS_TO_TICKS(100));
    bool edit_reacquired = slate_ws_mode() == SLATE_WS_MODE_EDIT;
    portENTER_CRITICAL(&s_state_lock);
    s_edit_owner_generation--;
    s_edit_last_ping_us = esp_timer_get_time() - SLATE_WS_EDIT_TIMEOUT_US;
    portEXIT_CRITICAL(&s_state_lock);
    bool reused_ping = selftest_send_text(fd, "{\"type\":\"ping\"}");
    vTaskDelay(pdMS_TO_TICKS(100));
    expire_edit_mode(esp_timer_get_time());
    SELFTEST_CHECK(edit_again && edit_reacquired && reused_ping &&
                       slate_ws_mode() == SLATE_WS_MODE_NORMAL &&
                       s_selftest_mode_changes == 4 &&
                       s_selftest_last_mode == SLATE_WS_MODE_NORMAL,
                   "reused fd fallback notifies observer");

    if (fd >= 0) {
        close(fd);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    fd = selftest_connect();
    SELFTEST_CHECK(fd >= 0 &&
                       selftest_send_text(fd, "{\"type\":\"auth\",\"token\":\"wrong\"}") &&
                       selftest_wait_for_text(fd, "\"type\":\"auth_invalid\"", NULL, NULL),
                   "wrong token rejected");
    if (fd >= 0) {
        httpd_ws_type_t type = HTTPD_WS_TYPE_CONTINUE;
        uint8_t close_payload[8];
        int close_len = selftest_recv_frame(fd, &type, close_payload, sizeof(close_payload));
        SELFTEST_CHECK(type == HTTPD_WS_TYPE_CLOSE && close_len == 2 &&
                           close_payload[0] == 0x03 && close_payload[1] == 0xF0,
                       "wrong token closes with 1008");
        close(fd);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    fd = selftest_connect();
    SELFTEST_CHECK(fd >= 0 && selftest_send_text(fd, "{not-json") &&
                       selftest_wait_for_text(fd, "\"type\":\"auth_invalid\"", NULL, NULL),
                   "malformed first frame rejected");
    if (fd >= 0) {
        httpd_ws_type_t type = HTTPD_WS_TYPE_CONTINUE;
        uint8_t close_payload[8];
        int close_len = selftest_recv_frame(fd, &type, close_payload, sizeof(close_payload));
        SELFTEST_CHECK(type == HTTPD_WS_TYPE_CLOSE && close_len == 2 &&
                           close_payload[0] == 0x03 && close_payload[1] == 0xF0,
                       "malformed frame closes with 1008");
        close(fd);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    fd = selftest_connect();
    SELFTEST_CHECK(fd >= 0, "silent client upgrade");
    if (fd >= 0) {
        httpd_ws_type_t type = HTTPD_WS_TYPE_CONTINUE;
        uint8_t close_payload[8];
        int close_len = selftest_recv_frame(fd, &type, close_payload, sizeof(close_payload));
        SELFTEST_CHECK(type == HTTPD_WS_TYPE_CLOSE && close_len == 2 &&
                           close_payload[0] == 0x03 && close_payload[1] == 0xF0,
                       "silent client closes with 1008");
        close(fd);
    }

    SELFTEST_CHECK(slate_ws_mode_observer_set(NULL, NULL) == ESP_OK,
                   "mode observer released");
    ESP_LOGI(TAG, "selftest: %d failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
#undef SELFTEST_CHECK
}

#endif
