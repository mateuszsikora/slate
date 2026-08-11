/*
 * Slate — Home Assistant discovery, authentication and reconnect lifecycle.
 *
 * This issue deliberately stops at the provider boundary. #75 maps
 * subscribe_entities into normalized state and #76 maps semantic actions into
 * call_service. Keeping those out is what lets a direct-provider dashboard
 * remain useful while HA is absent, unconfigured or restarting.
 */

#include "slate_ha.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mdns.h"
#include "slate_api.h"
#include "slate_state.h"
#include "slate_store.h"
#include "slate_wifi.h"

#define BODY_MAX                 (SLATE_HA_TOKEN_MAX_LEN + SLATE_HA_URL_MAX_LEN + 96)
#define WS_URI_MAX               (SLATE_HA_URL_MAX_LEN + sizeof("/api/websocket"))
#define AUTH_FRAME_MAX           (SLATE_HA_TOKEN_MAX_LEN + 64)
#define AUTH_REPLY_MAX           1024
#define MANAGER_QUEUE_DEPTH      8
#define MANAGER_TASK_STACK       6144
#define MANAGER_TASK_PRIORITY    5
#define DISCOVERY_MAX_RESULTS    8
#define DISCOVERY_TIMEOUT_MS     3000
#define CONNECT_TEST_TIMEOUT_MS  12000

static const char *TAG = "slate_ha";

static const uint32_t BACKOFF_MS[] = {1000, 2000, 4000, 8000, 15000, 30000};

typedef enum {
    CMD_WIFI_UP,
    CMD_WIFI_DOWN,
    CMD_RELOAD,
    CMD_AUTH_INVALID,
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
    esp_websocket_client_handle_t client;
    char url[SLATE_HA_URL_MAX_LEN];
    char uri[WS_URI_MAX];
    char token[SLATE_HA_TOKEN_MAX_LEN];
    message_buffer_t message;
    atomic_bool enabled;
    atomic_bool auth_rejected;
    atomic_size_t backoff_index;
} main_connection_t;

typedef struct {
    EventGroupHandle_t events;
    const char *token;
    message_buffer_t message;
} connection_test_t;

static QueueHandle_t s_commands;
static TaskHandle_t s_manager_task;
static main_connection_t s_main;
static bool s_wifi_up;
static bool s_mdns_ready;
static bool s_auth_blocked;
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
 * Redirects are intentionally not relied on: esp_websocket_client does not
 * promise to follow one, and an HTTP-to-HTTPS redirect would move credentials
 * before Slate had authenticated the destination. Query strings, fragments,
 * whitespace and an empty authority are rejected as `bad_url` instead.
 */
static bool websocket_uri(const char *url, char *out, size_t out_len)
{
    if (url == NULL || out == NULL || out_len == 0) {
        return false;
    }

    const char *rest = NULL;
    const char *scheme = NULL;
    if (strncmp(url, "http://", 7) == 0) {
        scheme = "ws://";
        rest = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        scheme = "wss://";
        rest = url + 8;
    } else {
        return false;
    }

    const char *path = strchr(rest, '/');
    size_t authority_len = path != NULL ? (size_t) (path - rest) : strlen(rest);
    if (*rest == '\0' || *rest == '/' || *rest == ':' || authority_len == 0 ||
        memchr(rest, '@', authority_len) != NULL || strchr(rest, '?') != NULL ||
        strchr(rest, '#') != NULL) {
        return false;
    }
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
    if (!s_mdns_ready) {
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
        if (!txt_value(result, "uuid", candidate.uuid, sizeof(candidate.uuid)) &&
            result->hostname != NULL) {
            strlcpy(candidate.uuid, result->hostname, sizeof(candidate.uuid));
        }
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

    if (id == WEBSOCKET_EVENT_ERROR || id == WEBSOCKET_EVENT_DISCONNECTED ||
        id == WEBSOCKET_EVENT_CLOSED) {
        EventBits_t done = xEventGroupGetBits(test->events);
        if ((done & (TEST_AUTH_OK | TEST_AUTH_INVALID)) == 0) {
            xEventGroupSetBits(test->events, TEST_TRANSPORT);
        }
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA) {
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

static void command(manager_command_t command)
{
    if (s_commands == NULL || xQueueSend(s_commands, &command, 0) != pdTRUE) {
        ESP_LOGW(TAG, "manager queue full; dropped command %d", command);
    }
}

static void main_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) base;
    main_connection_t *connection = arg;
    esp_websocket_event_data_t *event = data;
    if (!atomic_load(&connection->enabled)) {
        return;
    }

    if (id == WEBSOCKET_EVENT_BEFORE_CONNECT) {
        if (!atomic_load(&connection->auth_rejected)) {
            slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_CONNECTING);
        }
        return;
    }

    if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED ||
        id == WEBSOCKET_EVENT_ERROR) {
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

    bool complete = false;
    if (!message_append(&connection->message, event, &complete)) {
        ESP_LOGW(TAG, "discarding malformed authentication frame");
        return;
    }
    if (!complete) {
        return;
    }

    char type[24];
    bool typed = message_type(&connection->message, type, sizeof(type));
    if (typed && strcmp(type, "auth_required") == 0) {
        if (!send_auth(event->client, connection->token)) {
            ESP_LOGW(TAG, "could not send Home Assistant authentication frame");
        }
    } else if (typed && strcmp(type, "auth_ok") == 0) {
        atomic_store(&connection->backoff_index, 0);
        esp_websocket_client_set_reconnect_timeout(event->client, (int) BACKOFF_MS[0]);
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ONLINE);
        ESP_LOGI(TAG, "Home Assistant authenticated");
    } else if (typed && strcmp(type, "auth_invalid") == 0) {
        atomic_store(&connection->auth_rejected, true);
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
        ESP_LOGE(TAG, "Home Assistant rejected the stored token; waiting for new credentials");
        command(CMD_AUTH_INVALID);
    }
}

static void connection_destroy(void)
{
    if (s_main.client == NULL) {
        return;
    }
    atomic_store(&s_main.enabled, false);
    esp_websocket_client_stop(s_main.client);
    esp_websocket_client_destroy(s_main.client);
    s_main.client = NULL;
    s_main.message.used = 0;
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
        .user_context = &s_main,
    };

    atomic_store(&s_main.auth_rejected, false);
    atomic_store(&s_main.backoff_index, 0);
    s_main.client = esp_websocket_client_init(&config);
    if (s_main.client == NULL) {
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_OFFLINE);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_websocket_register_events(s_main.client, WEBSOCKET_EVENT_ANY,
                                                   main_event, &s_main);
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
        switch (received) {
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
            if (slate_store_ha_token_is_set() &&
                !atomic_load(&s_main.auth_rejected)) {
                slate_state_provider_set_status(SLATE_HA_PROVIDER_ID,
                                                SLATE_PROVIDER_OFFLINE);
            }
            break;
        case CMD_RELOAD:
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
            break;
        case CMD_AUTH_INVALID:
            s_auth_blocked = true;
            connection_destroy();
            /* ERROR is sticky until CMD_RELOAD supplies tested credentials. */
            slate_state_provider_set_status(SLATE_HA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
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

    test_result_t tested = TEST_RESULT_UNREACHABLE;
    esp_err_t stored = ESP_OK;
    if (error == NULL) {
        tested = test_credentials(uri, token);
        if (tested == TEST_RESULT_OK) {
            stored = slate_store_ha_set(url, token);
        }
    }

    clear_json_strings(root);
    cJSON_Delete(root);

    if (error != NULL) {
        return slate_api_refuse(req, "400 Bad Request", error);
    }
    if (tested == TEST_RESULT_AUTH_INVALID) {
        return slate_api_refuse(req, "422 Unprocessable Content", "ha_auth_invalid");
    }
    if (tested != TEST_RESULT_OK) {
        return slate_api_refuse(req, "502 Bad Gateway", "ha_unreachable");
    }
    if (stored != ESP_OK) {
        return slate_api_refuse(req, "500 Internal Server Error", "store_failed");
    }

    command(CMD_RELOAD);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
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

/* --- Public lifecycle -------------------------------------------------- */

esp_err_t slate_ha_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_commands = xQueueCreate(MANAGER_QUEUE_DEPTH, sizeof(manager_command_t));
    if (s_commands == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(manager_task, "slate_ha", MANAGER_TASK_STACK, NULL,
                    MANAGER_TASK_PRIORITY, &s_manager_task) != pdPASS) {
        vQueueDelete(s_commands);
        s_commands = NULL;
        return ESP_ERR_NO_MEM;
    }

    const slate_state_provider_t provider = {
        .id = SLATE_HA_PROVIDER_ID,
    };
    esp_err_t provider_err = slate_state_provider_register(&provider);
    if (provider_err == ESP_OK) {
        slate_state_provider_set_status(SLATE_HA_PROVIDER_ID,
            slate_store_ha_token_is_set() ? SLATE_PROVIDER_OFFLINE
                                          : SLATE_PROVIDER_UNCONFIGURED);
    }

    const httpd_uri_t configure = {
        .uri = SLATE_API_BASE_PATH "/ha",
        .method = HTTP_POST,
        .handler = configure_handler,
    };
    const httpd_uri_t discover = {
        .uri = SLATE_API_BASE_PATH "/ha/discover",
        .method = HTTP_GET,
        .handler = discovery_handler,
    };
    esp_err_t route_err = slate_api_register_uri(&configure, SLATE_API_AUTH_DEVICE_TOKEN);
    if (route_err == ESP_OK) {
        route_err = slate_api_register_uri(&discover, SLATE_API_AUTH_DEVICE_TOKEN);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "provider ready: POST " SLATE_API_BASE_PATH "/ha");
    return provider_err != ESP_OK ? provider_err : route_err;
}

esp_err_t slate_ha_start(void)
{
    if (!s_initialized || s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        s_mdns_ready = true;
        err = mdns_hostname_set(slate_store_device_name());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "setting mDNS hostname: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "Home Assistant discovery unavailable: %s", esp_err_to_name(err));
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
    CHECK(!websocket_uri("ftp://ha.example.test", uri, sizeof(uri)), "scheme refused");
    CHECK(!websocket_uri("http://ha/#fragment", uri, sizeof(uri)), "fragment refused");

    const uint32_t expected[] = {1000, 2000, 4000, 8000, 15000, 30000, 30000};
    size_t index = 0;
    bool sequence = true;
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        sequence = sequence && backoff_at(index) == expected[i];
        index = backoff_next(index);
    }
    CHECK(sequence, "1/2/4/8/15/30 second backoff ceiling");

#undef CHECK
    ESP_LOGI(TAG, "selftest: %u failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif
