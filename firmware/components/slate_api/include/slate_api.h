/*
 * Slate — HTTP API v1.
 *
 * design.md ADR-3/ADR-4, §4.1 (routes), §4.3 (authentication).
 *
 * The server and the authentication policy live in one component. Later
 * components register their routes through slate_api_register_uri() rather
 * than reaching into esp_http_server directly, so §11.1's OTA upload cannot
 * accidentally forget the token and #55's setup exception cannot grow beyond
 * the three routes §4.3 names.
 */

#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLATE_API_BASE_PATH        "/api/v1"
#define SLATE_SETUP_AP_ADDRESS     "192.168.4.1"
#define SLATE_API_MAX_URI_HANDLERS 24

typedef enum {
    /** No device token. Intended only for GET /info and CORS preflight. */
    SLATE_API_AUTH_PUBLIC = 0,

    /** A valid `Authorization: Bearer <device_token>` is mandatory. */
    SLATE_API_AUTH_DEVICE_TOKEN,

    /**
     * The token is mandatory except when the request arrived on the setup
     * access point's local address. §4.3 permits this only for the setup page,
     * GET /wifi/scan and POST /wifi; #55 selects it for those handlers.
     */
    SLATE_API_AUTH_SETUP_AP,

    /**
     * The HTTP upgrade is public and the device token is required in the first
     * WebSocket text frame (§4.2). Registration is restricted to GET /ws in the
     * same way the two policies above are restricted to their exact routes.
     */
    SLATE_API_AUTH_WS_FIRST_FRAME,
} slate_api_auth_t;

/** @brief Start the HTTP server and register GET /info and GET /status. */
esp_err_t slate_api_init(void);

/**
 * @brief Register an API handler behind the shared auth and CORS layer.
 *
 * Call after slate_api_init(). The URI string and any user_ctx data must
 * remain valid for the lifetime of the server, matching esp_http_server's
 * contract. The wrapped handler receives its original user_ctx unchanged.
 */
esp_err_t slate_api_register_uri(const httpd_uri_t *uri, slate_api_auth_t auth);

/**
 * @brief Send `root` as the response body, and delete it.
 *
 * Ownership of `root` is taken whatever happens — including when it is NULL,
 * which is how a component reports an allocation that failed part-way through
 * building a document, and which answers 500 `out_of_memory`.
 */
esp_err_t slate_api_send_json(httpd_req_t *req, cJSON *root);

/**
 * @brief Answer `{"error": "<error>"}` with the given HTTP status line.
 *
 * The failure shape is contract (§4, ADR-4), so it is spelled here rather than
 * in each component that registers a route: a client matches on `error` across
 * the whole API instead of on one component's idea of what a failure looks
 * like. `status` is an esp_http_server status line, e.g. "400 Bad Request".
 */
esp_err_t slate_api_send_error(httpd_req_t *req, const char *status, const char *error);

#ifdef SLATE_API_SELFTEST

/**
 * @brief Exercise the real loopback HTTP server without exposing the token.
 *
 * Development verifier for #10, called by main only when built with
 * `-DSLATE_API_SELFTEST=1`. It checks public, missing, wrong and correct bearer
 * cases plus CORS preflight, logs only PASS/FAIL and returns ESP_FAIL if any
 * case failed. The pairing QR in #36 makes external authenticated curl tests
 * possible; until then this is the only test client that can obtain the token
 * without putting it in a serial log.
 */
esp_err_t slate_api_selftest(void);

#endif

#ifdef __cplusplus
}
#endif
