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
#include "slate_store.h"
#include "slate_wifi.h"

#ifdef SLATE_WS_SELFTEST
#include "lwip/inet.h"
#include "lwip/sockets.h"
#endif

static const char *TAG = "ws";

#define SLATE_WS_MAX_CLIENTS       4
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
    int64_t last_ping_us;
    uint64_t log_cursor;
    uint64_t backlog_end;
    unsigned reloaded_schema;
    size_t reloaded_tiles;
} ws_client_t;

typedef enum {
    SEND_LOG,
    SEND_STATUS,
    SEND_RELOADED,
    SEND_POLICY_CLOSE,
} send_kind_t;

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
static int s_edit_owner_fd = -1;
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

static void client_session_freed(void *ctx)
{
    ws_client_t *client = ctx;
    if (!client) {
        return;
    }

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

    s_server = req->handle;
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

static void set_mode(slate_ws_mode_t mode, int owner_fd, int64_t now)
{
    bool changed;
    portENTER_CRITICAL(&s_state_lock);
    changed = s_mode != mode;
    s_mode = mode;
    if (mode == SLATE_WS_MODE_EDIT) {
        s_edit_owner_fd = owner_fd;
        s_edit_last_ping_us = now;
    } else {
        s_edit_owner_fd = -1;
        s_edit_last_ping_us = 0;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (changed) {
        ESP_LOGI(TAG, "mode changed to %s", mode == SLATE_WS_MODE_EDIT ? "edit" : "normal");
    }
}

static esp_err_t handle_authenticated(ws_client_t *client, int fd, cJSON *root)
{
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        return ESP_OK;
    }

    const int64_t now = esp_timer_get_time();
    if (strcmp(type->valuestring, "ping") == 0) {
        portENTER_CRITICAL(&s_state_lock);
        if (client->used && client->fd == fd && client->authenticated) {
            client->last_ping_us = now;
            if (s_mode == SLATE_WS_MODE_EDIT && s_edit_owner_fd == fd) {
                s_edit_last_ping_us = now;
            }
        }
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }

    if (strcmp(type->valuestring, "mode") == 0) {
        cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        if (cJSON_IsString(mode) && strcmp(mode->valuestring, "edit") == 0) {
            set_mode(SLATE_WS_MODE_EDIT, fd, now);
        } else if (cJSON_IsString(mode) && strcmp(mode->valuestring, "normal") == 0) {
            set_mode(SLATE_WS_MODE_NORMAL, -1, now);
        }
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
        client->last_ping_us = now;
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
        err = handle_authenticated(client, fd, root);
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

static void finish_send(send_work_t *work, esp_err_t result)
{
    bool close_session = result != ESP_OK || work->kind == SEND_POLICY_CLOSE;

    portENTER_CRITICAL(&s_state_lock);
    if (work->client->used && work->client->generation == work->generation) {
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

    if (client_is_current(work, auth_required)) {
        httpd_ws_frame_t frame = {
            .type = work->kind == SEND_POLICY_CLOSE ? HTTPD_WS_TYPE_CLOSE
                                                    : HTTPD_WS_TYPE_TEXT,
            .payload = work->payload,
            .len = work->len,
        };
        result = httpd_ws_send_frame_async(s_server, work->fd, &frame);
    }

    finish_send(work, result);
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

    int len = snprintf(out, out_size,
                       "{\"type\":\"status\",\"ha\":\"%s\",\"wifi\":%s,"
                       "\"heap_free\":%u,\"lvgl_heap_free\":%s,\"lvgl_frag_pct\":%s}",
                       slate_store_ha_token_is_set() ? "disconnected" : "unconfigured",
                       wifi_value, (unsigned) esp_get_free_heap_size(), lvgl_free, lvgl_frag);
    return len > 0 && (size_t) len < out_size ? (size_t) len : 0;
}

static void mark_status_due(int64_t now)
{
    if (now - s_last_status_us < SLATE_WS_STATUS_PERIOD_US) {
        return;
    }
    s_last_status_us = now;

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
        set_mode(SLATE_WS_MODE_NORMAL, -1, now);
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

esp_err_t slate_ws_init(void)
{
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < SLATE_WS_MAX_CLIENTS; i++) {
        s_clients[i].fd = -1;
    }
    s_last_status_us = esp_timer_get_time();
    s_capture_mutex = xSemaphoreCreateMutex();
    if (!s_capture_mutex) {
        return ESP_ERR_NO_MEM;
    }

    const httpd_uri_t ws = {
        .uri = SLATE_API_BASE_PATH "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .ws_post_handshake_cb = ws_connected,
    };
    esp_err_t err = slate_api_register_uri(&ws, SLATE_API_AUTH_WS_FIRST_FRAME);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_capture_mutex);
        s_capture_mutex = NULL;
        return err;
    }

    if (xTaskCreate(ws_task, "slate_ws", SLATE_WS_TASK_STACK, NULL,
                    SLATE_WS_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        vSemaphoreDelete(s_capture_mutex);
        s_capture_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_previous_vprintf = esp_log_set_vprintf(capture_vprintf);
    ESP_LOGI(TAG, "WebSocket channel ready with %u B retained log ring",
             SLATE_WS_LOG_RING_BYTES);
    return ESP_OK;
}

slate_ws_mode_t slate_ws_mode(void)
{
    portENTER_CRITICAL(&s_state_lock);
    slate_ws_mode_t mode = s_mode;
    portEXIT_CRITICAL(&s_state_lock);
    return mode;
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

    /* Fill past capacity first: the real protocol check below then proves that
     * replay begins on a complete retained record rather than in raw bytes. */
    static const char FIRST[] = "ws-selftest-first\n";
    static const char LAST[] = "ws-selftest-last\n";
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
    bool ok = next > oldest && log_ring_copy(next - 1, value, sizeof(value), &seq) &&
              seq == next - 1 && strcmp(value, LAST) == 0;
    SELFTEST_CHECK(ok, "retained ring keeps newest record");

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

    bool edit_sent = authenticated &&
                     selftest_send_text(fd, "{\"type\":\"mode\",\"mode\":\"edit\"}");
    vTaskDelay(pdMS_TO_TICKS(100));
    SELFTEST_CHECK(edit_sent && slate_ws_mode() == SLATE_WS_MODE_EDIT, "mode edit accepted");
    SELFTEST_CHECK(selftest_send_text(fd, "{\"type\":\"ping\"}"), "application ping accepted");

    SELFTEST_CHECK(slate_ws_publish_reloaded(1, 7) == ESP_OK &&
                       selftest_wait_for_text(fd, "\"type\":\"reloaded\"", NULL, NULL),
                   "reloaded frame delivered");

    portENTER_CRITICAL(&s_state_lock);
    s_edit_last_ping_us = esp_timer_get_time() - SLATE_WS_EDIT_TIMEOUT_US;
    portEXIT_CRITICAL(&s_state_lock);
    expire_edit_mode(esp_timer_get_time());
    SELFTEST_CHECK(slate_ws_mode() == SLATE_WS_MODE_NORMAL, "edit mode 60 s fallback");

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

    ESP_LOGI(TAG, "selftest: %d failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
#undef SELFTEST_CHECK
}

#endif
