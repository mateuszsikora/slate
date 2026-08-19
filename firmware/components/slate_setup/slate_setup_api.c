/*
 * Slate — the setup page and §4.1's three `/wifi` endpoints.
 *
 * design.md §4.1 (the routes), §4.3 (why three of them need no token on the
 * access point), §9.2 (the cached scan, the compiled-in page), §9.3 (why
 * `POST /wifi` cannot answer the question it is asked), §9.6 and §4.1 (the
 * `ipv4` object, both modes of it).
 *
 * This file validates the addressing and hands it on. It does not apply it and
 * cannot report whether it worked: §9.6's trial takes an ARP exchange on an
 * association that does not exist yet, and §9.3's one radio has already dropped
 * the browser by the time there is an answer. slate_wifi owns both.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#include "slate_api.h"
#include "slate_setup_private.h"
#include "slate_store.h"
#include "slate_wifi.h"

static const char *TAG = "setup.api";

/*
 * The page, gzipped at build time and linked in. §9.2: compiled into the
 * firmware rather than served from LittleFS, because it has to work on a device
 * that has never had a filesystem — and a blank filesystem is exactly what a
 * first flash or a bad OTA leaves behind. The 24 KB budget is checked by the
 * build (portal/gzip.cmake), not by anybody remembering to look.
 */
extern const uint8_t _binary_portal_html_gz_start[];
extern const uint8_t _binary_portal_html_gz_end[];

/*
 * Everything `POST /wifi` accepts fits in a fraction of this: a 32-byte SSID,
 * two 63-byte passphrases and §4.1's `ipv4` object. The bound exists so that the
 * one HTTP task cannot be held by a body it is waiting for, and it is generous
 * enough that no legitimate client has to know the number.
 */
#define BODY_MAX 512
#define SETUP_AP_PASSWORD_MIN 8

static bool admin_pin_is_valid(const char *pin)
{
    size_t len = strlen(pin);
    if (len < SLATE_ADMIN_PIN_MIN_LEN || len > SLATE_ADMIN_PIN_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            return false;
        }
    }
    return true;
}

/* lwIP resolves against three servers and ignores the rest, so accepting a
 * fourth would be accepting a value that is silently dropped. Spelled as the
 * store's number rather than as three, because the parsed servers go straight
 * into a slate_ipv4_config_t and the two limits being the same is what makes
 * that safe. */
#define DNS_SERVERS_MAX SLATE_IPV4_DNS_MAX

/* --- GET / -------------------------------------------------------------- */

static esp_err_t page_handler(httpd_req_t *req)
{
    const size_t len = (size_t) (_binary_portal_html_gz_end - _binary_portal_html_gz_start);

    /*
     * Served gzipped unconditionally rather than negotiated. Every browser has
     * sent `Accept-Encoding: gzip` for twenty years, the alternative is carrying
     * both copies in a flash budget §9.2 caps at 24 KB, and the one client that
     * does not is curl — which is a `| gunzip` away and is not who this page is
     * for.
     */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *) _binary_portal_html_gz_start, len);
}

/* --- GET /wifi/scan ----------------------------------------------------- */

static const char *auth_str(wifi_auth_mode_t auth)
{
    /*
     * What the page draws a padlock from, so the distinctions it cannot use are
     * not made: somebody choosing a network is being asked "does this want a
     * password", not which key exchange it prefers. `enterprise` is named
     * separately for the opposite reason — it also wants a username, and this
     * page has no field for one, so a padlock that looked ordinary there would
     * cost a real quarter of an hour.
     */
    switch (auth) {
    /* OWE is opportunistic encryption with no passphrase to type, so from this
     * page's point of view it is an open network and saying anything else would
     * put a password field in front of somebody who has none. */
    case WIFI_AUTH_OPEN:
    case WIFI_AUTH_OWE:          return "open";
    case WIFI_AUTH_WEP:          return "wep";
    case WIFI_AUTH_WPA_PSK:      return "wpa";
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return "wpa3";
    case WIFI_AUTH_ENTERPRISE: /* WIFI_AUTH_WPA2_ENTERPRISE is the same value */
    case WIFI_AUTH_WPA_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192: return "enterprise";
    /* Everything else — WAPI, DPP, whatever arrives next — is a network that
     * wants something, and the page treats anything that is not `open` as
     * wanting a passphrase. That is the safe direction to be vague in. */
    default:                     return "unknown";
    }
}

static bool query_flag_is_set(httpd_req_t *req, const char *key)
{
    char query[64];
    char value[8];

    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, key, value, sizeof(value)) == ESP_OK &&
           strcmp(value, "1") == 0;
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    /*
     * §9.2 does not sweep per request — a sweep makes the access point
     * unresponsive for its duration, which the browser waiting for this response
     * experiences as the panel having crashed. So the cache is what is served,
     * and `?rescan=1` is the page's refresh button, which says it takes a few
     * seconds before it is pressed.
     */
    if (query_flag_is_set(req, "rescan")) {
        esp_err_t err = slate_setup_scan();
        if (err != ESP_OK) {
            /* Not a failure of this request. The driver refuses a sweep while
             * the station is mid-association, which is the normal state of a
             * panel that has an access point up — and a stale list with a
             * manual SSID field beside it is a working page. */
            ESP_LOGI(TAG, "serving the cached sweep: %s", esp_err_to_name(err));
        }
    }

    slate_setup_network_t *networks = calloc(SLATE_SETUP_SCAN_MAX, sizeof(*networks));
    if (networks == NULL) {
        return slate_api_send_json(req, NULL);
    }

    int64_t age_us = -1;
    size_t count = slate_setup_scan_copy(networks, &age_us);

    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(root, "networks");
    bool ok = list != NULL;

    for (size_t i = 0; ok && i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        ok = entry != NULL && cJSON_AddItemToArray(list, entry);
        if (!ok) {
            cJSON_Delete(entry);
            break;
        }
        ok = cJSON_AddStringToObject(entry, "ssid", networks[i].ssid) != NULL &&
             cJSON_AddNumberToObject(entry, "rssi", networks[i].rssi) != NULL &&
             cJSON_AddNumberToObject(entry, "channel", networks[i].channel) != NULL &&
             cJSON_AddStringToObject(entry, "auth", auth_str(networks[i].auth)) != NULL;
    }
    free(networks);

    /*
     * How old the list is, because §9.2 serves a cache and a client cannot
     * otherwise tell a sweep taken on the way up from one taken an hour ago.
     * `null` says there has not been a sweep at all, which is a different thing
     * from an empty room and the page says so.
     */
    if (ok) {
        ok = age_us < 0 ? cJSON_AddNullToObject(root, "age_s") != NULL
                        : cJSON_AddNumberToObject(root, "age_s", age_us / 1000000) != NULL;
    }
    if (!ok) {
        cJSON_Delete(root);
        root = NULL;
    }
    return slate_api_send_json(req, root);
}

/* --- POST /wifi: the `ipv4` object -------------------------------------- */

/**
 * Parse a dotted quad strictly, into host byte order.
 *
 * Strictly, because lwIP's own `ip4addr_aton()` is not: it accepts `192.168.1`
 * and `0xc0.0xa8.1.1`, and a validator that accepts shorthand turns a typed
 * address into a different one without saying so. This is the front door for a
 * value that #63 will make the panel unreachable with if it is wrong.
 */
static bool parse_address(const char *text, uint32_t *out)
{
    uint32_t value = 0;

    for (int octet = 0; octet < 4; octet++) {
        unsigned digits = 0;
        unsigned number = 0;

        while (*text >= '0' && *text <= '9' && digits < 3) {
            number = number * 10 + (unsigned) (*text - '0');
            text++;
            digits++;
        }
        if (digits == 0 || number > 255 || (*text >= '0' && *text <= '9')) {
            return false;
        }

        value = value << 8 | number;

        if (octet < 3) {
            if (*text != '.') {
                return false;
            }
            text++;
        }
    }

    if (*text != '\0') {
        return false;
    }
    *out = value;
    return true;
}

/** `192.168.1.42/24` — the address and the prefix that gives it a subnet. */
static bool parse_cidr(const char *text, uint32_t *address, unsigned *prefix)
{
    const char *slash = strchr(text, '/');
    char host[16];

    if (slash == NULL || (size_t) (slash - text) >= sizeof(host)) {
        return false;
    }
    memcpy(host, text, (size_t) (slash - text));
    host[slash - text] = '\0';

    if (!parse_address(host, address)) {
        return false;
    }

    unsigned digits = 0;
    unsigned value = 0;
    for (const char *at = slash + 1; *at != '\0'; at++) {
        if (*at < '0' || *at > '9' || digits >= 2) {
            return false;
        }
        value = value * 10 + (unsigned) (*at - '0');
        digits++;
    }
    if (digits == 0) {
        return false;
    }

    *prefix = value;
    return true;
}

static const char *string_field(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/** An optional string distinguishes absence/null from a present value of the wrong type. */
static bool optional_string_field(const cJSON *object, const char *name, const char **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    *out = NULL;

    if (item == NULL || cJSON_IsNull(item)) {
        return true;
    }
    if (!cJSON_IsString(item)) {
        return false;
    }

    *out = item->valuestring;
    return true;
}

/** Wipe parsed string values before cJSON returns their allocations to the heap. */
static void clear_json_strings(cJSON *item)
{
    for (cJSON *current = item; current != NULL; current = current->next) {
        clear_json_strings(current->child);
        if (current->valuestring != NULL) {
            explicit_bzero(current->valuestring, strlen(current->valuestring));
        }
    }
}

/**
 * Validate §4.1's `ipv4` object and parse what the station will apply.
 *
 * @return NULL if it is coherent, otherwise the `error` string to answer with.
 *
 * `out->state` reports the mode asked for — SLATE_IPV4_DHCP or, for `static`,
 * SLATE_IPV4_STATIC_PENDING, which is the state §9.6 says a freshly submitted
 * configuration starts in. The address fields are parsed whichever mode it is,
 * because the checks run before fields alongside `"dhcp"` are refused: a bad
 * address still gets its specific error rather than being silently ignored.
 *
 * The checks are the cheap ones that catch a typo before it can cost anything:
 * a prefix between 8 and 30, an address that is a host on the subnet its own
 * prefix implies rather than that subnet's network or broadcast address, and a
 * gateway on the same subnet. What they cannot catch — a gateway that is a
 * perfectly good host address and simply is not the router — is what the trial
 * of §9.6 is for.
 */
static const char *validate_ipv4(const cJSON *ipv4, slate_ipv4_config_t *out)
{
    memset(out, 0, sizeof(*out));

    /* §4.1: "An absent `ipv4` means `dhcp`." That default is what makes the
     * field addable without a version, so absent and null both mean the same
     * thing and neither is an error. */
    if (ipv4 == NULL || cJSON_IsNull(ipv4)) {
        return NULL;
    }
    if (!cJSON_IsObject(ipv4)) {
        return "bad_ipv4";
    }

    bool is_static = false;
    const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(ipv4, "mode");
    if (mode_item != NULL && !cJSON_IsNull(mode_item)) {
        const char *mode = cJSON_IsString(mode_item) ? mode_item->valuestring : NULL;
        if (mode == NULL || (strcmp(mode, "dhcp") != 0 && strcmp(mode, "static") != 0)) {
            return "bad_ipv4_mode";
        }
        is_static = strcmp(mode, "static") == 0;
    }

    const char *address_text = NULL;
    const char *gateway_text = NULL;
    if (!optional_string_field(ipv4, "address", &address_text)) {
        return "bad_address";
    }
    if (!optional_string_field(ipv4, "gateway", &gateway_text)) {
        return "bad_gateway";
    }

    if (is_static && (address_text == NULL || gateway_text == NULL)) {
        /* A static configuration without both of these is not one. §9.6 resolves
         * the gateway over ARP to decide whether to keep the address at all, so
         * there is nothing for it to prove without one. */
        return address_text == NULL ? "bad_address" : "bad_gateway";
    }

    uint32_t address = 0;
    uint32_t mask = 0;
    if (address_text != NULL) {
        unsigned prefix = 0;
        if (!parse_cidr(address_text, &address, &prefix) || prefix < 8 || prefix > 30) {
            return "bad_address";
        }

        mask = ~0u << (32 - prefix);
        uint32_t host = address & ~mask;
        if (host == 0 || host == (~mask & 0xFFFFFFFFu)) {
            /* The network and broadcast addresses of the subnet the client just
             * described. Both associate perfectly and answer nothing. */
            return "bad_address";
        }

        out->address = address;
        out->prefix = (uint8_t) prefix;
    }

    if (gateway_text != NULL) {
        uint32_t gateway = 0;
        if (!parse_address(gateway_text, &gateway)) {
            return "bad_gateway";
        }
        if (address_text != NULL) {
            uint32_t gateway_host = gateway & ~mask;
            if ((gateway & mask) != (address & mask) || gateway_host == 0 ||
                gateway_host == (~mask & 0xFFFFFFFFu) || gateway == address) {
                /* A gateway has to be another host on this subnet. Its network
                 * and broadcast addresses are syntactically valid dotted quads,
                 * but neither can answer the ARP proof §9.6 requires; the panel's
                 * own address cannot be its next hop either. */
                return "bad_gateway";
            }
        }
        out->gateway = gateway;
    }

    const cJSON *dns = cJSON_GetObjectItemCaseSensitive(ipv4, "dns");
    if (dns != NULL && !cJSON_IsNull(dns)) {
        if (!cJSON_IsArray(dns) || cJSON_GetArraySize(dns) > DNS_SERVERS_MAX) {
            return "bad_dns";
        }
        const cJSON *server = NULL;
        cJSON_ArrayForEach(server, dns) {
            uint32_t resolver = 0;
            if (!cJSON_IsString(server) || !parse_address(server->valuestring, &resolver)) {
                return "bad_dns";
            }
            out->dns[out->dns_count++] = resolver;
        }
    }

    /* DHCP is the absence of a static configuration, not a mode that makes its
     * fields inert. Accepting a coherent address here would be worse than only
     * accepting a malformed one: a client typo would receive 202 even though
     * the panel discarded exactly the setting it meant to change. Validate
     * first, above, so a malformed field keeps the most useful error. */
    if (!is_static) {
        if (address_text != NULL) {
            return "bad_address";
        }
        if (gateway_text != NULL) {
            return "bad_gateway";
        }
        if (dns != NULL && !cJSON_IsNull(dns)) {
            return "bad_dns";
        }
    }

    /* Last, so that everything above returns with the state still DHCP and a
     * caller cannot act on a half-parsed static configuration. */
    if (is_static) {
        out->state = SLATE_IPV4_STATIC_PENDING;
    }
    return NULL;
}

/* --- POST /wifi --------------------------------------------------------- */

/** Read the whole body, or say why not. NULL on success. */
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

static esp_err_t wifi_set_handler(httpd_req_t *req)
{
    char body[BODY_MAX];
    char accepted_ssid[SLATE_WIFI_SSID_BUF_LEN] = {0};
    const char *problem = read_body(req, body, sizeof(body));
    if (problem != NULL) {
        return slate_api_refuse(req,
                                strcmp(problem, "too_large") == 0 ? "413 Payload Too Large"
                                                                  : "400 Bad Request",
                                problem);
    }

    cJSON *root = cJSON_Parse(body);
    /* The passphrase was in this buffer; §12 keeps it out of everything that is
     * not NVS, and a task stack is where §11.3's core dump would find it. */
    explicit_bzero(body, sizeof(body));

    if (!cJSON_IsObject(root)) {
        clear_json_strings(root);
        cJSON_Delete(root);
        return slate_api_refuse(req, "400 Bad Request", "invalid_json");
    }

    const char *ssid = string_field(root, "ssid");
    const char *password = NULL;
    const char *setup_password = NULL;
    const char *admin_pin = NULL;
    const char *error = NULL;
    slate_ipv4_config_t ipv4 = {0};

    if (ssid == NULL || ssid[0] == '\0') {
        error = "ssid_required";
    } else if (strlen(ssid) >= SLATE_WIFI_SSID_BUF_LEN) {
        error = "ssid_too_long";
    } else if (!optional_string_field(root, "password", &password)) {
        error = "invalid_json";
    } else if (password != NULL && strlen(password) >= SLATE_WIFI_PASSWORD_BUF_LEN) {
        error = "password_too_long";
    } else if (!optional_string_field(root, "setup_password", &setup_password)) {
        error = "invalid_json";
    } else if (setup_password != NULL && setup_password[0] != '\0' &&
               strlen(setup_password) < SETUP_AP_PASSWORD_MIN) {
        error = "setup_password_too_short";
    } else if (setup_password != NULL &&
               strlen(setup_password) >= SLATE_WIFI_PASSWORD_BUF_LEN) {
        error = "setup_password_too_long";
    } else if (!optional_string_field(root, "admin_pin", &admin_pin)) {
        error = "invalid_json";
    } else if (admin_pin != NULL && admin_pin[0] != '\0' &&
               !admin_pin_is_valid(admin_pin)) {
        error = "admin_pin_invalid";
    }

    if (error == NULL) {
        error = validate_ipv4(cJSON_GetObjectItemCaseSensitive(root, "ipv4"), &ipv4);
    }

    char next_setup_password[SLATE_WIFI_PASSWORD_BUF_LEN] = {0};
    char previous_setup_password[SLATE_WIFI_PASSWORD_BUF_LEN] = {0};
    char next_admin_pin[SLATE_ADMIN_PIN_MAX_LEN + 1] = {0};
    const bool update_setup_password = error == NULL && setup_password != NULL;
    const bool update_admin_pin = error == NULL && admin_pin != NULL;
    if (update_setup_password) {
        strlcpy(next_setup_password, setup_password, sizeof(next_setup_password));
    }
    if (update_admin_pin) {
        strlcpy(next_admin_pin, admin_pin, sizeof(next_admin_pin));
    }

    esp_err_t err = ESP_OK;
    bool previous_setup_password_set = false;
    bool setup_password_written = false;
    bool wifi_write_attempted = false;
    if (error == NULL) {
        /* `ssid` belongs to `root`, which is deleted before the response is
         * built. Keep the accepted value rather than borrowing freed cJSON
         * storage on the success path. */
        strlcpy(accepted_ssid, ssid, sizeof(accepted_ssid));

        if (update_setup_password) {
            esp_err_t read_err = slate_store_str_get(SLATE_KEY_SETUP_AP_PASS,
                                                     previous_setup_password,
                                                     sizeof(previous_setup_password));
            previous_setup_password_set = read_err == ESP_OK;
            if (read_err != ESP_OK && read_err != ESP_ERR_NOT_FOUND) {
                err = read_err;
            } else {
                err = next_setup_password[0] == '\0'
                          ? slate_store_erase(SLATE_KEY_SETUP_AP_PASS)
                          : slate_store_str_set(SLATE_KEY_SETUP_AP_PASS, next_setup_password);
                setup_password_written = err == ESP_OK;
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "storing setup access point security: %s",
                         esp_err_to_name(err));
            }
        }

        if (err == ESP_OK) {
            wifi_write_attempted = true;
            err = slate_wifi_connect(ssid, password, &ipv4);
        }
        if (wifi_write_attempted && err != ESP_OK) {
            ESP_LOGE(TAG, "storing credentials for \"%s\": %s", ssid, esp_err_to_name(err));

            /* Keep the two settings one operation from the page's point of
             * view. The running AP is unchanged until it is raised again, so a
             * failed station write can put its previous NVS value back safely. */
            if (setup_password_written) {
                esp_err_t rollback_err = previous_setup_password_set
                                             ? slate_store_str_set(SLATE_KEY_SETUP_AP_PASS,
                                                                   previous_setup_password)
                                             : slate_store_erase(SLATE_KEY_SETUP_AP_PASS);
                if (rollback_err != ESP_OK) {
                    ESP_LOGE(TAG, "restoring setup access point security: %s",
                             esp_err_to_name(rollback_err));
                }
            }
        }

        if (err == ESP_OK && update_admin_pin) {
            err = next_admin_pin[0] == '\0' ? slate_store_admin_pin_clear()
                                               : slate_store_admin_pin_set(next_admin_pin);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "storing web editor PIN: %s", esp_err_to_name(err));
            } else {
                /* Invalidate credentials exposed by firmware versions whose QR
                 * contained the token, and any already-open editor sessions. */
                err = slate_store_device_token_reissue();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "rotating the internal session credential: %s",
                             esp_err_to_name(err));
                }
            }
        }
    }

    /* cJSON parses strings into separate heap allocations. Clearing only `body`
     * leaves the passphrase in a freed heap block, so wipe that copy before the
     * tree gives the allocation back. Do this on refusals too: an invalid SSID or
     * IPv4 object does not make the password beside it less secret. */
    clear_json_strings(root);
    cJSON_Delete(root);
    explicit_bzero(next_setup_password, sizeof(next_setup_password));
    explicit_bzero(previous_setup_password, sizeof(previous_setup_password));
    explicit_bzero(next_admin_pin, sizeof(next_admin_pin));

    if (error != NULL) {
        ESP_LOGW(TAG, "refused: %s", error);
        return slate_api_refuse(req, "400 Bad Request", error);
    }
    if (err != ESP_OK) {
        return slate_api_refuse(req, "500 Internal Server Error", "store_failed");
    }

    /*
     * 202, because the question the caller is really asking cannot be answered
     * here and this is the status that says so. §9.3: there is one radio, the
     * access point follows the station's channel, and at the instant the station
     * associates every client attached to the access point is dropped —
     * including the browser waiting for this response. The credentials are
     * stored and are being applied; the outcome appears on the panel and in
     * `GET /info.network`.
     *
     * The body echoes what was accepted rather than what is now true. That is
     * the opposite of `/info.network.ipv4`, which §4.1 makes a report of where
     * the address actually came from — and the two differ precisely because this
     * one is an acknowledgement and that one is an observation. With a static
     * configuration the gap between them is the point: this says `static` the
     * moment it is stored, and §9.6 may still have reverted it to `dhcp` fifteen
     * seconds later.
     */
    cJSON *response = cJSON_CreateObject();
    cJSON *echo = cJSON_AddObjectToObject(response, "ipv4");
    bool ok = echo != NULL &&
              cJSON_AddStringToObject(echo, "mode",
                                      ipv4.state == SLATE_IPV4_DHCP ? "dhcp" : "static") != NULL &&
              cJSON_AddStringToObject(response, "ssid", accepted_ssid) != NULL;
    if (!ok) {
        cJSON_Delete(response);
        response = NULL;
    }

    httpd_resp_set_status(req, "202 Accepted");
    return slate_api_send_json(req, response);
}

/* --- DELETE /wifi ------------------------------------------------------- */

static esp_err_t wifi_forget_handler(httpd_req_t *req)
{
    /*
     * §9.5: this is one of the two ways back to setup, and it is the one that
     * works from a desk. The reply travels on a connection that is about to stop
     * being reachable — the station is being disconnected as it goes out — and
     * the panel comes up on its own access point with the address on the screen.
     * That is the intended behaviour and not a failure to report anything.
     *
     * It carries the device token (§4.3 does not extend the access point's
     * exception to it): the panel is on a network and reachable, so whoever is
     * asking has had the chance to open an editor session, and moving somebody's panel off
     * their WiFi is not something an unauthenticated request gets to do.
     */
    esp_err_t err = slate_wifi_forget();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "forgetting the credentials: %s", esp_err_to_name(err));
        return slate_api_refuse(req, "500 Internal Server Error", "store_failed");
    }

    return slate_api_send_json(req, cJSON_CreateObject());
}

/* --- Registration ------------------------------------------------------- */

esp_err_t slate_setup_api_init(void)
{
    /*
     * §4.3's exception, and it is narrow on purpose: the page, the sweep and the
     * submission are served without a token on the access point interface only,
     * and the two endpoints answer 401 on the station exactly as everything else
     * does. #61's register function enforces the list — SLATE_API_AUTH_SETUP_AP
     * is refused for any other route — so this cannot grow by a component adding
     * a handler.
     *
     * The page is not in that list any more. `GET /` is claimed by §9.2 here and
     * by §10's editor, so slate_api owns the route and picks between them by the
     * interface a request arrived on (#31): this page on the access point's own
     * address, the editor everywhere else. Nothing about what is served where
     * changed — the station never saw this page.
     */
    static const httpd_uri_t scan = {
        .uri = SLATE_API_BASE_PATH "/wifi/scan",
        .method = HTTP_GET,
        .handler = scan_handler,
    };
    static const httpd_uri_t set = {
        .uri = SLATE_API_BASE_PATH "/wifi",
        .method = HTTP_POST,
        .handler = wifi_set_handler,
    };
    static const httpd_uri_t forget = {
        .uri = SLATE_API_BASE_PATH "/wifi",
        .method = HTTP_DELETE,
        .handler = wifi_forget_handler,
    };

    /* The page is the access point's half of `GET /`; #31's editor is the
     * other half, and slate_api decides between them by the interface a
     * request arrived on rather than by which of the two registered first. */
    esp_err_t err = slate_api_register_root(SLATE_API_ROOT_SETUP_AP, page_handler, NULL);
    if (err == ESP_OK) {
        err = slate_api_register_uri(&scan, SLATE_API_AUTH_SETUP_AP);
    }
    if (err == ESP_OK) {
        err = slate_api_register_uri(&set, SLATE_API_AUTH_SETUP_AP);
    }
    if (err == ESP_OK) {
        err = slate_api_register_uri(&forget, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "setup page is %u B gzipped",
             (unsigned) (_binary_portal_html_gz_end - _binary_portal_html_gz_start));
    return ESP_OK;
}

#ifdef SLATE_SETUP_SELFTEST

/* --- Development verifier ----------------------------------------------- */

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

/** One request to one of the device's own addresses, checked by status and text. */
static bool expect(const char *name, const char *address, const char *request,
                   int expected_status, const char *expected_text)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "selftest: %-46s FAIL (socket)", name);
        return false;
    }

    /*
     * A second is four orders of magnitude more than a request that never leaves
     * the device needs, and it is a timeout rather than a read-until-EOF because
     * the responses that keep their connection alive — every refusal with no body
     * — would otherwise be waited on for nothing. It is what bounds this whole
     * verifier to a few seconds of the boot it runs in.
     */
    const struct timeval timeout = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    const struct sockaddr_in target = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
        .sin_addr.s_addr = esp_ip4addr_aton(address),
    };

    /* Only the head of the response is read. The page is 5 KB of gzip and this
     * is checking a status line and a header, so the body is deliberately left
     * in the socket and the connection dropped. */
    char response[512];
    size_t used = 0;
    bool ok = connect(fd, (const struct sockaddr *) &target, sizeof(target)) == 0 &&
              send_all(fd, request, strlen(request));
    while (ok && used + 1 < sizeof(response)) {
        int received = recv(fd, response + used, sizeof(response) - used - 1, 0);
        if (received <= 0) {
            break;
        }
        used += (size_t) received;
    }
    close(fd);
    response[used] = '\0';

    const char *space = strchr(response, ' ');
    int status = space != NULL ? atoi(space + 1) : 0;
    ok = ok && status == expected_status && strstr(response, expected_text) != NULL;

    ESP_LOGI(TAG, "selftest: %-46s %s", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        ESP_LOGW(TAG, "selftest:   wanted %d and \"%s\", got %d", expected_status, expected_text,
                 status);
    }
    return ok;
}

/** A POST of `body`, with the Content-Length counted rather than written down. */
static bool expect_post(const char *name, const char *address, const char *body,
                        int expected_status, const char *expected_text)
{
    char request[512];
    int len = snprintf(request, sizeof(request),
                       "POST /api/v1/wifi HTTP/1.0\r\nHost: %s\r\n"
                       "Content-Type: application/json\r\nContent-Length: %u\r\n"
                       "Connection: close\r\n\r\n%s",
                       address, (unsigned) strlen(body), body);
    if (len < 0 || (size_t) len >= sizeof(request)) {
        ESP_LOGE(TAG, "selftest: %-46s FAIL (request does not fit)", name);
        return false;
    }
    return expect(name, address, request, expected_status, expected_text);
}

/**
 * The captive responder, end to end: a real query on the wire for a name that
 * exists, and the answer has to be the panel rather than the truth.
 */
static bool expect_dns_answer(void)
{
    /* `captive.apple.com`, hand-encoded, QTYPE A, QCLASS IN. Written out because
     * this is the exact packet iOS sends and building it from a string would be a
     * DNS encoder written to test a DNS answerer. */
    static const uint8_t QUERY[] = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        7,    'c',  'a',  'p',  't',  'i',  'v',  'e',
        5,    'a',  'p',  'p',  'l',  'e',
        3,    'c',  'o',  'm',
        0x00, 0x00, 0x01, 0x00, 0x01,
    };

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "selftest: %-46s FAIL (socket)", "captive DNS answers with the panel");
        return false;
    }

    const struct timeval timeout = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    const struct sockaddr_in server = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = esp_ip4addr_aton(SLATE_SETUP_AP_ADDRESS),
    };

    uint8_t answer[128] = {0};
    bool ok = sendto(fd, QUERY, sizeof(QUERY), 0, (const struct sockaddr *) &server,
                     sizeof(server)) == (int) sizeof(QUERY);
    int received = ok ? recv(fd, answer, sizeof(answer), 0) : -1;
    close(fd);

    /* Header, question, and a sixteen-byte answer whose last four bytes are the
     * address. Checked as the whole shape rather than by searching for the
     * address: four bytes that happen to appear somewhere in a response are not
     * an A record. */
    uint32_t expected = esp_ip4addr_aton(SLATE_SETUP_AP_ADDRESS);
    ok = ok && received == (int) sizeof(QUERY) + 16 && (answer[2] & 0x80) != 0 &&
         answer[7] == 1 && memcmp(answer + received - 4, &expected, 4) == 0;

    ESP_LOGI(TAG, "selftest: %-46s %s", "captive DNS answers with the panel", ok ? "PASS" : "FAIL");
    if (!ok) {
        ESP_LOGW(TAG, "selftest:   %d byte(s) back, wanted %d", received,
                 (int) sizeof(QUERY) + 16);
    }
    return ok;
}

esp_err_t slate_setup_selftest(void)
{
    static const char PAGE[] = "GET / HTTP/1.0\r\nHost: " SLATE_SETUP_AP_ADDRESS
                               "\r\nConnection: close\r\n\r\n";
    static const char SCAN[] = "GET /api/v1/wifi/scan HTTP/1.0\r\nHost: " SLATE_SETUP_AP_ADDRESS
                               "\r\nConnection: close\r\n\r\n";
    static const char INFO[] = "GET /api/v1/info HTTP/1.0\r\nHost: " SLATE_SETUP_AP_ADDRESS
                               "\r\nConnection: close\r\n\r\n";
    static const char STATUS[] = "GET /api/v1/status HTTP/1.0\r\nHost: " SLATE_SETUP_AP_ADDRESS
                                 "\r\nConnection: close\r\n\r\n";
    static const char PROBE[] = "GET /hotspot-detect.html HTTP/1.0\r\nHost: captive.apple.com"
                                "\r\nConnection: close\r\n\r\n";

    /*
     * Every body below is a refusal, so nothing here can change what network the
     * panel is trying to join. That rules out the one case worth the most —
     * a coherent static configuration being accepted — because accepting it is
     * exactly what it would do, on a device whose real credentials are in the
     * same two keys. §9.6's happy path is verified on hardware against a real
     * router, and there is no honest way to do it from in here.
     */
    static const char STATIC_BAD_DNS[] =
        "{\"ssid\":\"selftest\",\"password\":\"selftest\",\"ipv4\":{\"mode\":\"static\","
        "\"address\":\"192.168.1.42/24\",\"gateway\":\"192.168.1.1\",\"dns\":[\"192.168.1\"]}}";
    static const char STATIC_NO_GATEWAY[] =
        "{\"ssid\":\"selftest\",\"ipv4\":{\"mode\":\"static\",\"address\":\"192.168.1.42/24\"}}";
    static const char STATIC_BAD_GATEWAY[] =
        "{\"ssid\":\"selftest\",\"password\":\"selftest\",\"ipv4\":{\"mode\":\"static\","
        "\"address\":\"192.168.1.42/24\",\"gateway\":\"10.0.0.1\"}}";
    static const char STATIC_NETWORK_GATEWAY[] =
        "{\"ssid\":\"selftest\",\"ipv4\":{\"mode\":\"static\","
        "\"address\":\"192.168.1.42/24\",\"gateway\":\"192.168.1.0\"}}";
    static const char STATIC_BROADCAST_GATEWAY[] =
        "{\"ssid\":\"selftest\",\"ipv4\":{\"mode\":\"static\","
        "\"address\":\"192.168.1.42/24\",\"gateway\":\"192.168.1.255\"}}";
    static const char STATIC_SELF_GATEWAY[] =
        "{\"ssid\":\"selftest\",\"ipv4\":{\"mode\":\"static\","
        "\"address\":\"192.168.1.42/24\",\"gateway\":\"192.168.1.42\"}}";
    static const char BAD_PASSWORD_TYPE[] =
        "{\"ssid\":\"selftest\",\"password\":false}";
    static const char BAD_SETUP_PASSWORD_TYPE[] =
        "{\"ssid\":\"selftest\",\"setup_password\":false}";
    static const char SHORT_SETUP_PASSWORD[] =
        "{\"ssid\":\"selftest\",\"setup_password\":\"short\"}";
    static const char BAD_ADMIN_PIN_TYPE[] =
        "{\"ssid\":\"selftest\",\"admin_pin\":1234}";
    static const char BAD_ADMIN_PIN[] =
        "{\"ssid\":\"selftest\",\"admin_pin\":\"12ab\"}";
    static const char BAD_ADDRESS_TYPE[] =
        "{\"ssid\":\"selftest\",\"ipv4\":{\"mode\":\"dhcp\",\"address\":42}}";
    static const char BAD_GATEWAY_TYPE[] =
        "{\"ssid\":\"selftest\",\"ipv4\":{\"mode\":\"dhcp\",\"gateway\":false}}";
    static const char DHCP_WITH_ADDRESS[] =
        "{\"ssid\":\"selftest\",\"ipv4\":{\"mode\":\"dhcp\","
        "\"address\":\"192.168.1.42/24\",\"gateway\":\"192.168.1.1\"}}";
    static const char DHCP_WITH_DNS[] =
        "{\"ssid\":\"selftest\",\"ipv4\":{\"mode\":\"dhcp\",\"dns\":[\"192.168.1.1\"]}}";

    int failures = 0;

    /* §4.3's exception: three routes and the page, on this interface only. */
    failures += !expect("the page needs no token on the AP", SLATE_SETUP_AP_ADDRESS, PAGE, 200,
                        "Content-Encoding: gzip");
    failures += !expect("GET /wifi/scan needs no token on the AP", SLATE_SETUP_AP_ADDRESS, SCAN,
                        200, "\"networks\":");
    failures += !expect_post("POST /wifi needs no token on the AP", SLATE_SETUP_AP_ADDRESS,
                             STATIC_BAD_DNS, 400, "\"error\":\"bad_dns\"");
    failures += !expect_post("a static address needs a gateway", SLATE_SETUP_AP_ADDRESS,
                             STATIC_NO_GATEWAY, 400, "\"error\":\"bad_gateway\"");
    failures += !expect_post("a gateway on another subnet is refused", SLATE_SETUP_AP_ADDRESS,
                             STATIC_BAD_GATEWAY, 400, "\"error\":\"bad_gateway\"");
    failures += !expect_post("the network address is not a gateway", SLATE_SETUP_AP_ADDRESS,
                             STATIC_NETWORK_GATEWAY, 400, "\"error\":\"bad_gateway\"");
    failures += !expect_post("the broadcast address is not a gateway", SLATE_SETUP_AP_ADDRESS,
                             STATIC_BROADCAST_GATEWAY, 400, "\"error\":\"bad_gateway\"");
    failures += !expect_post("the panel is not its own gateway", SLATE_SETUP_AP_ADDRESS,
                             STATIC_SELF_GATEWAY, 400, "\"error\":\"bad_gateway\"");
    failures += !expect_post("a non-string password is not treated as absent",
                             SLATE_SETUP_AP_ADDRESS, BAD_PASSWORD_TYPE, 400,
                             "\"error\":\"invalid_json\"");
    failures += !expect_post("a non-string setup password is refused",
                             SLATE_SETUP_AP_ADDRESS, BAD_SETUP_PASSWORD_TYPE, 400,
                             "\"error\":\"invalid_json\"");
    failures += !expect_post("a short setup password is refused",
                             SLATE_SETUP_AP_ADDRESS, SHORT_SETUP_PASSWORD, 400,
                             "\"error\":\"setup_password_too_short\"");
    failures += !expect_post("a non-string administrator PIN is refused",
                             SLATE_SETUP_AP_ADDRESS, BAD_ADMIN_PIN_TYPE, 400,
                             "\"error\":\"invalid_json\"");
    failures += !expect_post("an administrator PIN contains only 4-12 digits",
                             SLATE_SETUP_AP_ADDRESS, BAD_ADMIN_PIN, 400,
                             "\"error\":\"admin_pin_invalid\"");
    failures += !expect_post("a non-string address is refused", SLATE_SETUP_AP_ADDRESS,
                             BAD_ADDRESS_TYPE, 400, "\"error\":\"bad_address\"");
    failures += !expect_post("a non-string gateway is refused", SLATE_SETUP_AP_ADDRESS,
                             BAD_GATEWAY_TYPE, 400, "\"error\":\"bad_gateway\"");
    failures += !expect_post("DHCP does not silently discard a static address",
                             SLATE_SETUP_AP_ADDRESS, DHCP_WITH_ADDRESS, 400,
                             "\"error\":\"bad_address\"");
    failures += !expect_post("DHCP does not silently discard static DNS",
                             SLATE_SETUP_AP_ADDRESS, DHCP_WITH_DNS, 400,
                             "\"error\":\"bad_dns\"");

    /* And nothing else. §12: "a panel moved to a hostile network still holds
     * every secret behind a token that only the screen has shown." */
    failures += !expect("GET /status still needs a token on the AP", SLATE_SETUP_AP_ADDRESS,
                        STATUS, 401, "\"error\":\"unauthorized\"");
    failures += !expect("the exception is the interface, not the route", "127.0.0.1", SCAN, 401,
                        "\"error\":\"unauthorized\"");

    failures += !expect("GET /info reports ap mode", SLATE_SETUP_AP_ADDRESS, INFO, 200,
                        "\"mode\":\"ap\"");

    /* §9.2's portal, both halves: the name resolves to the panel and the probe
     * path it then fetches lands on the page rather than on a 404. */
    failures += !expect_dns_answer();
    failures += !expect("a portal probe is redirected to the page", SLATE_SETUP_AP_ADDRESS, PROBE,
                        302, "Location: http://" SLATE_SETUP_AP_ADDRESS "/");

    ESP_LOGI(TAG, "selftest: %d failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif
