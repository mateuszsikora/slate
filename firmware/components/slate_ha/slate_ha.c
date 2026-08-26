/*
 * Slate — Home Assistant discovery, authentication and reconnect lifecycle.
 *
 * Home Assistant transport and payload types terminate here. Authentication,
 * reconnect and subscribe_entities feed the provider-neutral state store;
 * semantic actions travel the other direction through call_service.
 */

#include "slate_ha.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_parser.h"
#include "mdns.h"

#include "slate_action.h"
#include "slate_api.h"
#include "slate_ha_actions.h"
#include "slate_ha_entities.h"
#include "slate_mdns.h"
#include "slate_state.h"
#include "slate_store.h"
#include "slate_wifi.h"

#define BODY_MAX                 (SLATE_HA_TOKEN_MAX_LEN + SLATE_HA_URL_MAX_LEN + 96)
#define WS_URI_MAX               (SLATE_HA_URL_MAX_LEN + sizeof("/api/websocket"))
#define AUTH_FRAME_MAX           (SLATE_HA_TOKEN_MAX_LEN + 64)
#define AUTH_REPLY_MAX           1024
#define MAIN_MESSAGE_MAX         (512 * 1024)
#define MANAGER_QUEUE_DEPTH      8
#define MANAGER_TASK_STACK       6144
#define MANAGER_TASK_PRIORITY    5
#define CONFIG_QUEUE_DEPTH       2
#define CONFIG_TASK_STACK        6144
#define CONFIG_TASK_PRIORITY     5
#define DISCOVERY_MAX_RESULTS    8
#define DISCOVERY_TIMEOUT_MS     3000
#define CONNECT_TEST_TIMEOUT_MS  12000
#define CATALOG_BODY_MAX         64
#define CATALOG_QUERY_MAX        48
#define CATALOG_TIMEOUT_US       (30LL * 1000 * 1000)

static const char *TAG = "slate_ha";

static const uint32_t BACKOFF_MS[] = {1000, 2000, 4000, 8000, 15000, 30000};

typedef enum {
    CMD_WIFI_UP,
    CMD_WIFI_DOWN,
    CMD_RELOAD,
    CMD_AUTH_OK,
    CMD_AUTH_INVALID,
    CMD_PROTOCOL_ERROR,
    CMD_RESUBSCRIBE,
    CMD_ACTION,
    CMD_ACTION_RESULT,
    CMD_CLEAR_ACTIONS,
    CMD_CATALOG_REQUEST,
} manager_command_kind_t;

typedef enum {
    HA_CATALOG_ENTITIES = 0,
    HA_CATALOG_DEVICES,
    HA_CATALOG_AREAS,
    HA_CATALOG_STATES,
    HA_CATALOG_STAGE_COUNT,
} ha_catalog_stage_t;

typedef struct {
    manager_command_kind_t kind;
    uint32_t generation;
    TaskHandle_t waiter;
    union {
        slate_ha_action_request_t action;
        struct {
            uint32_t command_id;
            bool success;
            char error[SLATE_HA_ACTION_ERROR_MAX + 1];
        } action_result;
        struct {
            uint32_t request_id;
            ha_catalog_stage_t stage;
        } catalog;
    } data;
} manager_command_t;

typedef struct {
    char name[MDNS_NAME_BUF_LEN];
    char uuid[33];
    char url[SLATE_HA_URL_MAX_LEN];
} discovered_instance_t;

typedef struct {
    char data[AUTH_REPLY_MAX];
    size_t used;
} message_buffer_t;

typedef struct {
    char *data;
    size_t used;
    size_t capacity;
    size_t frame_used;
    size_t frame_length;
    bool active;
} payload_buffer_t;

typedef struct main_connection main_connection_t;

typedef struct {
    main_connection_t *connection;
    uint32_t generation;
    unsigned before_connect_count;
    bool redirected;
    payload_buffer_t message;
} main_event_context_t;

struct main_connection {
    esp_websocket_client_handle_t client;
    main_event_context_t *event_context;
    char url[SLATE_HA_URL_MAX_LEN];
    char uri[WS_URI_MAX];
    char token[SLATE_HA_TOKEN_MAX_LEN];
    uint32_t generation;
    atomic_bool enabled;
    atomic_bool auth_rejected;
    atomic_bool authenticated;
    atomic_size_t backoff_index;
    atomic_uint_fast32_t subscription_id;
    uint32_t next_command_id;
};

typedef enum {
    CATALOG_IDLE,
    CATALOG_PENDING,
    CATALOG_READY,
    CATALOG_FAILED,
} catalog_status_t;

typedef struct {
    SemaphoreHandle_t lock;
    catalog_status_t status;
    uint32_t next_request_id;
    uint32_t request_id;
    uint32_t command_id;
    uint32_t stale_command_id;
    int64_t started_at_us;
    char *response;
    size_t response_len;
    char error[32];
} catalog_relay_t;

typedef struct {
    EventGroupHandle_t events;
    const char *token;
    unsigned before_connect_count;
    bool redirected;
    message_buffer_t message;
} connection_test_t;

typedef struct {
    httpd_req_t *request;
    char url[SLATE_HA_URL_MAX_LEN];
    char uri[WS_URI_MAX];
    char token[SLATE_HA_TOKEN_MAX_LEN];
} configure_job_t;

static QueueHandle_t s_commands;
static QueueHandle_t s_config_jobs;
static TaskHandle_t s_manager_task;
static TaskHandle_t s_config_task;
static main_connection_t s_main;
static catalog_relay_t s_catalog;
static bool s_wifi_up;
static bool s_auth_blocked;
static atomic_bool s_resubscribe_queued;
static bool s_initialized;
static bool s_started;

#define TEST_AUTH_OK      BIT0
#define TEST_AUTH_INVALID BIT1
#define TEST_TRANSPORT    BIT2
#define TEST_PROTOCOL     BIT3

/* --- Small pure helpers ------------------------------------------------ */

static uint32_t backoff_at(size_t index)
{
    size_t last = sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0]) - 1;
    return BACKOFF_MS[index > last ? last : index];
}

static size_t backoff_next(size_t index)
{
    size_t last = sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0]) - 1;
    return index < last ? index + 1 : last;
}

/**
 * Turn the base URL the editor displays into HA's WebSocket endpoint.
 *
 * The same parser the WebSocket client uses validates the authority here, so
 * malformed ports and hosts are `bad_url` rather than later masquerading as
 * `ha_unreachable`. Redirects are detected separately before an auth frame is
 * sent. Query strings, fragments and userinfo are not valid base URLs.
 */
static bool websocket_uri(const char *url, char *out, size_t out_len)
{
    if (url == NULL || out == NULL || out_len == 0) {
        return false;
    }

    struct http_parser_url parsed;
    http_parser_url_init(&parsed);
    if (http_parser_parse_url(url, strlen(url), false, &parsed) != 0 ||
        !(parsed.field_set & (1U << UF_SCHEMA)) ||
        !(parsed.field_set & (1U << UF_HOST)) ||
        parsed.field_data[UF_HOST].len == 0 ||
        (parsed.field_set & ((1U << UF_USERINFO) | (1U << UF_QUERY) |
                             (1U << UF_FRAGMENT))) != 0 ||
        ((parsed.field_set & (1U << UF_PORT)) != 0 && parsed.port == 0)) {
        return false;
    }

    const char *scheme_at = url + parsed.field_data[UF_SCHEMA].off;
    size_t scheme_len = parsed.field_data[UF_SCHEMA].len;
    const char *scheme = NULL;
    if (scheme_len == 4 && strncasecmp(scheme_at, "http", scheme_len) == 0) {
        scheme = "ws://";
    } else if (scheme_len == 5 && strncasecmp(scheme_at, "https", scheme_len) == 0) {
        scheme = "wss://";
    } else {
        return false;
    }

    const char *rest = scheme_at + scheme_len;
    if (strncmp(rest, "://", 3) != 0) {
        return false;
    }
    rest += 3;
    for (const unsigned char *at = (const unsigned char *) rest; *at != '\0'; at++) {
        if (*at <= 0x20 || *at == 0x7f) {
            return false;
        }
    }

    size_t rest_len = strlen(rest);
    while (rest_len > 0 && rest[rest_len - 1] == '/') {
        rest_len--;
    }
    if (rest_len == 0) {
        return false;
    }

    int written = snprintf(out, out_len, "%s%.*s/api/websocket", scheme,
                           (int) rest_len, rest);
    return written > 0 && (size_t) written < out_len;
}

/** Build and wipe the only frame that ever contains the HA credential. */
static bool send_auth(esp_websocket_client_handle_t client, const char *token)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *secret = NULL;
    char *frame = NULL;
    bool ok = root != NULL &&
              cJSON_AddStringToObject(root, "type", "auth") != NULL &&
              (secret = cJSON_AddStringToObject(root, "access_token", token)) != NULL &&
              (frame = cJSON_PrintUnformatted(root)) != NULL;

    if (ok) {
        size_t len = strlen(frame);
        ok = len < AUTH_FRAME_MAX &&
             esp_websocket_client_send_text(client, frame, (int) len,
                                            pdMS_TO_TICKS(2000)) == (int) len;
    }

    if (secret != NULL && secret->valuestring != NULL) {
        explicit_bzero(secret->valuestring, strlen(secret->valuestring));
    }
    if (frame != NULL) {
        explicit_bzero(frame, strlen(frame));
        cJSON_free(frame);
    }
    cJSON_Delete(root);
    return ok;
}

/**
 * Reassemble one small authentication message. #75 owns the separate, PSRAM-
 * backed assembler for entity payloads measured in hundreds of kilobytes.
 */
static bool message_append(message_buffer_t *message,
                           const esp_websocket_event_data_t *event, bool *complete)
{
    *complete = false;
    if (event->op_code != 0x1 && !(event->op_code == 0x0 && event->payload_offset > 0)) {
        return false;
    }
    if (event->payload_len <= 0 || (size_t) event->payload_len >= sizeof(message->data) ||
        event->payload_offset < 0 || event->data_len < 0 ||
        (size_t) event->payload_offset != message->used ||
        message->used + (size_t) event->data_len >= sizeof(message->data)) {
        message->used = 0;
        return false;
    }

    memcpy(message->data + message->used, event->data_ptr, (size_t) event->data_len);
    message->used += (size_t) event->data_len;
    if (event->fin && message->used == (size_t) event->payload_len) {
        message->data[message->used] = '\0';
        *complete = true;
    }
    return true;
}

static bool message_type(message_buffer_t *message, char *out, size_t out_len)
{
    cJSON *root = cJSON_ParseWithLength(message->data, message->used);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *value = cJSON_IsString(type) ? type->valuestring : NULL;
    out[0] = '\0';
    if (value != NULL) {
        strlcpy(out, value, out_len);
    }
    cJSON_Delete(root);
    message->used = 0;
    return out[0] != '\0';
}

static void payload_reset(payload_buffer_t *message)
{
    free(message->data);
    *message = (payload_buffer_t) {0};
}

static bool payload_reserve(payload_buffer_t *message, size_t required)
{
    if (required <= message->capacity) {
        return true;
    }
    size_t capacity = message->capacity > 0 ? message->capacity : 1024;
    while (capacity < required) {
        capacity = capacity > MAIN_MESSAGE_MAX / 2 ? MAIN_MESSAGE_MAX : capacity * 2;
        if (capacity < required && capacity == MAIN_MESSAGE_MAX) {
            return false;
        }
    }

    char *fresh = heap_caps_malloc_prefer(capacity + 1, 2,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (fresh == NULL) {
        return false;
    }
    if (message->used > 0) {
        memcpy(fresh, message->data, message->used);
    }
    free(message->data);
    message->data = fresh;
    message->capacity = capacity;
    return true;
}

/** Reassemble a bounded HA message across TCP chunks and WebSocket fragments. */
static bool payload_append(payload_buffer_t *message,
                           const esp_websocket_event_data_t *event, bool *complete)
{
    *complete = false;
    if (event->payload_len <= 0 || (size_t) event->payload_len > MAIN_MESSAGE_MAX ||
        event->payload_offset < 0 || event->data_len <= 0 || event->data_ptr == NULL ||
        (size_t) event->payload_offset + (size_t) event->data_len >
            (size_t) event->payload_len) {
        payload_reset(message);
        return false;
    }

    if (!message->active) {
        if (event->op_code != 0x1 || event->payload_offset != 0) {
            payload_reset(message);
            return false;
        }
        message->active = true;
        message->frame_length = (size_t) event->payload_len;
    } else if (event->payload_offset == 0) {
        if (message->frame_used != message->frame_length || event->op_code != 0x0) {
            payload_reset(message);
            return false;
        }
        message->frame_used = 0;
        message->frame_length = (size_t) event->payload_len;
    } else if ((size_t) event->payload_offset != message->frame_used ||
               (size_t) event->payload_len != message->frame_length ||
               (event->op_code != 0x1 && event->op_code != 0x0)) {
        payload_reset(message);
        return false;
    }

    size_t chunk = (size_t) event->data_len;
    if (message->used > MAIN_MESSAGE_MAX - chunk ||
        !payload_reserve(message, message->used + chunk)) {
        payload_reset(message);
        return false;
    }
    memcpy(message->data + message->used, event->data_ptr, (size_t) event->data_len);
    message->used += chunk;
    message->frame_used += chunk;
    if (event->fin && message->frame_used == message->frame_length) {
        message->data[message->used] = '\0';
        *complete = true;
    }
    return true;
}

/* --- mDNS discovery ---------------------------------------------------- */

static bool txt_value(const mdns_result_t *result, const char *key,
                      char *out, size_t out_len)
{
    for (size_t i = 0; i < result->txt_count; i++) {
        if (result->txt[i].key == NULL || strcmp(result->txt[i].key, key) != 0 ||
            result->txt[i].value == NULL) {
            continue;
        }
        size_t len = result->txt_value_len != NULL ? result->txt_value_len[i]
                                                   : strlen(result->txt[i].value);
        if (len == 0 || len >= out_len) {
            return false;
        }
        memcpy(out, result->txt[i].value, len);
        out[len] = '\0';
        return true;
    }
    return false;
}

static bool instance_duplicate(const discovered_instance_t *instances, size_t count,
                               const discovered_instance_t *candidate)
{
    for (size_t i = 0; i < count; i++) {
        if ((candidate->uuid[0] != '\0' && strcmp(instances[i].uuid, candidate->uuid) == 0) ||
            strcmp(instances[i].url, candidate->url) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t discover_instances(discovered_instance_t *instances, size_t capacity,
                                    size_t *out_count)
{
    *out_count = 0;
    if (!slate_mdns_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_home-assistant", "_tcp", DISCOVERY_TIMEOUT_MS,
                                   DISCOVERY_MAX_RESULTS, &results);
    if (err != ESP_OK) {
        return err;
    }

    for (const mdns_result_t *result = results;
         result != NULL && *out_count < capacity; result = result->next) {
        discovered_instance_t candidate = {0};

        /* `internal_url` carries the scheme. Guessing HTTP from port 8123 would
         * turn a TLS installation into a false discovery result; an empty TXT
         * value therefore stays a manual-URL case. */
        if (!txt_value(result, "internal_url", candidate.url, sizeof(candidate.url)) ||
            !websocket_uri(candidate.url, (char[WS_URI_MAX]) {0}, WS_URI_MAX)) {
            continue;
        }

        if (!txt_value(result, "location_name", candidate.name, sizeof(candidate.name)) &&
            result->instance_name != NULL) {
            strlcpy(candidate.name, result->instance_name, sizeof(candidate.name));
        }
        /* A hostname is useful for transport but is not HA's stable UUID. If a
         * non-conforming advertisement omits the field, expose it as empty
         * rather than giving clients a false identity that may later change. */
        txt_value(result, "uuid", candidate.uuid, sizeof(candidate.uuid));
        if (candidate.name[0] == '\0') {
            strlcpy(candidate.name, "Home Assistant", sizeof(candidate.name));
        }

        if (!instance_duplicate(instances, *out_count, &candidate)) {
            instances[(*out_count)++] = candidate;
        }
    }

    mdns_query_results_free(results);
    return ESP_OK;
}

/* --- One-shot credential test ----------------------------------------- */

static void test_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) base;
    connection_test_t *test = arg;
    esp_websocket_event_data_t *event = data;

    if (id == WEBSOCKET_EVENT_BEFORE_CONNECT) {
        test->redirected = ++test->before_connect_count > 1;
        return;
    }
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        if (test->redirected) {
            /* esp_websocket_client follows HTTP redirects internally. Refuse
             * before a redirected peer can ask for the credential. */
            xEventGroupSetBits(test->events, TEST_PROTOCOL);
        }
        return;
    }
    if (id == WEBSOCKET_EVENT_ERROR || id == WEBSOCKET_EVENT_DISCONNECTED ||
        id == WEBSOCKET_EVENT_CLOSED) {
        test->before_connect_count = 0;
        test->redirected = false;
        EventBits_t done = xEventGroupGetBits(test->events);
        if ((done & (TEST_AUTH_OK | TEST_AUTH_INVALID | TEST_PROTOCOL)) == 0) {
            xEventGroupSetBits(test->events, TEST_TRANSPORT);
        }
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA) {
        return;
    }
    if (test->redirected) {
        xEventGroupSetBits(test->events, TEST_PROTOCOL);
        return;
    }

    bool complete = false;
    if (!message_append(&test->message, event, &complete)) {
        xEventGroupSetBits(test->events, TEST_PROTOCOL);
        return;
    }
    if (!complete) {
        return;
    }

    char type[24];
    bool typed = message_type(&test->message, type, sizeof(type));
    if (typed && strcmp(type, "auth_required") == 0) {
        if (!send_auth(event->client, test->token)) {
            xEventGroupSetBits(test->events, TEST_TRANSPORT);
        }
    } else if (typed && strcmp(type, "auth_ok") == 0) {
        xEventGroupSetBits(test->events, TEST_AUTH_OK);
    } else if (typed && strcmp(type, "auth_invalid") == 0) {
        xEventGroupSetBits(test->events, TEST_AUTH_INVALID);
    }
}

typedef enum {
    TEST_RESULT_OK,
    TEST_RESULT_AUTH_INVALID,
    TEST_RESULT_UNREACHABLE,
} test_result_t;

static test_result_t test_credentials(const char *uri, const char *token)
{
    connection_test_t *test = calloc(1, sizeof(*test));
    if (test == NULL) {
        return TEST_RESULT_UNREACHABLE;
    }
    test->events = xEventGroupCreate();
    test->token = token;
    if (test->events == NULL) {
        free(test);
        return TEST_RESULT_UNREACHABLE;
    }

    const esp_websocket_client_config_t config = {
        .uri = uri,
        .disable_auto_reconnect = true,
        .task_name = "ha_test_ws",
        .task_stack = 5120,
        .buffer_size = AUTH_REPLY_MAX,
        .network_timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    if (client == NULL ||
        esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, test_event, test) != ESP_OK ||
        esp_websocket_client_start(client) != ESP_OK) {
        if (client != NULL) {
            esp_websocket_client_destroy(client);
        }
        vEventGroupDelete(test->events);
        free(test);
        return TEST_RESULT_UNREACHABLE;
    }

    EventBits_t result = xEventGroupWaitBits(
        test->events, TEST_AUTH_OK | TEST_AUTH_INVALID | TEST_TRANSPORT | TEST_PROTOCOL,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_TEST_TIMEOUT_MS));

    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    vEventGroupDelete(test->events);
    free(test);

    if ((result & TEST_AUTH_OK) != 0) {
        return TEST_RESULT_OK;
    }
    if ((result & TEST_AUTH_INVALID) != 0) {
        return TEST_RESULT_AUTH_INVALID;
    }
    return TEST_RESULT_UNREACHABLE;
}

/* --- Long-lived provider connection ----------------------------------- */

static bool command_send(manager_command_kind_t kind, uint32_t generation,
                         TaskHandle_t waiter, TickType_t timeout)
{
    const manager_command_t command = {
        .kind = kind,
        .generation = generation,
        .waiter = waiter,
    };
    if (s_commands == NULL || xQueueSend(s_commands, &command, timeout) != pdTRUE) {
        ESP_LOGW(TAG, "manager queue full; dropped command %d", kind);
        return false;
    }
    return true;
}

static void command(manager_command_kind_t kind)
{
    command_send(kind, 0, NULL, 0);
}

/** Copy a borrowed bus request into the manager's single transport queue. */
static esp_err_t provider_action_dispatch(void *ctx, uint32_t id,
                                          const slate_action_request_t *request)
{
    (void) ctx;
    if (!atomic_load(&s_main.authenticated)) {
        return ESP_ERR_INVALID_STATE;
    }

    manager_command_t queued = {
        .kind = CMD_ACTION,
    };
    esp_err_t err = slate_ha_action_request_copy(&queued.data.action, id, request);
    if (err != ESP_OK) {
        return err;
    }
    return xQueueSend(s_commands, &queued, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static uint32_t next_command_id(void)
{
    s_main.next_command_id++;
    if (s_main.next_command_id == 0 || s_main.next_command_id > INT32_MAX) {
        s_main.next_command_id = 1;
    }
    return s_main.next_command_id;
}

static bool command_id(const cJSON *item, uint32_t *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 1 || item->valuedouble > UINT32_MAX) {
        return false;
    }
    uint32_t value = (uint32_t) item->valuedouble;
    if ((double) value != item->valuedouble) {
        return false;
    }
    *out = value;
    return true;
}

/** Read only a top-level numeric `id`; the potentially huge `result` is opaque. */
static bool raw_command_id(const char *json, size_t len, uint32_t *out)
{
    size_t i = 0;
    unsigned depth = 0;
    while (i < len) {
        unsigned char ch = (unsigned char) json[i];
        if (ch == '{' || ch == '[') {
            depth++;
            i++;
            continue;
        }
        if (ch == '}' || ch == ']') {
            if (depth == 0) {
                return false;
            }
            depth--;
            i++;
            continue;
        }
        if (ch != '"') {
            i++;
            continue;
        }

        size_t start = ++i;
        bool escaped = false;
        while (i < len) {
            ch = (unsigned char) json[i];
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                break;
            }
            i++;
        }
        if (i >= len) {
            return false;
        }
        size_t string_len = i - start;
        i++;

        if (depth != 1 || string_len != 2 ||
            json[start] != 'i' || json[start + 1] != 'd') {
            continue;
        }
        while (i < len && (json[i] == ' ' || json[i] == '\t' ||
                           json[i] == '\r' || json[i] == '\n')) {
            i++;
        }
        if (i >= len || json[i++] != ':') {
            continue;
        }
        while (i < len && (json[i] == ' ' || json[i] == '\t' ||
                           json[i] == '\r' || json[i] == '\n')) {
            i++;
        }
        if (i >= len || json[i] < '0' || json[i] > '9') {
            return false;
        }
        uint64_t value = 0;
        while (i < len && json[i] >= '0' && json[i] <= '9') {
            value = value * 10 + (uint64_t) (json[i++] - '0');
            if (value > UINT32_MAX) {
                return false;
            }
        }
        while (i < len && (json[i] == ' ' || json[i] == '\t' ||
                           json[i] == '\r' || json[i] == '\n')) {
            i++;
        }
        if (value == 0 || i >= len || (json[i] != ',' && json[i] != '}')) {
            return false;
        }
        *out = (uint32_t) value;
        return true;
    }
    return false;
}

static esp_err_t send_json_frame(cJSON *root)
{
    char *frame = cJSON_PrintUnformatted(root);
    if (frame == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = strlen(frame);
    int sent = esp_websocket_client_send_text(s_main.client, frame, (int) len,
                                               pdMS_TO_TICKS(3000));
    cJSON_free(frame);
    return sent == (int) len ? ESP_OK : ESP_FAIL;
}

static const char *catalog_command(ha_catalog_stage_t stage)
{
    static const char *const COMMANDS[HA_CATALOG_STAGE_COUNT] = {
        [HA_CATALOG_ENTITIES] = "config/entity_registry/list_for_display",
        [HA_CATALOG_DEVICES] = "config/device_registry/list",
        [HA_CATALOG_AREAS] = "config/area_registry/list",
        [HA_CATALOG_STATES] = "get_states",
    };
    return stage < HA_CATALOG_STAGE_COUNT ? COMMANDS[stage] : NULL;
}

static bool catalog_stage(const char *name, ha_catalog_stage_t *out)
{
    static const char *const NAMES[HA_CATALOG_STAGE_COUNT] = {
        [HA_CATALOG_ENTITIES] = "entities",
        [HA_CATALOG_DEVICES] = "devices",
        [HA_CATALOG_AREAS] = "areas",
        [HA_CATALOG_STATES] = "states",
    };
    for (ha_catalog_stage_t stage = 0; stage < HA_CATALOG_STAGE_COUNT; stage++) {
        if (strcmp(name, NAMES[stage]) == 0) {
            *out = stage;
            return true;
        }
    }
    return false;
}

static void catalog_reset_locked(void)
{
    if (s_catalog.status == CATALOG_PENDING && s_catalog.command_id != 0) {
        s_catalog.stale_command_id = s_catalog.command_id;
    }
    free(s_catalog.response);
    s_catalog.response = NULL;
    s_catalog.response_len = 0;
    s_catalog.status = CATALOG_IDLE;
    s_catalog.request_id = 0;
    s_catalog.command_id = 0;
    s_catalog.started_at_us = 0;
    s_catalog.error[0] = '\0';
}

static void catalog_fail(uint32_t request_id, const char *error)
{
    xSemaphoreTake(s_catalog.lock, portMAX_DELAY);
    if (s_catalog.status == CATALOG_PENDING &&
        (request_id == 0 || s_catalog.request_id == request_id)) {
        s_catalog.status = CATALOG_FAILED;
        s_catalog.stale_command_id = s_catalog.command_id;
        s_catalog.command_id = 0;
        strlcpy(s_catalog.error, error, sizeof(s_catalog.error));
    }
    xSemaphoreGive(s_catalog.lock);
}

static bool catalog_set_command(uint32_t request_id, uint32_t command_id)
{
    bool current = false;
    xSemaphoreTake(s_catalog.lock, portMAX_DELAY);
    if (s_catalog.status == CATALOG_PENDING &&
        s_catalog.request_id == request_id && s_catalog.command_id == 0) {
        s_catalog.command_id = command_id;
        current = true;
    }
    xSemaphoreGive(s_catalog.lock);
    return current;
}

/** Take ownership of a complete raw HA response without constructing a tree. */
static bool catalog_accept(uint32_t command_id, char *response, size_t response_len)
{
    bool intercepted = false;
    xSemaphoreTake(s_catalog.lock, portMAX_DELAY);
    if (s_catalog.status == CATALOG_PENDING &&
        s_catalog.command_id == command_id) {
        s_catalog.response = response;
        s_catalog.response_len = response_len;
        s_catalog.command_id = 0;
        s_catalog.status = CATALOG_READY;
        intercepted = true;
    } else if (s_catalog.stale_command_id == command_id) {
        /* A timed-out/disconnected relay response must not fall through to the
         * ordinary cJSON parser merely because its browser request has gone. */
        s_catalog.stale_command_id = 0;
        free(response);
        intercepted = true;
    }
    xSemaphoreGive(s_catalog.lock);
    return intercepted;
}

static void connection_send_catalog(uint32_t request_id, ha_catalog_stage_t stage)
{
    if (s_main.client == NULL || !atomic_load(&s_main.authenticated)) {
        catalog_fail(request_id, "ha_unavailable");
        return;
    }

    uint32_t command_id = next_command_id();
    if (!catalog_set_command(request_id, command_id)) {
        return;
    }

    cJSON *frame = cJSON_CreateObject();
    bool ok = frame != NULL &&
              cJSON_AddNumberToObject(frame, "id", command_id) != NULL &&
              cJSON_AddStringToObject(frame, "type", catalog_command(stage)) != NULL;
    esp_err_t err = ok ? send_json_frame(frame) : ESP_ERR_NO_MEM;
    cJSON_Delete(frame);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not relay Home Assistant catalog stage %u: %s",
                 (unsigned) stage, esp_err_to_name(err));
        catalog_fail(request_id, "ha_unavailable");
    }
}

static void fail_bus_action(uint32_t bus_id, const char *error)
{
    slate_action_result(SLATE_HA_PROVIDER_ID, bus_id, false, error);
}

/** Allocate an HA command id, remember the bus id, then put the frame on wire. */
static void connection_send_action(const slate_ha_action_request_t *request)
{
    if (s_main.client == NULL || !atomic_load(&s_main.authenticated)) {
        fail_bus_action(request->bus_id, "transport_unavailable");
        return;
    }

    uint32_t command_id = next_command_id();
    cJSON *frame = NULL;
    esp_err_t err = slate_ha_action_frame(command_id, request, &frame);
    if (err != ESP_OK) {
        fail_bus_action(request->bus_id, esp_err_to_name(err));
        return;
    }

    err = slate_ha_action_track(s_main.generation, command_id, request->bus_id);
    if (err == ESP_OK) {
        err = send_json_frame(frame);
    }
    cJSON_Delete(frame);

    if (err != ESP_OK) {
        uint32_t ignored = 0;
        slate_ha_action_take(s_main.generation, command_id, &ignored);
        fail_bus_action(request->bus_id,
                        err == ESP_ERR_NO_MEM ? "too_many_actions"
                                              : "transport_unavailable");
        ESP_LOGW(TAG, "could not send HA action %" PRIu32 ": %s",
                 request->bus_id, esp_err_to_name(err));
    }
}

static void complete_bus_action(const manager_command_t *command)
{
    uint32_t bus_id = 0;
    if (!slate_ha_action_take(command->generation,
                              command->data.action_result.command_id, &bus_id)) {
        ESP_LOGD(TAG, "ignored unknown or late HA result for command %" PRIu32,
                 command->data.action_result.command_id);
        return;
    }

    slate_action_result(SLATE_HA_PROVIDER_ID, bus_id,
                        command->data.action_result.success,
                        command->data.action_result.error[0] != '\0'
                            ? command->data.action_result.error
                            : NULL);
}

static void clear_bus_actions(const char *error)
{
    slate_ha_action_clear();
    slate_action_provider_unavailable(SLATE_HA_PROVIDER_ID, error);
}

static esp_err_t send_unsubscribe(uint32_t subscription)
{
    cJSON *root = cJSON_CreateObject();
    bool ok = root != NULL &&
              cJSON_AddNumberToObject(root, "id", next_command_id()) != NULL &&
              cJSON_AddStringToObject(root, "type", "unsubscribe_events") != NULL &&
              cJSON_AddNumberToObject(root, "subscription", subscription) != NULL;
    esp_err_t err = ok ? send_json_frame(root) : ESP_ERR_NO_MEM;
    cJSON_Delete(root);
    return err;
}

/** Replace HA's subscription with the state store's latest exact id set. */
static esp_err_t connection_resubscribe(void)
{
    if (s_main.client == NULL || !atomic_load(&s_main.authenticated)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t previous = (uint32_t) atomic_exchange(&s_main.subscription_id, 0);
    if (previous != 0) {
        esp_err_t err = send_unsubscribe(previous);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "could not retire HA subscription %u: %s",
                     (unsigned) previous, esp_err_to_name(err));
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *ids = root != NULL ? cJSON_AddArrayToObject(root, "entity_ids") : NULL;
    uint32_t subscription = next_command_id();
    size_t count = 0;
    bool ok = ids != NULL &&
              cJSON_AddNumberToObject(root, "id", subscription) != NULL &&
              cJSON_AddStringToObject(root, "type", "subscribe_entities") != NULL &&
              slate_ha_entities_append_ids(ids, &count) == ESP_OK;
    if (!ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (count == 0) {
        /* HA interprets a missing or empty entity_ids filter as every entity. */
        cJSON_Delete(root);
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ONLINE);
        ESP_LOGI(TAG, "Home Assistant authenticated with no bound entities");
        return ESP_OK;
    }

    esp_err_t prepare_err = slate_ha_entities_prepare_subscription();
    if (prepare_err != ESP_OK) {
        cJSON_Delete(root);
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
        return prepare_err;
    }
    atomic_store(&s_main.subscription_id, subscription);
    esp_err_t err = send_json_frame(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        atomic_store(&s_main.subscription_id, 0);
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_OFFLINE);
        return err;
    }

    slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ONLINE);
    ESP_LOGI(TAG, "subscribed to %u Home Assistant entit%s as command %u",
             (unsigned) count, count == 1 ? "y" : "ies", (unsigned) subscription);
    return ESP_OK;
}

static bool subscription_is_current(size_t count, uint32_t subscription,
                                    slate_provider_status_t status)
{
    return status == SLATE_PROVIDER_ONLINE &&
           ((count == 0 && subscription == 0) ||
            (count > 0 && subscription != 0));
}

static esp_err_t provider_subscribe(void *ctx, const char *const *resources, size_t count)
{
    (void) ctx;
    bool changed = false;
    esp_err_t err = slate_ha_entities_bind(resources, count, &changed);
    if (err != ESP_OK) {
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
        return err;
    }
    uint32_t subscription = atomic_load(&s_main.subscription_id);
    bool subscription_current = subscription_is_current(
        count, subscription, slate_state_provider_status(SLATE_HA_PROVIDER_ID));
    if (!changed &&
        (!atomic_load(&s_main.authenticated) || subscription_current)) {
        return ESP_OK;
    }

    bool expected = false;
    if (atomic_compare_exchange_strong(&s_resubscribe_queued, &expected, true) &&
        !command_send(CMD_RESUBSCRIBE, 0, NULL, 0)) {
        atomic_store(&s_resubscribe_queued, false);
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void main_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) base;
    main_event_context_t *context = arg;
    main_connection_t *connection = context->connection;
    esp_websocket_event_data_t *event = data;
    if (!atomic_load(&connection->enabled)) {
        return;
    }

    if (id == WEBSOCKET_EVENT_BEFORE_CONNECT) {
        context->redirected = ++context->before_connect_count > 1;
        if (!atomic_load(&connection->auth_rejected)) {
            slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_CONNECTING);
        }
        return;
    }
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        if (context->redirected) {
            ESP_LOGE(TAG, "refusing redirected Home Assistant WebSocket endpoint");
            slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
            command_send(CMD_PROTOCOL_ERROR, context->generation, NULL, 0);
        }
        return;
    }

    if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED ||
        id == WEBSOCKET_EVENT_ERROR) {
        context->before_connect_count = 0;
        context->redirected = false;
        payload_reset(&context->message);
        bool was_authenticated = atomic_exchange(&connection->authenticated, false);
        atomic_store(&connection->subscription_id, 0);
        if (was_authenticated) {
            /* ERROR is sometimes followed by DISCONNECTED, and sometimes is
             * the only event visible before the socket stalls. Close action
             * delivery on the first transport failure exactly once. */
            slate_action_provider_unavailable(SLATE_HA_PROVIDER_ID,
                                              "transport_disconnected");
            command_send(CMD_CLEAR_ACTIONS, context->generation, NULL, 0);
        }
        if (!atomic_load(&connection->auth_rejected)) {
            slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_OFFLINE);
            /* A transport failure emits ERROR then DISCONNECTED; a clean
             * server shutdown emits CLOSED instead. Advance on either terminal
             * event, never on ERROR, so one outage consumes exactly one step. */
            if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED) {
                size_t index = atomic_load(&connection->backoff_index);
                esp_websocket_client_set_reconnect_timeout(event->client,
                                                           (int) backoff_at(index));
                atomic_store(&connection->backoff_index, backoff_next(index));
            }
        }
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA) {
        return;
    }
    if (context->redirected) {
        return;
    }
    /* esp_websocket_client dispatches control frames before handling them.
     * PING/PONG/CLOSE are transport traffic, never Home Assistant JSON. */
    if (event->op_code == 0x8 || event->op_code == 0x9 || event->op_code == 0xA) {
        return;
    }

    bool complete = false;
    if (!payload_append(&context->message, event, &complete)) {
        ESP_LOGW(TAG, "discarding malformed or oversized Home Assistant frame");
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
        return;
    }
    if (!complete) {
        return;
    }

    uint32_t raw_id = 0;
    if (raw_command_id(context->message.data, context->message.used, &raw_id) &&
        catalog_accept(raw_id, context->message.data, context->message.used)) {
        /* The relay consumed the buffer (normally by taking ownership; a late
         * response may instead be freed). The next HA frame gets a fresh one. */
        context->message = (payload_buffer_t) {0};
        return;
    }

    cJSON *root = cJSON_ParseWithLength(context->message.data, context->message.used);
    payload_reset(&context->message);
    if (!cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "could not parse Home Assistant JSON payload");
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
        cJSON_Delete(root);
        return;
    }
    const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type = cJSON_IsString(type_item) ? type_item->valuestring : NULL;
    if (type == NULL) {
        ESP_LOGE(TAG, "Home Assistant JSON payload has no message type");
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type, "auth_required") == 0) {
        if (atomic_load(&connection->auth_rejected)) {
            ESP_LOGW(TAG, "refusing to resend a rejected Home Assistant token");
        } else if (!send_auth(event->client, connection->token)) {
            ESP_LOGW(TAG, "could not send Home Assistant authentication frame");
        }
    } else if (strcmp(type, "auth_ok") == 0) {
        atomic_store(&connection->backoff_index, 0);
        esp_websocket_client_set_reconnect_timeout(event->client, (int) BACKOFF_MS[0]);
        if (!command_send(CMD_AUTH_OK, context->generation, NULL, 0)) {
            /* Authentication already succeeded on the wire.  Retaining that
             * fact lets a later provider rebuild retry the subscription if
             * the manager queue was temporarily full. */
            atomic_store(&connection->authenticated, true);
            slate_state_provider_set_status(SLATE_HA_PROVIDER_ID,
                                            SLATE_PROVIDER_ERROR);
        }
    } else if (strcmp(type, "auth_invalid") == 0) {
        atomic_store(&connection->auth_rejected, true);
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
        ESP_LOGE(TAG, "Home Assistant rejected the stored token; waiting for new credentials");
        command_send(CMD_AUTH_INVALID, context->generation, NULL, 0);
    } else if (strcmp(type, "result") == 0) {
        const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
        const cJSON *success = cJSON_GetObjectItemCaseSensitive(root, "success");
        uint32_t id_value = 0;
        bool has_id = command_id(id_item, &id_value);
        if (has_id && id_value == atomic_load(&connection->subscription_id)) {
            if (!cJSON_IsTrue(success)) {
                const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
                const cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
                ESP_LOGE(TAG, "Home Assistant refused entity subscription %u: %s",
                         (unsigned) id_value,
                         cJSON_IsString(code) ? code->valuestring : "unknown_error");
                atomic_store(&connection->subscription_id, 0);
                slate_state_provider_set_status(SLATE_HA_PROVIDER_ID,
                                                SLATE_PROVIDER_ERROR);
            }
        } else if (has_id) {
            manager_command_t result = {
                .kind = CMD_ACTION_RESULT,
                .generation = context->generation,
            };
            result.data.action_result.command_id = id_value;
            esp_err_t fields = slate_ha_action_result_fields(
                root, &result.data.action_result.success,
                result.data.action_result.error,
                sizeof(result.data.action_result.error));
            if (fields != ESP_OK) {
                result.data.action_result.success = false;
                strlcpy(result.data.action_result.error, "invalid_result",
                        sizeof(result.data.action_result.error));
            }
            if (xQueueSend(s_commands, &result, 0) != pdTRUE) {
                ESP_LOGW(TAG, "manager queue full; action result %u will time out",
                         (unsigned) id_value);
            }
        }
    } else if (strcmp(type, "event") == 0) {
        const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
        uint32_t id_value = 0;
        const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "event");
        if (command_id(id_item, &id_value) &&
            id_value == atomic_load(&connection->subscription_id)) {
            esp_err_t err = slate_ha_entities_process_event(payload);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "discarding malformed HA entity diff: %s",
                         esp_err_to_name(err));
                /* A compressed diff cannot be skipped without making the
                 * adapter's complete snapshots suspect until reconnect. */
                slate_state_provider_set_status(SLATE_HA_PROVIDER_ID,
                                                SLATE_PROVIDER_ERROR);
            }
        }
    }
    cJSON_Delete(root);
}

static void connection_destroy(void)
{
    atomic_store(&s_main.enabled, false);
    atomic_store(&s_main.authenticated, false);
    atomic_store(&s_main.subscription_id, 0);
    catalog_fail(0, "ha_unavailable");
    if (s_main.client == NULL) {
        if (s_main.event_context != NULL) {
            payload_reset(&s_main.event_context->message);
        }
        free(s_main.event_context);
        s_main.event_context = NULL;
        return;
    }
    esp_websocket_client_stop(s_main.client);
    esp_websocket_client_destroy(s_main.client);
    s_main.client = NULL;
    payload_reset(&s_main.event_context->message);
    free(s_main.event_context);
    s_main.event_context = NULL;
}

static esp_err_t connection_start(void)
{
    if (!s_wifi_up) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!slate_store_ha_token_is_set()) {
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_UNCONFIGURED);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_auth_blocked) {
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
        return ESP_ERR_INVALID_STATE;
    }

    explicit_bzero(s_main.token, sizeof(s_main.token));
    if (slate_store_ha_url_get(s_main.url, sizeof(s_main.url)) != ESP_OK ||
        slate_store_ha_token_get(s_main.token, sizeof(s_main.token)) != ESP_OK ||
        !websocket_uri(s_main.url, s_main.uri, sizeof(s_main.uri))) {
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
        explicit_bzero(s_main.token, sizeof(s_main.token));
        return ESP_ERR_INVALID_ARG;
    }

    main_event_context_t *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_OFFLINE);
        explicit_bzero(s_main.token, sizeof(s_main.token));
        return ESP_ERR_NO_MEM;
    }
    if (++s_main.generation == 0) {
        s_main.generation = 1;
    }
    context->connection = &s_main;
    context->generation = s_main.generation;
    s_main.event_context = context;

    const esp_websocket_client_config_t config = {
        .uri = s_main.uri,
        .disable_auto_reconnect = false,
        .enable_close_reconnect = true,
        .task_name = "slate_ha_ws",
        .task_stack = 6144,
        .buffer_size = AUTH_REPLY_MAX,
        .pingpong_timeout_sec = 30,
        .reconnect_timeout_ms = (int) BACKOFF_MS[0],
        .network_timeout_ms = 10000,
        .ping_interval_sec = 15,
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_context = context,
    };

    atomic_store(&s_main.auth_rejected, false);
    atomic_store(&s_main.authenticated, false);
    atomic_store(&s_main.subscription_id, 0);
    atomic_store(&s_main.backoff_index, 0);
    s_main.client = esp_websocket_client_init(&config);
    if (s_main.client == NULL) {
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_OFFLINE);
        free(s_main.event_context);
        s_main.event_context = NULL;
        explicit_bzero(s_main.token, sizeof(s_main.token));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_websocket_register_events(s_main.client, WEBSOCKET_EVENT_ANY,
                                                   main_event, context);
    if (err == ESP_OK) {
        atomic_store(&s_main.enabled, true);
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_CONNECTING);
        err = esp_websocket_client_start(s_main.client);
    }
    if (err != ESP_OK) {
        connection_destroy();
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_OFFLINE);
    }
    return err;
}

static void manager_task(void *arg)
{
    (void) arg;
    manager_command_t received;

    for (;;) {
        if (xQueueReceive(s_commands, &received, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (received.kind) {
        case CMD_WIFI_UP:
            s_wifi_up = true;
            if (s_main.client == NULL) {
                if (!s_auth_blocked) {
                    connection_start();
                }
            } else {
                atomic_store(&s_main.backoff_index, 0);
                esp_websocket_client_set_reconnect_timeout(s_main.client,
                                                           (int) BACKOFF_MS[0]);
            }
            break;
        case CMD_WIFI_DOWN:
            s_wifi_up = false;
            atomic_store(&s_main.authenticated, false);
            catalog_fail(0, "ha_unavailable");
            clear_bus_actions("wifi_disconnected");
            if (slate_store_ha_token_is_set() &&
                !atomic_load(&s_main.auth_rejected)) {
                slate_state_provider_set_status(SLATE_HA_PROVIDER_ID,
                                                SLATE_PROVIDER_OFFLINE);
            }
            break;
        case CMD_RELOAD:
            clear_bus_actions("credentials_reloaded");
            connection_destroy();
            explicit_bzero(s_main.token, sizeof(s_main.token));
            s_auth_blocked = false;
            if (s_wifi_up) {
                connection_start();
            } else {
                slate_state_provider_set_status(SLATE_HA_PROVIDER_ID,
                    slate_store_ha_token_is_set() ? SLATE_PROVIDER_OFFLINE
                                                  : SLATE_PROVIDER_UNCONFIGURED);
            }
            if (received.waiter != NULL) {
                xTaskNotifyGive(received.waiter);
            }
            break;
        case CMD_AUTH_OK:
            if (received.generation != s_main.generation || s_main.client == NULL) {
                break;
            }
            atomic_store(&s_main.authenticated, true);
            if (connection_resubscribe() != ESP_OK) {
                ESP_LOGW(TAG, "could not establish Home Assistant entity subscription");
            }
            break;
        case CMD_AUTH_INVALID:
            if (received.generation != s_main.generation) {
                ESP_LOGD(TAG, "ignoring stale auth failure from generation %u",
                         (unsigned) received.generation);
                break;
            }
            s_auth_blocked = true;
            clear_bus_actions("authentication_rejected");
            connection_destroy();
            explicit_bzero(s_main.token, sizeof(s_main.token));
            /* ERROR is sticky until CMD_RELOAD supplies tested credentials. */
            slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
            break;
        case CMD_PROTOCOL_ERROR:
            if (received.generation != s_main.generation) {
                break;
            }
            s_auth_blocked = true;
            clear_bus_actions("protocol_error");
            connection_destroy();
            explicit_bzero(s_main.token, sizeof(s_main.token));
            slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
            break;
        case CMD_RESUBSCRIBE:
            /* Clear before reading the cache. A rebuild racing after this line
             * queues one more command; a rebuild just before it is already in
             * the cache this command reads. */
            atomic_store(&s_resubscribe_queued, false);
            if (s_main.client != NULL && atomic_load(&s_main.authenticated) &&
                connection_resubscribe() != ESP_OK) {
                ESP_LOGW(TAG, "configuration-driven HA resubscription failed");
            }
            break;
        case CMD_ACTION:
            connection_send_action(&received.data.action);
            break;
        case CMD_ACTION_RESULT:
            complete_bus_action(&received);
            break;
        case CMD_CLEAR_ACTIONS:
            if (received.generation == s_main.generation) {
                slate_ha_action_clear();
                catalog_fail(0, "ha_unavailable");
            }
            break;
        case CMD_CATALOG_REQUEST:
            connection_send_catalog(received.data.catalog.request_id,
                                    received.data.catalog.stage);
            break;
        }
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) data;

    if (id == SLATE_WIFI_EVENT_CONNECTED) {
        command(CMD_WIFI_UP);
    } else if (id == SLATE_WIFI_EVENT_DISCONNECTED) {
        command(CMD_WIFI_DOWN);
    }
}

/* --- Device API -------------------------------------------------------- */

static const char *read_body(httpd_req_t *req, char *buffer, size_t size)
{
    if (req->content_len == 0) {
        return "empty_body";
    }
    if (req->content_len >= size) {
        return "too_large";
    }

    size_t received = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (chunk <= 0) {
            return "truncated";
        }
        received += (size_t) chunk;
    }
    buffer[received] = '\0';
    return NULL;
}

static void clear_json_strings(cJSON *item)
{
    for (cJSON *current = item; current != NULL; current = current->next) {
        clear_json_strings(current->child);
        if (current->valuestring != NULL) {
            explicit_bzero(current->valuestring, strlen(current->valuestring));
        }
    }
}

static esp_err_t configure_handler(httpd_req_t *req)
{
    char body[BODY_MAX];
    const char *problem = read_body(req, body, sizeof(body));
    if (problem != NULL) {
        explicit_bzero(body, sizeof(body));
        return slate_api_refuse(req,
                                strcmp(problem, "too_large") == 0 ? "413 Payload Too Large"
                                                                  : "400 Bad Request",
                                problem);
    }

    cJSON *root = cJSON_Parse(body);
    explicit_bzero(body, sizeof(body));
    if (!cJSON_IsObject(root)) {
        clear_json_strings(root);
        cJSON_Delete(root);
        return slate_api_refuse(req, "400 Bad Request", "invalid_json");
    }

    const cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *token_item = cJSON_GetObjectItemCaseSensitive(root, "token");
    const char *url = cJSON_IsString(url_item) ? url_item->valuestring : NULL;
    const char *token = cJSON_IsString(token_item) ? token_item->valuestring : NULL;
    const char *error = NULL;
    char uri[WS_URI_MAX];

    if (url == NULL || url[0] == '\0') {
        error = "url_required";
    } else if (token == NULL || token[0] == '\0') {
        error = "token_required";
    } else if (strlen(url) >= SLATE_HA_URL_MAX_LEN) {
        error = "url_too_long";
    } else if (strlen(token) >= SLATE_HA_TOKEN_MAX_LEN) {
        error = "token_too_long";
    } else if (!websocket_uri(url, uri, sizeof(uri))) {
        error = "bad_url";
    }

    configure_job_t *job = NULL;
    if (error == NULL) {
        job = calloc(1, sizeof(*job));
        if (job != NULL) {
            strlcpy(job->url, url, sizeof(job->url));
            strlcpy(job->uri, uri, sizeof(job->uri));
            strlcpy(job->token, token, sizeof(job->token));
        }
    }

    clear_json_strings(root);
    cJSON_Delete(root);

    if (error != NULL) {
        return slate_api_refuse(req, "400 Bad Request", error);
    }
    if (job == NULL) {
        return slate_api_send_json(req, NULL);
    }

    esp_err_t err = httpd_req_async_handler_begin(req, &job->request);
    if (err != ESP_OK) {
        explicit_bzero(job->token, sizeof(job->token));
        free(job);
        return slate_api_send_json(req, NULL);
    }

    if (xQueueSend(s_config_jobs, &job, 0) != pdTRUE) {
        slate_api_refuse(job->request, "503 Service Unavailable", "ha_busy");
        httpd_req_async_handler_complete(job->request);
        explicit_bzero(job->token, sizeof(job->token));
        free(job);
    }
    return ESP_OK;
}

static void configure_task(void *arg)
{
    (void) arg;
    configure_job_t *job;

    for (;;) {
        if (xQueueReceive(s_config_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        test_result_t tested = test_credentials(job->uri, job->token);
        esp_err_t stored = ESP_OK;
        if (tested == TEST_RESULT_OK) {
            stored = slate_store_ha_set(job->url, job->token);
        }

        if (tested == TEST_RESULT_AUTH_INVALID) {
            slate_api_refuse(job->request, "422 Unprocessable Content", "ha_auth_invalid");
        } else if (tested != TEST_RESULT_OK) {
            slate_api_refuse(job->request, "502 Bad Gateway", "ha_unreachable");
        } else if (stored != ESP_OK) {
            slate_api_refuse(job->request, "500 Internal Server Error", "store_failed");
        } else {
            /* The dedicated worker is the only sender that waits for reload,
             * so its task notification cannot collide with another protocol. */
            ulTaskNotifyTake(pdTRUE, 0);
            if (!command_send(CMD_RELOAD, 0, xTaskGetCurrentTaskHandle(),
                              portMAX_DELAY)) {
                slate_api_refuse(job->request, "500 Internal Server Error", "reload_failed");
            } else {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                httpd_resp_set_status(job->request, "204 No Content");
                httpd_resp_send(job->request, NULL, 0);
            }
        }

        httpd_req_async_handler_complete(job->request);
        explicit_bzero(job->token, sizeof(job->token));
        free(job);
    }
}

static esp_err_t discovery_handler(httpd_req_t *req)
{
    discovered_instance_t *instances = calloc(DISCOVERY_MAX_RESULTS, sizeof(*instances));
    if (instances == NULL) {
        return slate_api_send_json(req, NULL);
    }
    size_t count = 0;
    esp_err_t err = discover_instances(instances, DISCOVERY_MAX_RESULTS, &count);
    if (err != ESP_OK) {
        free(instances);
        return slate_api_refuse(req, "503 Service Unavailable", "discovery_unavailable");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = root != NULL ? cJSON_AddArrayToObject(root, "instances") : NULL;
    bool ok = array != NULL;
    for (size_t i = 0; ok && i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        ok = item != NULL &&
             cJSON_AddStringToObject(item, "name", instances[i].name) != NULL &&
             cJSON_AddStringToObject(item, "uuid", instances[i].uuid) != NULL &&
             cJSON_AddStringToObject(item, "url", instances[i].url) != NULL &&
             cJSON_AddItemToArray(array, item);
        if (!ok) {
            cJSON_Delete(item);
        }
    }
    if (!ok) {
        cJSON_Delete(root);
        free(instances);
        return slate_api_send_json(req, NULL);
    }
    free(instances);
    return slate_api_send_json(req, root);
}

static esp_err_t configuration_handler(httpd_req_t *req)
{
    char url[SLATE_HA_URL_MAX_LEN] = {0};
    bool configured = slate_store_ha_token_is_set();
    esp_err_t url_err = configured
                            ? slate_store_ha_url_get(url, sizeof(url))
                            : ESP_ERR_NOT_FOUND;

    cJSON *root = cJSON_CreateObject();
    bool ok = root != NULL &&
              cJSON_AddBoolToObject(root, "configured", configured) != NULL;
    if (ok) {
        ok = url_err == ESP_OK
                 ? cJSON_AddStringToObject(root, "url", url) != NULL
                 : cJSON_AddNullToObject(root, "url") != NULL;
    }
    if (!ok) {
        cJSON_Delete(root);
        return slate_api_send_json(req, NULL);
    }
    return slate_api_send_json(req, root);
}

static esp_err_t disconnect_handler(httpd_req_t *req)
{
    esp_err_t err = slate_store_ha_clear();
    if (err != ESP_OK) {
        return slate_api_refuse(req, "500 Internal Server Error", "store_failed");
    }
    if (!command_send(CMD_RELOAD, 0, NULL, pdMS_TO_TICKS(100))) {
        return slate_api_refuse(req, "503 Service Unavailable", "ha_busy");
    }

    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static bool catalog_expired_locked(void)
{
    return s_catalog.status != CATALOG_IDLE && s_catalog.started_at_us > 0 &&
           esp_timer_get_time() - s_catalog.started_at_us >= CATALOG_TIMEOUT_US;
}

static esp_err_t catalog_start_handler(httpd_req_t *req)
{
    char body[CATALOG_BODY_MAX] = {0};
    const char *problem = read_body(req, body, sizeof(body));
    if (problem != NULL) {
        return slate_api_refuse(req,
                                strcmp(problem, "too_large") == 0
                                    ? "413 Payload Too Large"
                                    : "400 Bad Request",
                                problem);
    }

    cJSON *root = cJSON_Parse(body);
    const cJSON *stage_item = cJSON_GetObjectItemCaseSensitive(root, "stage");
    ha_catalog_stage_t stage = HA_CATALOG_ENTITIES;
    bool valid = cJSON_IsObject(root) && cJSON_IsString(stage_item) &&
                 catalog_stage(stage_item->valuestring, &stage);
    cJSON_Delete(root);
    if (!valid) {
        return slate_api_refuse(req, "400 Bad Request", "catalog_stage_invalid");
    }
    if (!atomic_load(&s_main.authenticated)) {
        return slate_api_refuse(req, "409 Conflict", "provider_unavailable");
    }

    uint32_t request_id = 0;
    xSemaphoreTake(s_catalog.lock, portMAX_DELAY);
    if (catalog_expired_locked()) {
        catalog_reset_locked();
    }
    if (s_catalog.status == CATALOG_IDLE) {
        if (++s_catalog.next_request_id == 0) {
            s_catalog.next_request_id = 1;
        }
        request_id = s_catalog.next_request_id;
        s_catalog.request_id = request_id;
        s_catalog.started_at_us = esp_timer_get_time();
        s_catalog.status = CATALOG_PENDING;
    }
    xSemaphoreGive(s_catalog.lock);
    if (request_id == 0) {
        return slate_api_refuse(req, "409 Conflict", "catalog_busy");
    }

    manager_command_t command = {
        .kind = CMD_CATALOG_REQUEST,
    };
    command.data.catalog.request_id = request_id;
    command.data.catalog.stage = stage;
    if (xQueueSend(s_commands, &command, 0) != pdTRUE) {
        xSemaphoreTake(s_catalog.lock, portMAX_DELAY);
        if (s_catalog.request_id == request_id) {
            catalog_reset_locked();
        }
        xSemaphoreGive(s_catalog.lock);
        return slate_api_refuse(req, "503 Service Unavailable", "ha_busy");
    }

    root = cJSON_CreateObject();
    bool ok = root != NULL &&
              cJSON_AddNumberToObject(root, "request", request_id) != NULL;
    if (!ok) {
        cJSON_Delete(root);
        catalog_fail(request_id, "out_of_memory");
        return slate_api_send_json(req, NULL);
    }
    httpd_resp_set_status(req, "202 Accepted");
    return slate_api_send_json(req, root);
}

static bool query_request_id(httpd_req_t *req, uint32_t *out)
{
    char query[CATALOG_QUERY_MAX] = {0};
    char value[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "request", value, sizeof(value)) != ESP_OK) {
        return false;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (value[0] == '\0' || end == NULL || *end != '\0' || parsed == 0 ||
        parsed > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t) parsed;
    return true;
}

static esp_err_t catalog_result_handler(httpd_req_t *req)
{
    uint32_t request_id = 0;
    if (!query_request_id(req, &request_id)) {
        return slate_api_refuse(req, "400 Bad Request", "catalog_request_required");
    }

    char *response = NULL;
    size_t response_len = 0;
    char error[sizeof(s_catalog.error)] = {0};
    catalog_status_t status = CATALOG_IDLE;

    xSemaphoreTake(s_catalog.lock, portMAX_DELAY);
    if (s_catalog.request_id == request_id) {
        if (catalog_expired_locked() && s_catalog.status == CATALOG_PENDING) {
            s_catalog.status = CATALOG_FAILED;
            s_catalog.stale_command_id = s_catalog.command_id;
            s_catalog.command_id = 0;
            strlcpy(s_catalog.error, "catalog_timeout", sizeof(s_catalog.error));
        }
        status = s_catalog.status;
        if (status == CATALOG_READY) {
            response = s_catalog.response;
            response_len = s_catalog.response_len;
            s_catalog.response = NULL;
            catalog_reset_locked();
        } else if (status == CATALOG_FAILED) {
            strlcpy(error, s_catalog.error, sizeof(error));
            catalog_reset_locked();
        }
    }
    xSemaphoreGive(s_catalog.lock);

    if (status == CATALOG_IDLE) {
        return slate_api_refuse(req, "404 Not Found", "catalog_request_not_found");
    }
    if (status == CATALOG_PENDING) {
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"status\":\"pending\"}");
    }
    if (status == CATALOG_FAILED) {
        return slate_api_refuse(req, "502 Bad Gateway",
                                error[0] != '\0' ? error : "ha_unavailable");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, response, response_len);
    free(response);
    return err;
}

/* --- Public lifecycle -------------------------------------------------- */

esp_err_t slate_ha_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t entities_err = slate_ha_entities_init();
    if (entities_err != ESP_OK) {
        return entities_err;
    }
    s_catalog.lock = xSemaphoreCreateMutex();
    if (s_catalog.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t actions_err = slate_ha_action_tracker_init();
    if (actions_err != ESP_OK) {
        return actions_err;
    }

    s_commands = xQueueCreate(MANAGER_QUEUE_DEPTH, sizeof(manager_command_t));
    if (s_commands == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_config_jobs = xQueueCreate(CONFIG_QUEUE_DEPTH, sizeof(configure_job_t *));
    if (s_config_jobs == NULL) {
        vQueueDelete(s_commands);
        s_commands = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(manager_task, "slate_ha", MANAGER_TASK_STACK, NULL,
                    MANAGER_TASK_PRIORITY, &s_manager_task) != pdPASS) {
        vQueueDelete(s_config_jobs);
        s_config_jobs = NULL;
        vQueueDelete(s_commands);
        s_commands = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(configure_task, "slate_ha_cfg", CONFIG_TASK_STACK, NULL,
                    CONFIG_TASK_PRIORITY, &s_config_task) != pdPASS) {
        vTaskDelete(s_manager_task);
        s_manager_task = NULL;
        vQueueDelete(s_config_jobs);
        s_config_jobs = NULL;
        vQueueDelete(s_commands);
        s_commands = NULL;
        return ESP_ERR_NO_MEM;
    }

    const slate_state_provider_t provider = {
        .id = SLATE_HA_PROVIDER_ID,
        .subscribe = provider_subscribe,
    };
    esp_err_t provider_err = slate_state_provider_register(&provider);
    if (provider_err == ESP_OK) {
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID,
            slate_store_ha_token_is_set() ? SLATE_PROVIDER_OFFLINE
                                          : SLATE_PROVIDER_UNCONFIGURED);
    }

    const slate_action_provider_t action_provider = {
        .id = SLATE_HA_PROVIDER_ID,
        .dispatch = provider_action_dispatch,
    };
    esp_err_t action_provider_err = slate_action_provider_register(&action_provider);
    if (action_provider_err != ESP_OK) {
        ESP_LOGE(TAG, "semantic action dispatch unavailable: %s",
                 esp_err_to_name(action_provider_err));
    }

    const httpd_uri_t configure = {
        .uri = SLATE_API_BASE_PATH "/ha",
        .method = HTTP_POST,
        .handler = configure_handler,
    };
    const httpd_uri_t configuration = {
        .uri = SLATE_API_BASE_PATH "/ha",
        .method = HTTP_GET,
        .handler = configuration_handler,
    };
    const httpd_uri_t disconnect = {
        .uri = SLATE_API_BASE_PATH "/ha",
        .method = HTTP_DELETE,
        .handler = disconnect_handler,
    };
    const httpd_uri_t discover = {
        .uri = SLATE_API_BASE_PATH "/ha/discover",
        .method = HTTP_GET,
        .handler = discovery_handler,
    };
    const httpd_uri_t catalog_start = {
        .uri = SLATE_API_BASE_PATH "/ha/catalog",
        .method = HTTP_POST,
        .handler = catalog_start_handler,
    };
    const httpd_uri_t catalog_result = {
        .uri = SLATE_API_BASE_PATH "/ha/catalog",
        .method = HTTP_GET,
        .handler = catalog_result_handler,
    };
    esp_err_t route_err = slate_api_register_uri(&configure, SLATE_API_AUTH_DEVICE_TOKEN);
    if (route_err == ESP_OK) {
        route_err = slate_api_register_uri(&configuration, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (route_err == ESP_OK) {
        route_err = slate_api_register_uri(&disconnect, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (route_err == ESP_OK) {
        route_err = slate_api_register_uri(&discover, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (route_err == ESP_OK) {
        route_err = slate_api_register_uri(&catalog_start,
                                           SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (route_err == ESP_OK) {
        route_err = slate_api_register_uri(&catalog_result,
                                           SLATE_API_AUTH_DEVICE_TOKEN);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "provider ready: POST " SLATE_API_BASE_PATH "/ha");
    if (provider_err != ESP_OK) {
        return provider_err;
    }
    return action_provider_err != ESP_OK ? action_provider_err : route_err;
}

esp_err_t slate_ha_start(void)
{
    if (!s_initialized || s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The responder belongs to slate_mdns (§4.3): the panel's own name is not
     * this adapter's to claim, and starting it here made discovery depend on a
     * component that has nothing to do with it. */
    if (!slate_mdns_ready()) {
        ESP_LOGW(TAG, "Home Assistant discovery unavailable: the mDNS responder is not running");
    }

    esp_err_t event_err = esp_event_handler_instance_register(
        SLATE_WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    if (event_err != ESP_OK) {
        return event_err;
    }

    s_started = true;
    slate_wifi_status_t wifi;
    slate_wifi_status(&wifi);
    command(wifi.connected ? CMD_WIFI_UP : CMD_WIFI_DOWN);
    return ESP_OK;
}

#ifdef SLATE_HA_SELFTEST

esp_err_t slate_ha_selftest(void)
{
    unsigned failures = 0;
#define CHECK(condition, name) do {                                                \
        if (condition) {                                                          \
            ESP_LOGI(TAG, "selftest PASS: %s", name);                            \
        } else {                                                                  \
            ESP_LOGE(TAG, "selftest FAIL: %s", name);                            \
            failures++;                                                          \
        }                                                                         \
    } while (0)

    char uri[WS_URI_MAX];
    CHECK(websocket_uri("http://homeassistant.local:8123", uri, sizeof(uri)) &&
              strcmp(uri, "ws://homeassistant.local:8123/api/websocket") == 0,
          "HTTP base URL");
    CHECK(websocket_uri("https://ha.example.test/", uri, sizeof(uri)) &&
              strcmp(uri, "wss://ha.example.test/api/websocket") == 0,
          "HTTPS trailing slash");
    CHECK(websocket_uri("HTTP://ha.example.test:8123", uri, sizeof(uri)) &&
              strcmp(uri, "ws://ha.example.test:8123/api/websocket") == 0,
          "case-insensitive HTTP scheme");
    CHECK(!websocket_uri("ftp://ha.example.test", uri, sizeof(uri)), "scheme refused");
    CHECK(!websocket_uri("http://ha/#fragment", uri, sizeof(uri)), "fragment refused");
    CHECK(!websocket_uri("http://ha:abc", uri, sizeof(uri)), "non-numeric port refused");
    CHECK(!websocket_uri("http://ha:0", uri, sizeof(uri)), "zero port refused");
    CHECK(!websocket_uri("http://user@ha", uri, sizeof(uri)), "userinfo refused");

    payload_buffer_t assembled = {0};
    bool complete = false;
    esp_websocket_event_data_t first_chunk = {
        .data_ptr = "abc",
        .data_len = 3,
        .op_code = 0x1,
        .payload_len = 6,
        .payload_offset = 0,
        .fin = 1,
    };
    esp_websocket_event_data_t second_chunk = first_chunk;
    second_chunk.data_ptr = "def";
    second_chunk.payload_offset = 3;
    CHECK(payload_append(&assembled, &first_chunk, &complete) && !complete &&
              payload_append(&assembled, &second_chunk, &complete) && complete &&
              strcmp(assembled.data, "abcdef") == 0,
          "reassemble TCP chunks of one WebSocket frame");
    payload_reset(&assembled);

    esp_websocket_event_data_t first_fragment = first_chunk;
    first_fragment.payload_len = 3;
    first_fragment.fin = 0;
    esp_websocket_event_data_t final_fragment = first_fragment;
    final_fragment.data_ptr = "def";
    final_fragment.op_code = 0x0;
    final_fragment.fin = 1;
    CHECK(payload_append(&assembled, &first_fragment, &complete) && !complete &&
              payload_append(&assembled, &final_fragment, &complete) && complete &&
              strcmp(assembled.data, "abcdef") == 0,
          "reassemble WebSocket continuation frames");
    payload_reset(&assembled);
    CHECK(!payload_append(&assembled, &final_fragment, &complete),
          "refuse continuation frame without a message");

    uint32_t parsed_id = 0;
    cJSON *valid_id = cJSON_CreateNumber(42);
    cJSON *maximum_id = cJSON_CreateNumber(UINT32_MAX);
    cJSON *fractional_id = cJSON_CreateNumber(42.5);
    CHECK(valid_id != NULL && maximum_id != NULL && fractional_id != NULL &&
              command_id(valid_id, &parsed_id) && parsed_id == 42 &&
              command_id(maximum_id, &parsed_id) && parsed_id == UINT32_MAX &&
              !command_id(fractional_id, &parsed_id),
          "command ids are bounded positive integers");
    cJSON_Delete(valid_id);
    cJSON_Delete(maximum_id);
    cJSON_Delete(fractional_id);

    const char raw_result[] =
        "{\"result\":{\"id\":7},\"type\":\"result\",\"id\":42,"
        "\"label\":\"escaped \\\"id\\\" and }\"}";
    CHECK(raw_command_id(raw_result, strlen(raw_result), &parsed_id) &&
              parsed_id == 42 &&
              !raw_command_id("{\"id\":0}", sizeof("{\"id\":0}") - 1, &parsed_id) &&
              !raw_command_id("{\"id\":1.5}", sizeof("{\"id\":1.5}") - 1,
                              &parsed_id),
          "raw catalog response id without parsing result tree");

    ha_catalog_stage_t stage = HA_CATALOG_STAGE_COUNT;
    CHECK(catalog_stage("entities", &stage) && stage == HA_CATALOG_ENTITIES &&
              catalog_stage("devices", &stage) && stage == HA_CATALOG_DEVICES &&
              catalog_stage("areas", &stage) && stage == HA_CATALOG_AREAS &&
              catalog_stage("states", &stage) && stage == HA_CATALOG_STATES &&
              !catalog_stage("call_service", &stage),
          "catalog relay exposes only fixed read commands");

    CHECK(subscription_is_current(0, 0, SLATE_PROVIDER_ONLINE) &&
              subscription_is_current(2, 7, SLATE_PROVIDER_ONLINE) &&
              !subscription_is_current(0, 7, SLATE_PROVIDER_ONLINE) &&
              !subscription_is_current(2, 0, SLATE_PROVIDER_ONLINE) &&
              !subscription_is_current(0, 0, SLATE_PROVIDER_ERROR),
          "subscription retry checks desired ids and provider health");

    const uint32_t expected[] = {1000, 2000, 4000, 8000, 15000, 30000, 30000};
    size_t index = 0;
    bool sequence = true;
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        sequence = sequence && backoff_at(index) == expected[i];
        index = backoff_next(index);
    }
    CHECK(sequence, "1/2/4/8/15/30 second backoff ceiling");

    CHECK(slate_ha_entities_selftest() == ESP_OK,
          "compressed entity diff and normalized mapping fixtures");
    CHECK(slate_ha_actions_selftest() == ESP_OK,
          "service-call mapping and result correlation fixtures");

#undef CHECK
    ESP_LOGI(TAG, "selftest: %u failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif
