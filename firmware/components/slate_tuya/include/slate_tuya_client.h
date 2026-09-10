/*
 * Tuya OpenAPI transport — the network half of the slate_tuya provider.
 *
 * One signed HTTPS client: region endpoint + credential pair in, signed
 * requests out, with the access-token lifecycle (acquire, refresh at ~80% of
 * lifetime, one retry on token rejection) held inside the handle. No globals,
 * no I/O at create time; the handle is safe to share between tasks because
 * every request takes the handle's mutex — in practice the poller/action task
 * is the only caller, and `slate_tuya_client_test()` builds its own throwaway
 * client rather than touching the live one.
 *
 * The signing rules this lives or dies by are Tuya's "Sign Requests for Cloud
 * Authorization" (developer.tuya.com, doc id Kbw0q34cs2e5g); see the top of
 * slate_tuya_client.c for the exact string-to-sign construction implemented.
 */

#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_client.h" /* HTTP_METHOD_GET/POST/DELETE */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct slate_tuya_client *slate_tuya_client_handle_t;

/* cfg: region, access_id, secret, uid (copied). Does no I/O. */
esp_err_t slate_tuya_client_create(const char *region, const char *access_id,
                                   const char *secret, const char *uid,
                                   slate_tuya_client_handle_t *out);
void slate_tuya_client_destroy(slate_tuya_client_handle_t h);

/* Error classification, so the lifecycle can pick offline vs error (§Lifecycle). */
typedef enum {
    SLATE_TUYA_ERR_NONE = 0,
    SLATE_TUYA_ERR_NETWORK,   /* DNS/TLS/timeout — transient */
    SLATE_TUYA_ERR_AUTH,      /* token rejected, sign invalid — needs a person */
    SLATE_TUYA_ERR_QUOTA,     /* Tuya "permission deny"/trial-expired error codes */
    SLATE_TUYA_ERR_API,       /* other non-success API envelope */
} slate_tuya_error_kind_t;

/* One signed request. path includes the query string. body may be NULL (GET).
 * On ESP_OK *out is a parsed cJSON the caller deletes; *err_kind classifies failure.
 * Envelope {"success":false} maps to the matching kind, never ESP_OK.
 * out_body may be NULL for fire-and-forget. */
esp_err_t slate_tuya_client_request(slate_tuya_client_handle_t h,
                                    int method /*HTTP_METHOD_GET/POST/DELETE*/,
                                    const char *path, const char *body,
                                    cJSON **out_body, slate_tuya_error_kind_t *err_kind);

/* Validates credentials without a full client: fetch token + one device-list page.
 * Used by the POST /tuya route before storing. */
esp_err_t slate_tuya_client_test(const char *region, const char *access_id,
                                 const char *secret, const char *uid,
                                 slate_tuya_error_kind_t *err_kind);

#ifdef SLATE_TUYA_SELFTEST
/* Deterministic protocol fixtures; performs no network I/O. */
esp_err_t slate_tuya_client_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
