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

#include "slate_config.h"
#include "slate_display.h"
#include "slate_state.h"
#include "slate_store.h"
#include "slate_theme.h"
#include "slate_wifi.h"

static const char *TAG = "api";

/* Spelled in the header now: §10's editor advertises the same string over
 * mDNS, and one board model per binary (ADR-1) makes it a constant both can
 * name rather than a value one of them copies. */
#define MODEL_ID SLATE_API_MODEL_ID
typedef struct {
    bool used;
    slate_api_auth_t auth;
    esp_err_t (*handler)(httpd_req_t *req);
    void *handler_ctx;
    httpd_uri_t wrapped;
} route_t;

typedef struct {
    bool used;
    char provider[SLATE_PROVIDER_ID_MAX + 1];
    slate_api_resources_append_fn append;
    void *ctx;
} resource_catalog_t;

/** The two pages of slate_api_root_t, indexed by it. */
typedef struct {
    esp_err_t (*handler)(httpd_req_t *req);
    void *ctx;
} root_page_t;

static httpd_handle_t s_server;
static route_t s_routes[SLATE_API_MAX_URI_HANDLERS];
static resource_catalog_t s_resource_catalogs[SLATE_STATE_MAX_PROVIDERS];
static root_page_t s_roots[SLATE_API_ROOT_EDITOR + 1];

#define SESSION_BODY_MAX 64
#define PIN_FAILURE_LIMIT 5
#define PIN_BLOCK_SECONDS 30

static unsigned s_pin_failures;
static int64_t s_pin_block_until_us;

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

/*
 * A rejected request whose body was not consumed leaves esp_http_server to
 * drain it before serving anyone else. The server does that on the one HTTP
 * task, in CONFIG_HTTPD_PURGE_BUF_LEN chunks of 32 bytes, and stops only when a
 * read returns nothing. A client announcing a large Content-Length and then
 * trickling can therefore hold the whole API indefinitely.
 *
 * Returning ESP_FAIL after sending the response drops that socket, which is
 * the only bound that does not depend on the caller cooperating. A body-less
 * refusal keeps the connection because there is nothing for the server to
 * purge. This is deliberately conservative after a handler consumes a body:
 * httpd_req_t retains the declared content_len rather than an unread count, so
 * the same request receives the same connection policy whichever component
 * parsed it. Routes such as OTA that always abandon a refused upload use the
 * explicit unconditional variant, including for an empty upload.
 */
static esp_err_t refuse_with_policy(httpd_req_t *req, const char *status,
                                    const char *error, bool close_connection)
{
    if (close_connection) {
        httpd_resp_set_hdr(req, "Connection", "close");
    }

    esp_err_t err = slate_api_send_error(req, status, error);
    return close_connection ? ESP_FAIL : err;
}

esp_err_t slate_api_refuse(httpd_req_t *req, const char *status, const char *error)
{
    return refuse_with_policy(req, status, error, req->content_len > 0);
}

esp_err_t slate_api_refuse_and_close(httpd_req_t *req, const char *status,
                                     const char *error)
{
    return refuse_with_policy(req, status, error, true);
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

static bool bearer_token_matches(httpd_req_t *req, bool allow_integration)
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
    const char *token = header + sizeof(PREFIX) - 1;
    bool matches = parsed &&
                   (slate_store_device_token_matches(token) ||
                    (allow_integration && slate_store_integration_key_matches(token)));
    memset(header, 0, sizeof(header));
    return matches;
}

static esp_err_t dispatch(httpd_req_t *req)
{
    route_t *route = req->user_ctx;
    set_common_headers(req);

    bool allowed = route->auth == SLATE_API_AUTH_PUBLIC ||
                   route->auth == SLATE_API_AUTH_WS_FIRST_FRAME ||
                   (route->auth == SLATE_API_AUTH_SETUP_AP && request_is_on_setup_ap(req)) ||
                   bearer_token_matches(
                       req, route->auth == SLATE_API_AUTH_DEVICE_OR_INTEGRATION);
    if (!allowed) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        return slate_api_refuse(req, "401 Unauthorized", "unauthorized");
    }

    /* Preserve the handler contract: the wrapper's context is private, and
     * the route receives the context its owner registered. httpd_req_t belongs
     * to this request, so concurrent requests do not share this assignment. */
    req->user_ctx = route->handler_ctx;
    esp_err_t err = route->handler(req);
    req->user_ctx = route;
    return err;
}

/**
 * Install a route behind dispatch(), with no policy of its own.
 *
 * Separate from slate_api_register_uri() so that the closed public surface
 * below is a check on *callers* rather than a check this file has to evade for
 * the two routes it owns itself — `GET /` among them, which no component may
 * register and which has no single auth policy to declare (see root_dispatch).
 */
static esp_err_t register_route(const httpd_uri_t *uri, slate_api_auth_t auth)
{
    route_t *route = NULL;
    for (size_t i = 0; i < SLATE_API_MAX_URI_HANDLERS; i++) {
        if (!s_routes[i].used) {
            route = &s_routes[i];
            break;
        }
    }
    if (route == NULL) {
        /* Named here because it arrives everywhere else as a component
         * reporting ESP_ERR_NO_MEM on a device with megabytes free: the
         * caller that overflows the table is simply the one that registered
         * last, and nothing about its message says the table is the cause. */
        ESP_LOGE(TAG, "no route slot left for %s — SLATE_API_MAX_URI_HANDLERS is %d", uri->uri,
                 SLATE_API_MAX_URI_HANDLERS);
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

/*
 * `GET /`, resolved by the interface the request arrived on rather than by
 * which component registered a handler last. §9.2 puts the setup page on the
 * access point — where a captive portal probe is redirected to it and a phone
 * types the address off the screen — and §10 puts the editor everywhere else.
 *
 * Neither page carries a token. The shared response policy still applies to
 * both, `Cache-Control: no-store` included: the editor bundle is a single
 * gzipped document on a LAN, and an exception to the one place that decides
 * caching would cost more than re-sending it.
 */
static esp_err_t root_dispatch(httpd_req_t *req)
{
    const root_page_t *page = request_is_on_setup_ap(req)
                                  ? &s_roots[SLATE_API_ROOT_SETUP_AP]
                                  : &s_roots[SLATE_API_ROOT_EDITOR];
    if (page->handler == NULL) {
        return slate_api_refuse(req, "404 Not Found", "not_found");
    }

    req->user_ctx = page->ctx;
    return page->handler(req);
}

esp_err_t slate_api_register_root(slate_api_root_t which,
                                  esp_err_t (*handler)(httpd_req_t *req),
                                  void *user_ctx)
{
    if (handler == NULL || which > SLATE_API_ROOT_EDITOR) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_roots[which].handler != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool first = s_roots[SLATE_API_ROOT_SETUP_AP].handler == NULL &&
                 s_roots[SLATE_API_ROOT_EDITOR].handler == NULL;
    s_roots[which].handler = handler;
    s_roots[which].ctx = user_ctx;
    if (!first) {
        return ESP_OK;
    }

    static const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_dispatch,
    };
    esp_err_t err = register_route(&root, SLATE_API_AUTH_PUBLIC);
    if (err != ESP_OK) {
        s_roots[which].handler = NULL;
        s_roots[which].ctx = NULL;
    }
    return err;
}

esp_err_t slate_api_register_uri(const httpd_uri_t *uri, slate_api_auth_t auth)
{
    if (uri == NULL || uri->uri == NULL || uri->handler == NULL ||
        auth > SLATE_API_AUTH_WS_FIRST_FRAME) {
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
                         strcmp(uri->uri, SLATE_API_BASE_PATH "/info") == 0) ||
                        (uri->method == HTTP_POST &&
                         strcmp(uri->uri, SLATE_API_BASE_PATH "/session") == 0);
    if (auth == SLATE_API_AUTH_PUBLIC && !public_route) {
        return ESP_ERR_NOT_ALLOWED;
    }

    /* The setup page itself is no longer in this list: `/` is the one route
     * both §9.2 and §10 claim, so it belongs to slate_api_register_root() and
     * cannot be registered as an ordinary route by anyone. What remains is the
     * two endpoints the page needs. */
    bool setup_ap_route =
        (uri->method == HTTP_GET &&
         strcmp(uri->uri, SLATE_API_BASE_PATH "/wifi/scan") == 0) ||
        (uri->method == HTTP_POST && strcmp(uri->uri, SLATE_API_BASE_PATH "/wifi") == 0);
    if (auth == SLATE_API_AUTH_SETUP_AP && !setup_ap_route) {
        return ESP_ERR_NOT_ALLOWED;
    }

    bool first_frame_ws_route = uri->method == HTTP_GET && uri->is_websocket &&
                                strcmp(uri->uri, SLATE_API_BASE_PATH "/ws") == 0;
    if (auth == SLATE_API_AUTH_WS_FIRST_FRAME && !first_frame_ws_route) {
        return ESP_ERR_NOT_ALLOWED;
    }

    bool integration_route = uri->method == HTTP_POST &&
                             strcmp(uri->uri,
                                    SLATE_API_BASE_PATH "/direct/state") == 0;
    if (auth == SLATE_API_AUTH_DEVICE_OR_INTEGRATION && !integration_route) {
        return ESP_ERR_NOT_ALLOWED;
    }

    /* No component gets to install `/`, whichever policy it names for it. */
    if (strcmp(uri->uri, "/") == 0) {
        return ESP_ERR_NOT_ALLOWED;
    }

    return register_route(uri, auth);
}

esp_err_t slate_api_resources_register(const char *provider,
                                       slate_api_resources_append_fn append,
                                       void *ctx)
{
    if (provider == NULL || provider[0] == '\0' ||
        strnlen(provider, SLATE_PROVIDER_ID_MAX + 1) > SLATE_PROVIDER_ID_MAX ||
        append == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    resource_catalog_t *free_slot = NULL;
    for (size_t i = 0; i < SLATE_STATE_MAX_PROVIDERS; i++) {
        if (s_resource_catalogs[i].used &&
            strcmp(s_resource_catalogs[i].provider, provider) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!s_resource_catalogs[i].used && free_slot == NULL) {
            free_slot = &s_resource_catalogs[i];
        }
    }
    if (free_slot == NULL) {
        return ESP_ERR_NO_MEM;
    }

    free_slot->used = true;
    strlcpy(free_slot->provider, provider, sizeof(free_slot->provider));
    free_slot->append = append;
    free_slot->ctx = ctx;
    return ESP_OK;
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

static cJSON *themes_json(void)
{
    cJSON *themes = cJSON_CreateArray();
    if (themes == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < slate_theme_count(); i++) {
        const slate_theme_t *theme = slate_theme_at(i);
        cJSON *id = theme != NULL ? cJSON_CreateString(theme->id) : NULL;
        if (id == NULL || !cJSON_AddItemToArray(themes, id)) {
            cJSON_Delete(id);
            cJSON_Delete(themes);
            return NULL;
        }
    }
    return themes;
}

/**
 * How many bound resources the store holds for one provider, and its status.
 *
 * §4.1 is explicit that a provider's `resource_count` is not the top-level one:
 * this is what the dashboard binds through that provider, while the number
 * beside it is the distinct resources the store holds in total. An id nothing
 * has registered — `ha` until M3 supplies its adapter — reads as unconfigured
 * with nothing bound, which is what §5.1 says such a provider is.
 */
static slate_state_provider_info_t provider_info(const char *id)
{
    slate_state_provider_info_t info = {0};
    strlcpy(info.id, id, sizeof(info.id));

    for (size_t i = 0; i < slate_state_provider_count(); i++) {
        slate_state_provider_info_t entry;
        if (slate_state_provider_at(i, &entry) == ESP_OK && strcmp(entry.id, id) == 0) {
            return entry;
        }
    }
    return info;
}

static cJSON *providers_json(void)
{
    /*
     * §5.1 names two provider ids for version 1 and makes both of them always
     * present in this list — `direct` because it is, and `ha` because it is
     * "present but `unconfigured` until it has credentials". So the shape is
     * stated here and the values are read from the provider registry: `direct`'s status is
     * §5.4's real one, `degraded` with no attached action consumer and `online`
     * with one, rather than the constant that stood in before it had a provider
     * to report. The HA adapter likewise owns its real connection lifecycle;
     * an absent registration still falls back to `unconfigured`, preserving the
     * stable response shape if that component could not initialise.
     */
    const slate_state_provider_info_t direct_info = provider_info("direct");
    const slate_state_provider_info_t ha_info = provider_info("ha");

    cJSON *providers = cJSON_CreateArray();
    cJSON *direct = cJSON_CreateObject();
    cJSON *ha = cJSON_CreateObject();
    bool ok = providers && direct && ha &&
              cJSON_AddStringToObject(direct, "id", "direct") != NULL &&
              cJSON_AddStringToObject(
                  direct, "status", slate_provider_status_str(direct_info.status)) != NULL &&
              cJSON_AddNumberToObject(direct, "resource_count",
                                      direct_info.resource_count) != NULL &&
              cJSON_AddStringToObject(ha, "id", "ha") != NULL &&
              cJSON_AddStringToObject(
                  ha, "status", slate_provider_status_str(ha_info.status)) != NULL &&
              cJSON_AddNumberToObject(ha, "resource_count", ha_info.resource_count) != NULL;

    if (ok) {
        ok = cJSON_AddItemToArray(providers, direct);
        if (ok) {
            direct = NULL;
            ok = cJSON_AddItemToArray(providers, ha);
            if (ok) {
                ha = NULL;
            }
        }
    }
    if (!ok) {
        cJSON_Delete(providers);
        cJSON_Delete(direct);
        cJSON_Delete(ha);
        return NULL;
    }
    return providers;
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

/** A host-order address as the dotted quad §4.1 spells everything else in. */
static char *quad(uint32_t address, char *out, size_t out_len)
{
    snprintf(out, out_len, "%u.%u.%u.%u", (unsigned) (address >> 24 & 0xFF),
             (unsigned) (address >> 16 & 0xFF), (unsigned) (address >> 8 & 0xFF),
             (unsigned) (address & 0xFF));
    return out;
}

/**
 * §4.1's `ipv4`: where the address came from, and what the panel was told to use.
 *
 * Two facts rather than one, because after §9.6's revert they differ and both
 * are needed. `mode` is the report — the station reads it off the interface, so
 * a configuration that failed its trial says `dhcp` here, which is what the
 * address on the screen actually is. `static` is what somebody typed and what
 * became of it, which is what the setup page pre-fills the form from; §9.6 makes
 * the recovery path "correct one field of what you already typed", and a page
 * that could only see the mode would have nothing to put in the other fields.
 *
 * `static` is `null` when nothing is stored, which is every DHCP panel. A field
 * added rather than one changed: ADR-4 makes the name contract, and §3.1's rule
 * about unknown fields points the same way for a client written against a
 * firmware that predates this.
 */
static bool ipv4_json(cJSON *ipv4, const slate_wifi_status_t *status)
{
    if (cJSON_AddStringToObject(ipv4, "mode", status->ipv4_static ? "static" : "dhcp") == NULL) {
        return false;
    }

    slate_ipv4_config_t stored;
    slate_store_ipv4_get(&stored);
    if (stored.state == SLATE_IPV4_DHCP) {
        return cJSON_AddNullToObject(ipv4, "static") != NULL;
    }

    char text[20];
    cJSON *object = cJSON_AddObjectToObject(ipv4, "static");
    bool ok = object != NULL &&
              cJSON_AddStringToObject(object, "state", slate_ipv4_state_str(stored.state)) != NULL;

    if (ok) {
        /* The address carries its prefix, in the CIDR form `POST /wifi` accepts,
         * so what comes back out of this endpoint can be sent straight back in.
         * The setup page does exactly that after a revert. */
        size_t len = strlen(quad(stored.address, text, sizeof(text)));
        snprintf(text + len, sizeof(text) - len, "/%u", (unsigned) stored.prefix);
        ok = cJSON_AddStringToObject(object, "address", text) != NULL &&
             cJSON_AddStringToObject(object, "gateway",
                                     quad(stored.gateway, text, sizeof(text))) != NULL;
    }

    cJSON *dns = ok ? cJSON_AddArrayToObject(object, "dns") : NULL;
    ok = ok && dns != NULL;
    for (unsigned i = 0; ok && i < stored.dns_count; i++) {
        cJSON *server = cJSON_CreateString(quad(stored.dns[i], text, sizeof(text)));
        ok = server != NULL && cJSON_AddItemToArray(dns, server);
        if (!ok) {
            cJSON_Delete(server);
        }
    }
    return ok;
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

    cJSON *ipv4 = ok ? cJSON_AddObjectToObject(network, "ipv4") : NULL;
    ok = ok && ipv4 != NULL && ipv4_json(ipv4, status) &&
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
        return slate_api_refuse(req, "500 Internal Server Error", "out_of_memory");
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return slate_api_refuse(req, "500 Internal Server Error", "out_of_memory");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

static bool add_capability(cJSON *object, const slate_capabilities_t *caps,
                           slate_action_t action)
{
    if (!slate_capabilities_have(caps, action)) {
        return true;
    }

    const char *name = slate_action_str(action);
    int16_t min = 0;
    int16_t max = 0;
    bool ranged = false;
    if (action == SLATE_ACTION_SET_BRIGHTNESS) {
        min = caps->brightness_min;
        max = caps->brightness_max;
        ranged = true;
    } else if (action == SLATE_ACTION_SET_COLOR_TEMPERATURE) {
        min = caps->color_temperature_min;
        max = caps->color_temperature_max;
        ranged = min != 0 || max != 0;
    } else if (action == SLATE_ACTION_SET_POSITION) {
        min = caps->position_min;
        max = caps->position_max;
        ranged = true;
    }

    if (!ranged) {
        return cJSON_AddBoolToObject(object, name, true) != NULL;
    }
    cJSON *range = cJSON_AddObjectToObject(object, name);
    return range != NULL && cJSON_AddNumberToObject(range, "min", min) != NULL &&
           cJSON_AddNumberToObject(range, "max", max) != NULL;
}

static const char *cover_motion_str(slate_cover_motion_t motion)
{
    switch (motion) {
    case SLATE_COVER_OPENING: return "opening";
    case SLATE_COVER_CLOSING: return "closing";
    case SLATE_COVER_IDLE:
    default:                  return "idle";
    }
}

esp_err_t slate_api_resource_append(cJSON *array, const slate_resource_t *resource)
{
    if (!cJSON_IsArray(array) || resource == NULL || resource->provider[0] == '\0' ||
        resource->resource[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *item = cJSON_CreateObject();
    cJSON *state = item != NULL ? cJSON_AddObjectToObject(item, "state") : NULL;
    bool available = resource->presentation == SLATE_PRESENT_OK;
    bool ok = state != NULL &&
              cJSON_AddStringToObject(item, "provider", resource->provider) != NULL &&
              cJSON_AddStringToObject(item, "resource", resource->resource) != NULL &&
              cJSON_AddStringToObject(item, "kind", slate_kind_str(resource->kind)) != NULL &&
              cJSON_AddBoolToObject(item, "available", available) != NULL;
    if (ok && resource->name[0] != '\0') {
        ok = cJSON_AddStringToObject(item, "name", resource->name) != NULL;
    }
    if (ok && resource->area[0] != '\0') {
        ok = cJSON_AddStringToObject(item, "area", resource->area) != NULL;
    }

    if (ok && resource->kind == SLATE_KIND_LIGHT) {
        ok = cJSON_AddStringToObject(state, "power",
                                     resource->state.light.on ? "on" : "off") != NULL;
        if (ok && resource->state.light.brightness != SLATE_STATE_ABSENT) {
            ok = cJSON_AddNumberToObject(state, "brightness",
                                         resource->state.light.brightness) != NULL;
        }
        if (ok && resource->state.light.color_temperature != SLATE_STATE_ABSENT) {
            ok = cJSON_AddNumberToObject(state, "color_temperature",
                                         resource->state.light.color_temperature) != NULL;
        }
    } else if (ok && resource->kind == SLATE_KIND_COVER) {
        if (resource->state.cover.position != SLATE_STATE_ABSENT) {
            ok = cJSON_AddNumberToObject(state, "position",
                                         resource->state.cover.position) != NULL;
        }
        ok = ok && cJSON_AddStringToObject(
                       state, "motion", cover_motion_str(resource->state.cover.motion)) != NULL;
    } else if (ok && resource->kind == SLATE_KIND_SENSOR) {
        ok = resource->state.sensor.numeric
                 ? cJSON_AddNumberToObject(state, "value",
                                           resource->state.sensor.value) != NULL
                 : cJSON_AddStringToObject(state, "value",
                                           resource->state.sensor.text) != NULL;
        if (ok && resource->state.sensor.unit[0] != '\0') {
            ok = cJSON_AddStringToObject(state, "unit",
                                         resource->state.sensor.unit) != NULL;
        }
        const char *measurement = slate_measurement_str(resource->state.sensor.measurement);
        if (ok && measurement != NULL) {
            ok = cJSON_AddStringToObject(state, "measurement", measurement) != NULL;
        }
        /* Reported even when the store derived it from `measurement`, because
         * §5.2 promises a complete current value and a picker that had to redo
         * that derivation would be the second copy of it. */
        const char *category = slate_category_str(resource->state.sensor.category);
        if (ok && category != NULL) {
            ok = cJSON_AddStringToObject(state, "category", category) != NULL;
        }
    }

    if (ok && resource->capabilities.actions != 0) {
        cJSON *capabilities = cJSON_AddObjectToObject(item, "capabilities");
        ok = capabilities != NULL;
        for (slate_action_t action = 0; ok && action < SLATE_ACTION_COUNT; action++) {
            ok = add_capability(capabilities, &resource->capabilities, action);
        }
    }
    if (ok) {
        ok = cJSON_AddItemToArray(array, item);
        if (ok) {
            item = NULL;
        }
    }
    cJSON_Delete(item);
    return ok ? ESP_OK : ESP_ERR_NO_MEM;
}

/* --- Built-in routes --------------------------------------------------- */

static esp_err_t info_handler(httpd_req_t *req)
{
    slate_wifi_status_t wifi;
    slate_wifi_status(&wifi);

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return slate_api_send_json(req, NULL);
    }

    bool ok = cJSON_AddStringToObject(root, "model", MODEL_ID) != NULL &&
              cJSON_AddStringToObject(root, "firmware_version", app->version) != NULL &&
              cJSON_AddNumberToObject(root, "schema_max", SLATE_CONFIG_SCHEMA_MAX) != NULL &&
              cJSON_AddStringToObject(root, "name", slate_store_device_name()) != NULL &&
              add_item(root, "themes", themes_json()) &&
              cJSON_AddStringToObject(root, "authentication",
                                      slate_store_admin_pin_is_set() ? "pin" : "open") != NULL &&
              add_item(root, "network", network_json(&wifi));
    if (!ok) {
        cJSON_Delete(root);
        return slate_api_send_json(req, NULL);
    }
    return slate_api_send_json(req, root);
}

static esp_err_t session_handler(httpd_req_t *req)
{
    /* The setup AP deliberately exposes only network provisioning. An open
     * editor must not turn that radio-range exception into full API access. */
    if (request_is_on_setup_ap(req)) {
        return slate_api_refuse(req, "401 Unauthorized", "unauthorized");
    }

    const bool pin_required = slate_store_admin_pin_is_set();
    const int64_t now = esp_timer_get_time();
    if (pin_required && now < s_pin_block_until_us) {
        int64_t seconds = (s_pin_block_until_us - now + 999999) / 1000000;
        char retry_after[24];
        snprintf(retry_after, sizeof(retry_after), "%lld", (long long) seconds);
        httpd_resp_set_hdr(req, "Retry-After", retry_after);
        return slate_api_refuse(req, "429 Too Many Requests", "try_later");
    }

    char body[SESSION_BODY_MAX] = {0};
    char pin[SLATE_ADMIN_PIN_MAX_LEN + 1] = {0};
    bool parsed = !pin_required;

    if (pin_required && req->content_len > 0 && req->content_len < sizeof(body)) {
        size_t received = 0;
        while (received < req->content_len) {
            int chunk = httpd_req_recv(req, body + received, req->content_len - received);
            if (chunk <= 0) {
                explicit_bzero(body, sizeof(body));
                return slate_api_refuse_and_close(req, "400 Bad Request", "truncated");
            }
            received += (size_t) chunk;
        }

        cJSON *root = cJSON_Parse(body);
        cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "pin");
        if (cJSON_IsString(value) && value->valuestring != NULL &&
            strlen(value->valuestring) <= SLATE_ADMIN_PIN_MAX_LEN) {
            strlcpy(pin, value->valuestring, sizeof(pin));
            parsed = true;
        }
        if (cJSON_IsString(value) && value->valuestring != NULL) {
            explicit_bzero(value->valuestring, strlen(value->valuestring));
        }
        cJSON_Delete(root);
    }
    explicit_bzero(body, sizeof(body));

    bool accepted = parsed && (!pin_required || slate_store_admin_pin_matches(pin));
    explicit_bzero(pin, sizeof(pin));
    if (!accepted) {
        s_pin_failures++;
        if (s_pin_failures >= PIN_FAILURE_LIMIT) {
            s_pin_failures = 0;
            s_pin_block_until_us = now + PIN_BLOCK_SECONDS * 1000000LL;
            httpd_resp_set_hdr(req, "Retry-After", "30");
            return slate_api_refuse(req, "429 Too Many Requests", "try_later");
        }
        return slate_api_refuse(req, "401 Unauthorized", "invalid_pin");
    }

    s_pin_failures = 0;
    s_pin_block_until_us = 0;
    char token[SLATE_DEVICE_TOKEN_LEN + 1] = {0};
    if (slate_store_device_token_copy(token, sizeof(token)) != ESP_OK) {
        return slate_api_refuse(req, "500 Internal Server Error", "session_failed");
    }

    cJSON *response = cJSON_CreateObject();
    bool ok = response != NULL && cJSON_AddStringToObject(response, "token", token) != NULL;
    explicit_bzero(token, sizeof(token));
    if (!ok) {
        cJSON_Delete(response);
        return slate_api_send_json(req, NULL);
    }
    return slate_api_send_json(req, response);
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
              add_item(root, "providers", providers_json());
    if (ok) {
        ok = wifi.connected ? cJSON_AddNumberToObject(root, "rssi", wifi.rssi) != NULL
                            : cJSON_AddNullToObject(root, "rssi") != NULL;
    }
    ok = ok && cJSON_AddNumberToObject(root, "uptime_s",
                                       esp_timer_get_time() / 1000000) != NULL &&
         cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size()) != NULL;

    slate_display_heap_metrics_t lvgl;
    slate_display_heap_metrics(&lvgl);
    if (ok && lvgl.available) {
        ok = cJSON_AddNumberToObject(root, "lvgl_heap_free", lvgl.free_size) != NULL &&
             cJSON_AddNumberToObject(root, "lvgl_heap_total", lvgl.total_size) != NULL &&
             cJSON_AddNumberToObject(root, "lvgl_frag_pct", lvgl.frag_pct) != NULL;
    } else if (ok) {
        ok = cJSON_AddNullToObject(root, "lvgl_heap_free") != NULL &&
             cJSON_AddNullToObject(root, "lvgl_heap_total") != NULL &&
             cJSON_AddNullToObject(root, "lvgl_frag_pct") != NULL;
    }
    ok = ok && cJSON_AddStringToObject(root, "reset_reason",
                                 reset_reason_str(esp_reset_reason())) != NULL &&
         cJSON_AddNumberToObject(root, "reboot_count", slate_store_boot_count()) != NULL &&
         cJSON_AddNumberToObject(root, "resource_count", slate_state_count()) != NULL &&
         cJSON_AddBoolToObject(root, "storage_reset",
                               slate_store_storage_was_reset()) != NULL;
    if (!ok) {
        cJSON_Delete(root);
        return slate_api_send_json(req, NULL);
    }
    return slate_api_send_json(req, root);
}

static bool provider_registered(const char *id, slate_provider_status_t *status)
{
    for (size_t i = 0; i < slate_state_provider_count(); i++) {
        slate_state_provider_info_t info;
        if (slate_state_provider_at(i, &info) == ESP_OK && strcmp(info.id, id) == 0) {
            *status = info.status;
            return true;
        }
    }
    return false;
}

static resource_catalog_t *resource_catalog(const char *provider)
{
    for (size_t i = 0; i < SLATE_STATE_MAX_PROVIDERS; i++) {
        if (s_resource_catalogs[i].used &&
            strcmp(s_resource_catalogs[i].provider, provider) == 0) {
            return &s_resource_catalogs[i];
        }
    }
    return NULL;
}

static esp_err_t append_bound_resources(const char *provider, cJSON *array)
{
    size_t count = slate_state_count();
    for (size_t i = 0; i < count; i++) {
        slate_resource_t resource;
        esp_err_t err = slate_state_at(i, &resource);
        if (err == ESP_ERR_NOT_FOUND) {
            continue; /* A configuration replacement shortened the table. */
        }
        if (err != ESP_OK) {
            return err;
        }
        if (resource.updated_us != 0 && strcmp(resource.provider, provider) == 0) {
            err = slate_api_resource_append(array, &resource);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t resources_handler(httpd_req_t *req)
{
    char provider[SLATE_PROVIDER_ID_MAX + 1];
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) {
        return slate_api_refuse(req, "400 Bad Request", "provider_required");
    }

    char *query = malloc(query_len + 1);
    if (query == NULL) {
        return slate_api_send_json(req, NULL);
    }
    esp_err_t query_err = httpd_req_get_url_query_str(req, query, query_len + 1);
    esp_err_t provider_err = query_err == ESP_OK
                                 ? httpd_query_key_value(query, "provider", provider,
                                                        sizeof(provider))
                                 : query_err;
    free(query);
    if (provider_err == ESP_ERR_HTTPD_RESULT_TRUNC) {
        /* A present id outside the public provider-id bound is unknown, not
         * missing. Keep the same answer as any other non-registered id. */
        return slate_api_refuse(req, "404 Not Found", "provider_not_found");
    }
    if (provider_err != ESP_OK || provider[0] == '\0') {
        return slate_api_refuse(req, "400 Bad Request", "provider_required");
    }

    slate_provider_status_t status;
    if (!provider_registered(provider, &status)) {
        return slate_api_refuse(req, "404 Not Found", "provider_not_found");
    }
    if (status == SLATE_PROVIDER_UNCONFIGURED) {
        return slate_api_refuse(req, "409 Conflict", "provider_unconfigured");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = root != NULL ? cJSON_AddArrayToObject(root, "resources") : NULL;
    if (array == NULL) {
        cJSON_Delete(root);
        return slate_api_send_json(req, NULL);
    }

    resource_catalog_t *catalog = resource_catalog(provider);
    esp_err_t err = catalog != NULL ? catalog->append(catalog->ctx, array)
                                    : append_bound_resources(provider, array);
    if (err != ESP_OK) {
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

    return slate_api_refuse(req, "404 Not Found", "not_found");
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
    const httpd_uri_t session = {
        .uri = SLATE_API_BASE_PATH "/session",
        .method = HTTP_POST,
        .handler = session_handler,
    };
    const httpd_uri_t resources = {
        .uri = SLATE_API_BASE_PATH "/resources",
        .method = HTTP_GET,
        .handler = resources_handler,
    };
    const httpd_uri_t options = {
        .uri = SLATE_API_BASE_PATH "/*",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
    };

    err = slate_api_register_uri(&info, SLATE_API_AUTH_PUBLIC);
    if (err == ESP_OK) {
        err = slate_api_register_uri(&session, SLATE_API_AUTH_PUBLIC);
    }
    if (err == ESP_OK) {
        err = slate_api_register_uri(&status, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (err == ESP_OK) {
        err = slate_api_register_uri(&resources, SLATE_API_AUTH_DEVICE_TOKEN);
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
        memset(s_roots, 0, sizeof(s_roots));
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

static int selftest_connect(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return -1;
    }

    struct timeval timeout = {.tv_sec = 3};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (connect(fd, (struct sockaddr *) &address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool receive_response(int fd, char *response, size_t capacity)
{
    size_t used = 0;
    size_t response_len = 0;
    bool response_len_known = false;

    while (used + 1 < capacity) {
        int received = recv(fd, response + used, capacity - used - 1, 0);
        if (received <= 0) {
            return false;
        }
        used += received;
        response[used] = '\0';

        if (!response_len_known) {
            char *headers_end = strstr(response, "\r\n\r\n");
            if (headers_end != NULL) {
                static const char LENGTH_HEADER[] = "\r\nContent-Length: ";
                char *length = strstr(response, LENGTH_HEADER);
                if (length == NULL || length >= headers_end) {
                    return false;
                }
                length += sizeof(LENGTH_HEADER) - 1;

                char *end = NULL;
                unsigned long body_len = strtoul(length, &end, 10);
                size_t header_len = (size_t) (headers_end - response) + 4;
                if (end == length || end[0] != '\r' || end[1] != '\n' ||
                    body_len > capacity - header_len - 1) {
                    return false;
                }
                response_len = header_len + body_len;
                response_len_known = true;
            }
        }

        if (response_len_known && used >= response_len) {
            return true;
        }
    }
    return false;
}

static bool response_matches(const char *response, int expected_status,
                             const char *expected_text)
{
    const char *space = strchr(response, ' ');
    int status = space != NULL ? atoi(space + 1) : 0;
    return status == expected_status && strstr(response, expected_text) != NULL;
}

static bool selftest_request(const char *name, const char *request, int expected_status,
                             const char *expected_text)
{
    int fd = selftest_connect();
    if (fd < 0) {
        ESP_LOGE(TAG, "selftest: %-30s FAIL (connect)", name);
        return false;
    }

    char response[1536] = {0};
    bool ok = send_all(fd, request, strlen(request)) &&
              receive_response(fd, response, sizeof(response));
    close(fd);
    ok = ok && response_matches(response, expected_status, expected_text);
    ESP_LOGI(TAG, "selftest: %-30s %s", name, ok ? "PASS" : "FAIL");
    memset(response, 0, sizeof(response));
    return ok;
}

static bool selftest_bodyless_refusal_keeps_connection(void)
{
    static const char REFUSED[] =
        "GET /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    static const char FOLLOW_UP[] =
        "GET /api/v1/info HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";

    int fd = selftest_connect();
    char response[1536] = {0};
    bool ok = fd >= 0 && send_all(fd, REFUSED, strlen(REFUSED)) &&
              receive_response(fd, response, sizeof(response)) &&
              response_matches(response, 401, "\"error\":\"unauthorized\"") &&
              strstr(response, "\r\nConnection: close\r\n") == NULL;

    memset(response, 0, sizeof(response));
    ok = ok && send_all(fd, FOLLOW_UP, strlen(FOLLOW_UP)) &&
         receive_response(fd, response, sizeof(response)) &&
         response_matches(response, 200, "\"model\":\"" MODEL_ID "\"");
    if (fd >= 0) {
        close(fd);
    }

    ESP_LOGI(TAG, "selftest: %-30s %s", "bodyless refusal keeps socket",
             ok ? "PASS" : "FAIL");
    return ok;
}

static bool selftest_body_refusal_closes_connection(void)
{
    static const char REFUSED[] =
        "GET /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Content-Length: 1\r\n\r\nx";

    int fd = selftest_connect();
    char response[1536] = {0};
    bool ok = fd >= 0 && send_all(fd, REFUSED, strlen(REFUSED)) &&
              receive_response(fd, response, sizeof(response)) &&
              response_matches(response, 401, "\"error\":\"unauthorized\"") &&
              strstr(response, "\r\nConnection: close\r\n") != NULL;

    char byte;
    ok = ok && recv(fd, &byte, sizeof(byte), 0) == 0;
    if (fd >= 0) {
        close(fd);
    }

    ESP_LOGI(TAG, "selftest: %-30s %s", "body refusal closes socket",
             ok ? "PASS" : "FAIL");
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
    failures += !selftest_request("GET /info advertises compiled themes", INFO, 200,
                                  "\"themes\":[\"midnight\",\"minimal-light\"]");
    failures += !selftest_request("GET /status needs a token", STATUS_NO_TOKEN, 401,
                                  "\"error\":\"unauthorized\"");
    failures += !selftest_request("wrong bearer token rejected", STATUS_WRONG_TOKEN, 401,
                                  "WWW-Authenticate: Bearer");

    char token[SLATE_DEVICE_TOKEN_LEN + 1];
    char request[320];
    bool token_ready = slate_store_device_token_copy(token, sizeof(token)) == ESP_OK;
    int len = token_ready ? snprintf(request, sizeof(request),
                                     "GET /api/v1/status HTTP/1.0\r\nHost: 127.0.0.1\r\n"
                                     "Authorization: Bearer %s\r\nConnection: close\r\n\r\n",
                                     token)
                          : -1;
    bool request_ready = len > 0 && (size_t) len < sizeof(request);
    failures += !request_ready ||
                !selftest_request("correct bearer token accepted", request, 200,
                                  "\"providers\":[{\"id\":\"direct\","
                                  "\"status\":\"degraded\"");
    memset(request, 0, sizeof(request));

    static const struct {
        const char *path;
        int status;
        const char *error;
        const char *name;
    } RESOURCE_CASES[] = {
        {"/api/v1/resources", 400, "\"error\":\"provider_required\"",
         "resources require provider"},
        {"/api/v1/resources?provider=does-not-exist", 404,
         "\"error\":\"provider_not_found\"", "resources reject unknown provider"},
        {"/api/v1/resources?provider=unknown-provider-long", 404,
         "\"error\":\"provider_not_found\"", "resources reject long unknown provider"},
        {"/api/v1/resources?provider=direct&padding=1234567890123456789012345678901234567890",
         200, "\"resources\":[", "resources accept unrelated query fields"},
        {"/api/v1/resources?provider=direct", 200, "\"resources\":[",
         "direct resources use common route"},
    };
    for (size_t i = 0; token_ready && i < sizeof(RESOURCE_CASES) / sizeof(RESOURCE_CASES[0]);
         i++) {
        len = snprintf(request, sizeof(request),
                       "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\n"
                       "Authorization: Bearer %s\r\nConnection: close\r\n\r\n",
                       RESOURCE_CASES[i].path, token);
        request_ready = len > 0 && (size_t) len < sizeof(request);
        failures += !request_ready ||
                    !selftest_request(RESOURCE_CASES[i].name, request,
                                      RESOURCE_CASES[i].status,
                                      RESOURCE_CASES[i].error);
        memset(request, 0, sizeof(request));
    }
    memset(token, 0, sizeof(token));

    failures += !selftest_request("CORS preflight is public", OPTIONS, 204,
                                  "Access-Control-Allow-Origin: *");
    failures += !selftest_bodyless_refusal_keeps_connection();
    failures += !selftest_body_refusal_closes_connection();
    ESP_LOGI(TAG, "selftest: %d failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif
