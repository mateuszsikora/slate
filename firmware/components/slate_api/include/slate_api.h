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
#include "slate_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLATE_API_BASE_PATH    "/api/v1"
#define SLATE_SETUP_AP_ADDRESS "192.168.4.1"

/*
 * Every route on the device, plus room to add one without this being the
 * change that breaks something else. §4.1's table and `GET /` came to 31 slots
 * when #37 added three, and the symptom of running out is worth knowing
 * because it is not the one anybody would guess: registration is ordered by
 * component initialisation, so the overflow lands on whichever route was
 * registered *last* — the configuration API's `POST /config/validate` at the
 * time — and reaches the boot log as that component reporting ESP_ERR_NO_MEM.
 * register_route() now names the real cause in a line of its own.
 *
 * The cost of a spare slot is one route_t in .bss and one pointer inside
 * esp_http_server; a few hundred bytes buys a component that cannot silently
 * lose its endpoint.
 */
#define SLATE_API_MAX_URI_HANDLERS 36

/* §4.1's `model`, and the same string §10's editor and mDNS advertise. One
 * board model per binary (ADR-1), so it is a constant rather than a lookup. */
#define SLATE_API_MODEL_ID "waveshare-s3-touch-7"

typedef enum {
    /** No device token. Restricted to GET /info, POST /session and preflight. */
    SLATE_API_AUTH_PUBLIC = 0,

    /** A valid `Authorization: Bearer <device_token>` is mandatory. */
    SLATE_API_AUTH_DEVICE_TOKEN,

    /**
     * Accept either the browser session credential or a named External API
     * key. Registration is restricted to POST /direct/state so a scoped key
     * cannot become a second administrator credential by caller error.
     */
    SLATE_API_AUTH_DEVICE_OR_INTEGRATION,

    /**
     * The token is mandatory except when the request arrived on the setup
     * access point's local address. §4.3 permits this only for the setup page,
     * GET /wifi/scan and POST /wifi; #55 selects it for those handlers.
     */
    SLATE_API_AUTH_SETUP_AP,

    /**
     * The HTTP upgrade is public and a credential is required in the first
     * WebSocket text frame (§4.2). A device-session credential gets the editor
     * channel; an External API key gets only the direct action-consumer flow.
     * Registration is restricted to GET /ws.
     */
    SLATE_API_AUTH_WS_FIRST_FRAME,
} slate_api_auth_t;

/**
 * @brief Which page `GET /` answers with, and on which interface.
 *
 * Two documents put a page at the root and they are both right. §9.2 serves
 * the setup page on the access point, where a phone that has just joined
 * `slate-<mac6>` opens `http://192.168.4.1` and a captive portal probe is
 * redirected to the same place. §10 serves the editor, which is what the
 * editor QR points a browser at once the panel is on a network.
 *
 * One URI can carry one handler, so the choice is made here rather than by
 * whichever component registered last: this file already has to know which
 * interface a request arrived on for §4.3's token exception, and that is the
 * same question.
 */
typedef enum {
    /** §9.2's setup page. Answers only on the access point's own address. */
    SLATE_API_ROOT_SETUP_AP = 0,

    /** §10's editor. Answers on every other interface. */
    SLATE_API_ROOT_EDITOR,
} slate_api_root_t;

/** @brief Start the HTTP server and register GET /info and GET /status. */
esp_err_t slate_api_init(void);

/**
 * @brief Register the page `GET /` serves on one interface.
 *
 * Both root pages are served **without a device token**, and the reason is
 * §4.3's own: a browser cannot put an `Authorization` header on a navigation,
 * and a page it must be told a token to load is a page nobody can open. What
 * that exposes is bounded and unchanged by this — neither document carries
 * device data, and every API route either of them then calls authenticates
 * exactly as it does today.
 *
 * The first registration installs the route; the second fills the other
 * interface. `handler` and `user_ctx` must outlive the server, as
 * esp_http_server requires. A root that nobody registered answers §4's
 * `404 not_found`.
 */
esp_err_t slate_api_register_root(slate_api_root_t which,
                                  esp_err_t (*handler)(httpd_req_t *req),
                                  void *user_ctx);

/**
 * @brief Register an API handler behind the shared auth and CORS layer.
 *
 * Call after slate_api_init(). The URI string and any user_ctx data must
 * remain valid for the lifetime of the server, matching esp_http_server's
 * contract. The wrapped handler receives its original user_ctx unchanged.
 */
esp_err_t slate_api_register_uri(const httpd_uri_t *uri, slate_api_auth_t auth);

/**
 * @brief Append one provider-neutral resource to a JSON array.
 *
 * This is the single serializer for §5.2's public vocabulary. Provider
 * adapters use it for discovery catalogs so upstream field names cannot leak
 * into the editor API.
 */
esp_err_t slate_api_resource_append(cJSON *array, const slate_resource_t *resource);

/** Append a provider's discovery catalog to `array`. */
typedef esp_err_t (*slate_api_resources_append_fn)(void *ctx, cJSON *array);

/**
 * @brief Register the discovery catalog behind `GET /resources` for a provider.
 *
 * Providers without a separate catalog use the normalized bound-resource
 * store automatically. Registration is intended during component init.
 */
esp_err_t slate_api_resources_register(const char *provider,
                                       slate_api_resources_append_fn append,
                                       void *ctx);

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
 * This is a low-level response primitive; route handlers should use
 * slate_api_refuse() or slate_api_refuse_and_close() so the shared connection
 * policy is applied.
 */
esp_err_t slate_api_send_error(httpd_req_t *req, const char *status, const char *error);

/**
 * @brief Refuse a request and apply the shared unread-body policy.
 *
 * Sends the same error document as slate_api_send_error(). A request that
 * declared a body closes its connection after the response so esp_http_server
 * cannot hold the API task while purging bytes the handler did not consume.
 * Body-less requests keep the connection when the response is sent cleanly.
 */
esp_err_t slate_api_refuse(httpd_req_t *req, const char *status, const char *error);

/**
 * @brief Refuse a request and close its connection unconditionally.
 *
 * Use only when a route deliberately abandons the connection after every
 * refusal, including a request that declared no body. The OTA upload uses this
 * because a refused upload is never resumed on its existing connection.
 */
esp_err_t slate_api_refuse_and_close(httpd_req_t *req, const char *status,
                                     const char *error);

#ifdef SLATE_API_SELFTEST

/**
 * @brief Exercise the real loopback HTTP server without exposing the token.
 *
 * Development verifier for #10, called by main only when built with
 * `-DSLATE_API_SELFTEST=1`. It checks public, missing, wrong and correct bearer
 * cases plus CORS preflight, logs only PASS/FAIL and returns ESP_FAIL if any
 * case failed. This is the only test client that can inspect bearer behavior
 * without putting the internal token in a serial log.
 */
esp_err_t slate_api_selftest(void);

#endif

#ifdef __cplusplus
}
#endif
