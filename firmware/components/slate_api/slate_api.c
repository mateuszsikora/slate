/*
 * Slate — HTTP API v1. See include/slate_api.h for the integration contract.
 *
 * design.md ADR-3/ADR-4, §4.1, §4.3, §9.3 and §12.
 */

#include "slate_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "slate_store.h"
#include "slate_wifi.h"

static const char *TAG = "api";

#define MODEL_ID   "waveshare-s3-touch-7"
#define SCHEMA_MAX 1

typedef struct {
    bool used;
    slate_api_auth_t auth;
    esp_err_t (*handler)(httpd_req_t *req);
    void *handler_ctx;
    httpd_uri_t wrapped;
} route_t;

static httpd_handle_t s_server;
static route_t s_routes[SLATE_API_MAX_URI_HANDLERS];

/* --- Shared response policy -------------------------------------------- */

static void set_common_headers(httpd_req_t *req)
{
    /* ADR-3: the browser talks to the device. `*` is deliberate — there are
     * no credentialed cookies, and hard-coding the station address would
     * break the same page when it is served on the setup interface. */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

esp_err_t slate_api_send_error(httpd_req_t *req, const char *status, const char *error)
{
    char body[64];
    int len = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", error);
    if (len < 0 || (size_t) len >= sizeof(body)) {
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, len);
}

static bool request_is_on_setup_ap(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage local = {0};
    socklen_t len = sizeof(local);
    if (fd < 0 || getsockname(fd, (struct sockaddr *) &local, &len) != 0 ||
        local.ss_family != AF_INET) {
        return false;
    }

    char address[INET_ADDRSTRLEN];
    const struct sockaddr_in *ipv4 = (const struct sockaddr_in *) &local;
    return inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address)) != NULL &&
           strcmp(address, SLATE_SETUP_AP_ADDRESS) == 0;
}

static bool bearer_token_matches(httpd_req_t *req)
{
    static const char PREFIX[] = "Bearer ";
    enum {
        HEADER_LEN = sizeof(PREFIX) - 1 + SLATE_DEVICE_TOKEN_LEN,
    };

    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len != HEADER_LEN) {
        return false;
    }

    char header[HEADER_LEN + 1];
    bool parsed = httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) ==
                      ESP_OK &&
                  memcmp(header, PREFIX, sizeof(PREFIX) - 1) == 0;
    bool matches = parsed &&
                   slate_store_device_token_matches(header + sizeof(PREFIX) - 1);
    memset(header, 0, sizeof(header));
    return matches;
}

static esp_err_t dispatch(httpd_req_t *req)
{
    route_t *route = req->user_ctx;
    set_common_headers(req);

    bool allowed = route->auth == SLATE_API_AUTH_PUBLIC ||
                   (route->auth == SLATE_API_AUTH_SETUP_AP && request_is_on_setup_ap(req)) ||
                   bearer_token_matches(req);
    if (!allowed) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        return slate_api_send_error(req, "401 Unauthorized", "unauthorized");
    }

    /* Preserve the handler contract: the wrapper's context is private, and
     * the route receives the context its owner registered. httpd_req_t belongs
     * to this request, so concurrent requests do not share this assignment. */
    req->user_ctx = route->handler_ctx;
    esp_err_t err = route->handler(req);
    req->user_ctx = route;
    return err;
}

esp_err_t slate_api_register_uri(const httpd_uri_t *uri, slate_api_auth_t auth)
{
    if (uri == NULL || uri->uri == NULL || uri->handler == NULL ||
        auth > SLATE_API_AUTH_SETUP_AP) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* §4.3's public surface is closed, not a convention for callers to
     * remember. Preflight carries no application data; GET /info is the one
     * public resource. A later component cannot expose a write merely by
     * selecting the wrong enum value. */
    bool public_route = uri->method == HTTP_OPTIONS ||
                        (uri->method == HTTP_GET &&
                         strcmp(uri->uri, SLATE_API_BASE_PATH "/info") == 0);
    if (auth == SLATE_API_AUTH_PUBLIC && !public_route) {
        return ESP_ERR_NOT_ALLOWED;
    }

    bool setup_ap_route =
        (uri->method == HTTP_GET && strcmp(uri->uri, "/") == 0) ||
        (uri->method == HTTP_GET &&
         strcmp(uri->uri, SLATE_API_BASE_PATH "/wifi/scan") == 0) ||
        (uri->method == HTTP_POST && strcmp(uri->uri, SLATE_API_BASE_PATH "/wifi") == 0);
    if (auth == SLATE_API_AUTH_SETUP_AP && !setup_ap_route) {
        return ESP_ERR_NOT_ALLOWED;
    }

    route_t *route = NULL;
    for (size_t i = 0; i < SLATE_API_MAX_URI_HANDLERS; i++) {
        if (!s_routes[i].used) {
            route = &s_routes[i];
            break;
        }
    }
    if (route == NULL) {
        return ESP_ERR_NO_MEM;
    }

    route->used = true;
    route->auth = auth;
    route->handler = uri->handler;
    route->handler_ctx = uri->user_ctx;
    route->wrapped = *uri;
    route->wrapped.handler = dispatch;
    route->wrapped.user_ctx = route;

    esp_err_t err = httpd_register_uri_handler(s_server, &route->wrapped);
    if (err != ESP_OK) {
        memset(route, 0, sizeof(*route));
    }
    return err;
}

/* --- Contract values --------------------------------------------------- */

static bool add_item(cJSON *object, const char *name, cJSON *item)
{
    if (item == NULL || !cJSON_AddItemToObject(object, name, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

static bool add_string_or_null(cJSON *object, const char *name, const char *value)
{
    if (value != NULL && value[0] != '\0') {
        return cJSON_AddStringToObject(object, name, value) != NULL;
    }
    return cJSON_AddNullToObject(object, name) != NULL;
}

static cJSON *network_json(const slate_wifi_status_t *status)
{
    cJSON *network = cJSON_CreateObject();
    if (network == NULL) {
        return NULL;
    }

    /* #55 changes `mode` and the active SSID when its AP is raised. Until
     * then this component truthfully has one station interface, even while
     * that station is unconfigured or retrying. */
    bool ok = cJSON_AddStringToObject(network, "mode", "sta") != NULL &&
              add_string_or_null(network, "ssid",
                                 status->connected ? status->sta_ssid : NULL) &&
              add_string_or_null(network, "ip", status->connected ? status->ip : NULL) &&
              add_string_or_null(network, "sta_ssid", status->sta_ssid) &&
              add_string_or_null(network, "last_error",
                                 slate_wifi_error_str(status->last_error));
    if (!ok) {
        cJSON_Delete(network);
        return NULL;
    }
    return network;
}

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "power_on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:  return "task_watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    case ESP_RST_EFUSE:     return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    case ESP_RST_UNKNOWN:
    default:                return "unknown";
    }
}

esp_err_t slate_api_send_json(httpd_req_t *req, cJSON *root)
{
    if (root == NULL) {
        return slate_api_send_error(req, "500 Internal Server Error", "out_of_memory");
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return slate_api_send_error(req, "500 Internal Server Error", "out_of_memory");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

/* --- Built-in routes --------------------------------------------------- */

static esp_err_t info_handler(httpd_req_t *req)
{
    slate_wifi_status_t wifi;
    slate_wifi_status(&wifi);

    char token[SLATE_DEVICE_TOKEN_LEN + 1];
    bool pairing_ready = slate_store_device_token_copy(token, sizeof(token)) == ESP_OK;
    memset(token, 0, sizeof(token));

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return slate_api_send_json(req, NULL);
    }

    bool ok = cJSON_AddStringToObject(root, "model", MODEL_ID) != NULL &&
              cJSON_AddStringToObject(root, "firmware_version", app->version) != NULL &&
              cJSON_AddNumberToObject(root, "schema_max", SCHEMA_MAX) != NULL &&
              cJSON_AddStringToObject(root, "name", slate_store_device_name()) != NULL &&
              add_item(root, "themes", cJSON_CreateArray()) &&
              cJSON_AddStringToObject(root, "pairing",
                                      pairing_ready ? "ready" : "degraded") != NULL &&
              add_item(root, "network", network_json(&wifi));
    if (!ok) {
        cJSON_Delete(root);
        return slate_api_send_json(req, NULL);
    }
    return slate_api_send_json(req, root);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    slate_wifi_status_t wifi;
    slate_wifi_status(&wifi);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return slate_api_send_json(req, NULL);
    }

    bool ok = add_item(root, "network", network_json(&wifi)) &&
              cJSON_AddStringToObject(
                  root, "ha",
                  slate_store_ha_token_is_set() ? "disconnected" : "unconfigured") != NULL;
    if (ok) {
        ok = wifi.connected ? cJSON_AddNumberToObject(root, "rssi", wifi.rssi) != NULL
                            : cJSON_AddNullToObject(root, "rssi") != NULL;
    }
    ok = ok && cJSON_AddNumberToObject(root, "uptime_s",
                                       esp_timer_get_time() / 1000000) != NULL &&
         cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size()) != NULL;

    /* #6 has not created LVGL's allocator yet. The fields land now because
     * ADR-4 makes their names contract; null says "not available" without
     * inventing a healthy-looking zero. #6 replaces these with lv_mem_monitor
     * values once an allocator exists. */
    ok = ok && cJSON_AddNullToObject(root, "lvgl_heap_free") != NULL &&
         cJSON_AddNullToObject(root, "lvgl_heap_total") != NULL &&
         cJSON_AddNullToObject(root, "lvgl_frag_pct") != NULL &&
         cJSON_AddStringToObject(root, "reset_reason",
                                 reset_reason_str(esp_reset_reason())) != NULL &&
         cJSON_AddNumberToObject(root, "reboot_count", slate_store_boot_count()) != NULL &&
         cJSON_AddNumberToObject(root, "entity_count", 0) != NULL &&
         cJSON_AddBoolToObject(root, "storage_reset",
                               slate_store_storage_was_reset()) != NULL;
    if (!ok) {
        cJSON_Delete(root);
        return slate_api_send_json(req, NULL);
    }
    return slate_api_send_json(req, root);
}

static esp_err_t options_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t slate_api_init(void)
{
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = SLATE_API_MAX_URI_HANDLERS;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    const httpd_uri_t info = {
        .uri = SLATE_API_BASE_PATH "/info",
        .method = HTTP_GET,
        .handler = info_handler,
    };
    const httpd_uri_t status = {
        .uri = SLATE_API_BASE_PATH "/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };
    const httpd_uri_t options = {
        .uri = SLATE_API_BASE_PATH "/*",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
    };

    err = slate_api_register_uri(&info, SLATE_API_AUTH_PUBLIC);
    if (err == ESP_OK) {
        err = slate_api_register_uri(&status, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (err == ESP_OK) {
        err = slate_api_register_uri(&options, SLATE_API_AUTH_PUBLIC);
    }
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(s_routes, 0, sizeof(s_routes));
        return err;
    }

    ESP_LOGI(TAG, "HTTP API listening on port %u", config.server_port);
    return ESP_OK;
}

#ifdef SLATE_API_SELFTEST

/* --- Development verifier --------------------------------------------- */

static bool send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        int written = send(fd, data, len, 0);
        if (written <= 0) {
            return false;
        }
        data += written;
        len -= written;
    }
    return true;
}

static bool selftest_request(const char *name, const char *request, int expected_status,
                             const char *expected_text)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "selftest: %-30s FAIL (socket)", name);
        return false;
    }

    struct timeval timeout = {.tv_sec = 3};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };

    char response[1536];
    size_t used = 0;
    bool ok = connect(fd, (struct sockaddr *) &address, sizeof(address)) == 0 &&
              send_all(fd, request, strlen(request));
    while (ok && used + 1 < sizeof(response)) {
        int received = recv(fd, response + used, sizeof(response) - used - 1, 0);
        if (received <= 0) {
            break;
        }
        used += received;
    }
    close(fd);
    response[used] = '\0';

    const char *space = strchr(response, ' ');
    int status = space != NULL ? atoi(space + 1) : 0;
    ok = ok && status == expected_status && strstr(response, expected_text) != NULL;
    ESP_LOGI(TAG, "selftest: %-30s %s", name, ok ? "PASS" : "FAIL");
    memset(response, 0, sizeof(response));
    return ok;
}

esp_err_t slate_api_selftest(void)
{
    static const char INFO[] =
        "GET /api/v1/info HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    static const char STATUS_NO_TOKEN[] =
        "GET /api/v1/status HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    static const char STATUS_WRONG_TOKEN[] =
        "GET /api/v1/status HTTP/1.0\r\nHost: 127.0.0.1\r\n"
        "Authorization: Bearer AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\nConnection: close\r\n\r\n";
    static const char OPTIONS[] =
        "OPTIONS /api/v1/status HTTP/1.0\r\nHost: 127.0.0.1\r\n"
        "Origin: http://editor.example\r\nAccess-Control-Request-Method: GET\r\n"
        "Connection: close\r\n\r\n";

    int failures = 0;
    failures += !selftest_request("GET /info is public", INFO, 200,
                                  "\"model\":\"" MODEL_ID "\"");
    failures += !selftest_request("GET /status needs a token", STATUS_NO_TOKEN, 401,
                                  "\"error\":\"unauthorized\"");
    failures += !selftest_request("wrong bearer token rejected", STATUS_WRONG_TOKEN, 401,
                                  "WWW-Authenticate: Bearer");

    char token[SLATE_DEVICE_TOKEN_LEN + 1];
    char request[256];
    bool token_ready = slate_store_device_token_copy(token, sizeof(token)) == ESP_OK;
    int len = token_ready ? snprintf(request, sizeof(request),
                                     "GET /api/v1/status HTTP/1.0\r\nHost: 127.0.0.1\r\n"
                                     "Authorization: Bearer %s\r\nConnection: close\r\n\r\n",
                                     token)
                          : -1;
    memset(token, 0, sizeof(token));
    bool request_ready = len > 0 && (size_t) len < sizeof(request);
    failures += !request_ready ||
                !selftest_request("correct bearer token accepted", request, 200,
                                  "\"reboot_count\":");
    memset(request, 0, sizeof(request));

    failures += !selftest_request("CORS preflight is public", OPTIONS, 204,
                                  "Access-Control-Allow-Origin: *");
    ESP_LOGI(TAG, "selftest: %d failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif
