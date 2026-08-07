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
#include "esp_netif.h"
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
        /* §4 promises every failure carries the same shape, so an error string
         * that does not fit degrades to a generic one rather than to a socket
         * that closes with no response at all. */
        ESP_LOGE(TAG, "error string does not fit the response buffer: %s", error);
        len = snprintf(body, sizeof(body), "{\"error\":\"internal\"}");
    }

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, len);
}

/**
 * Whether this request arrived on the setup access point's own address.
 *
 * §4.3's exception is "on the access point interface only", and the interface a
 * request came in on is not something HTTP carries — a Host header is whatever
 * the client typed. The local end of the accepted socket is the fact, so that is
 * what is asked.
 *
 * The address family is where this gets its one surprise, and it is worth
 * spelling out because getting it wrong fails in the safe direction and is
 * therefore quiet: `esp_http_server` opens its listening socket with `PF_INET6`
 * whenever lwIP has IPv6 compiled in, which is ESP-IDF's default. Every IPv4
 * client then arrives on a dual-stack socket, and `getsockname()` reports
 * `AF_INET6` with an IPv4-mapped address — `::ffff:192.168.4.1` — rather than
 * `AF_INET`. A check that only accepts `AF_INET` therefore matches nothing at
 * all, which does not look like a bug: every route keeps working, and the one
 * client that cannot send a token gets a 401 on the one page that is supposed to
 * work without one.
 */
static bool request_is_on_setup_ap(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage local = {0};
    socklen_t len = sizeof(local);
    if (fd < 0 || getsockname(fd, (struct sockaddr *) &local, &len) != 0) {
        return false;
    }

    uint32_t address = 0;
    if (local.ss_family == AF_INET) {
        address = ((const struct sockaddr_in *) &local)->sin_addr.s_addr;
    } else if (local.ss_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *) &local;
        if (!IN6_IS_ADDR_V4MAPPED(&ipv6->sin6_addr)) {
            /* A client that reached the panel over real IPv6 is not on the setup
             * access point, which serves IPv4 and a DHCP server (§9.2). */
            return false;
        }
        memcpy(&address, ipv6->sin6_addr.un.u8_addr + 12, sizeof(address));
    } else {
        return false;
    }

    /* Compared as an address rather than as text: one of the two sides would
     * otherwise be a string produced by inet_ntop, and "0.0.0.0" versus "::"
     * versus a mapped form is a comparison with more than one right answer. */
    return address == esp_ip4addr_aton(SLATE_SETUP_AP_ADDRESS);
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
        slate_api_send_error(req, "401 Unauthorized", "unauthorized");

        /*
         * A rejected request whose body was never read leaves esp_http_server
         * to drain it before it serves anyone else, in CONFIG_HTTPD_PURGE_BUF_LEN
         * chunks of 32 bytes, and that loop ends only when a read returns
         * nothing. An unauthenticated client announcing a huge Content-Length
         * and then trickling would hold the one HTTP task there indefinitely,
         * so a 401 with a body closes the socket: it is the only bound that
         * does not depend on the caller cooperating. A body-less 401 keeps the
         * connection, because there is nothing to drain and a browser polling
         * /info should not pay for a reconnect.
         */
        return req->content_len > 0 ? ESP_FAIL : ESP_OK;
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

typedef struct {
    bool up;
    esp_netif_ip_info_t ip;
} setup_ap_snapshot_t;

/**
 * Read the setup interface without letting its pointer escape the TCP/IP task.
 *
 * esp_netif_get_handle_from_ifkey() only serialises the lookup. The returned
 * pointer is not retained, so the setup task can destroy it before a caller's
 * following esp_netif_is_netif_up() or esp_netif_get_ip_info(). Iterating and
 * copying the two values inside esp_netif_tcpip_exec() makes the whole read one
 * operation with respect to esp_netif_destroy().
 */
static esp_err_t setup_ap_snapshot(void *ctx)
{
    setup_ap_snapshot_t *snapshot = ctx;

    for (esp_netif_t *netif = esp_netif_next_unsafe(NULL); netif != NULL;
         netif = esp_netif_next_unsafe(netif)) {
        const char *key = esp_netif_get_ifkey(netif);
        if (key == NULL || strcmp(key, "WIFI_AP_DEF") != 0) {
            continue;
        }

        snapshot->up = esp_netif_is_netif_up(netif);
        return snapshot->up ? esp_netif_get_ip_info(netif, &snapshot->ip) : ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static cJSON *network_json(const slate_wifi_status_t *status)
{
    cJSON *network = cJSON_CreateObject();
    if (network == NULL) {
        return NULL;
    }

    /*
     * Read out of esp_netif rather than kept here. #55 raises and tears down the
     * access point on its own task, and a copy of "is it up" in this component
     * would be a second answer to keep in step with the one that owns the
     * interface — which §4.1 already refuses to do for `ipv4`, for the same
     * reason: "a field that repeated the request instead would make the setup
     * page show a number meaning two different things."
     *
     * `sta` wins whenever the station has an address, even during the moment
     * before the access point is torn down. §4.1 says `ssid` is "the network the
     * device is currently on or offering", and a panel that is on one is not
     * offering one for much longer.
     */
    setup_ap_snapshot_t ap = {0};
    esp_err_t ap_err = esp_netif_tcpip_exec(setup_ap_snapshot, &ap);
    bool on_setup_ap = !status->connected && ap_err == ESP_OK && ap.up;

    char ap_address[16] = {0};
    if (on_setup_ap) {
        esp_ip4addr_ntoa(&ap.ip.ip, ap_address, sizeof(ap_address));
    }

    /* §9.2 and §4.3 make the access point's SSID the device name — "so one panel
     * is called one thing everywhere" — and slate_store owns that string, so
     * this reports it rather than building a second spelling of it. */
    bool ok = cJSON_AddStringToObject(network, "mode", on_setup_ap ? "ap" : "sta") != NULL &&
              add_string_or_null(network, "ssid",
                                 on_setup_ap ? slate_store_device_name()
                                             : (status->connected ? status->sta_ssid : NULL)) &&
              add_string_or_null(network, "ip",
                                 on_setup_ap ? ap_address
                                             : (status->connected ? status->ip : NULL)) &&
              add_string_or_null(network, "sta_ssid", status->sta_ssid);

    /*
     * §4.1: `ipv4` is "where the station's address came from, and it is reported
     * rather than echoed". In M1 there is one answer it can be — #63 implements
     * the static half, and until then a static configuration is refused by
     * `POST /wifi` rather than stored, so nothing can have come from one. The
     * field lands now because ADR-4 makes its name contract and §4.1 settles the
     * shape here rather than there.
     */
    cJSON *ipv4 = ok ? cJSON_AddObjectToObject(network, "ipv4") : NULL;
    ok = ok && ipv4 != NULL && cJSON_AddStringToObject(ipv4, "mode", "dhcp") != NULL &&
         add_string_or_null(network, "last_error", slate_wifi_error_str(status->last_error));
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

/*
 * The other half of §9.2's captive portal, and the reason it is here rather than
 * in #55's component.
 *
 * The DNS responder answers every query with the device address, so a phone's
 * portal probe — `http://captive.apple.com/hotspot-detect.html` and its
 * equivalents — arrives at this server on a path nothing is registered for. A
 * bare 404 there is what makes the portal sheet open onto an error page, so an
 * unmatched request that came in on the access point is redirected to the setup
 * page instead. That is what "phones open the page unprompted" needs.
 *
 * Doing it with a wildcard route would put every unregistered GET path behind
 * §4.3's token exception, leaving registration order to decide whether
 * `/api/v1/status` resolved to a handler or to the exception — an accident
 * waiting for the next component to register a route. A 404 handler cannot be
 * reached by any request a route matched, so it cannot shadow one.
 *
 * Everything under the API base keeps answering §4's failure shape, redirect or
 * not: a client that asked for a route this firmware does not have wants to be
 * told so, not sent a setup page with a 302.
 */
static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t error)
{
    (void) error;
    set_common_headers(req);

    static const char BASE[] = SLATE_API_BASE_PATH "/";
    bool api_request = strncmp(req->uri, BASE, sizeof(BASE) - 1) == 0;

    if (!api_request && request_is_on_setup_ap(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://" SLATE_SETUP_AP_ADDRESS "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    slate_api_send_error(req, "404 Not Found", "not_found");

    /* Same bound as the 401 above: a body nobody read is a body esp_http_server
     * drains 32 bytes at a time on the one HTTP task, and a client that keeps
     * trickling owns it for as long as it likes. */
    return req->content_len > 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t slate_api_init(void)
{
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * The first line in this firmware that opens a socket, so it is the one that
     * has to be sure lwIP is running: httpd_start() reaches the TCP/IP thread,
     * and without one the assert inside it is a panic on the second line of
     * app_main rather than an error this function could return.
     *
     * Idempotent, and here for the same reason slate_wifi_init() calls it —
     * #55's setup portal has to be watching for the station's hand-off before the
     * station exists, so the HTTP server now comes up before the radio and
     * neither component gets to assume the other went first.
     */
    esp_err_t netif_err = esp_netif_init();
    if (netif_err != ESP_OK) {
        return netif_err;
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
    if (err == ESP_OK) {
        err = httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler);
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
