/*
 * slate_tuya_client.c — see include/slate_tuya_client.h.
 *
 * Signing, per Tuya's "Sign Requests for Cloud Authorization"
 * (https://developer.tuya.com/en/docs/iot/new-singnature?id=Kbw0q34cs2e5g,
 * verified 2026-09-10; the older doc id Ka43a5mtx1gsc agrees):
 *
 *   stringToSign = METHOD "\n" SHA256_HEX(body) "\n" HEADERS "\n" URL
 *   token calls:  str = client_id + t + stringToSign
 *   other calls:  str = client_id + access_token + t + stringToSign
 *   sign = uppercase hex of HMAC-SHA256(str, key = secret)
 *
 * HEADERS is empty for this client: it sends no Signature-Headers, so no
 * header participates in the signature, and the empty HEADERS is exactly the
 * blank line the docs' FAQ warns about ("Why does a blank line exist in
 * stringToSign?"). nonce is documented as optional and is omitted. URL is the
 * request path with its query parameters re-sorted by key in byte order —
 * only the signed copy is sorted; the request on the wire keeps the caller's
 * order. SHA256_HEX of an empty body is still computed (e3b0c442…b855), per
 * the docs' note.
 */

#include "slate_tuya_client.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include "slate_store.h" /* SLATE_TUYA_*_MAX_LEN — the ceilings the route and store enforce */

static const char *TAG = "slate_tuya_client";

/*
 * A hard ceiling rather than a growing buffer, for the same reason as
 * slate_shelly's BODY_MAX: truncated JSON parses as a parse failure instead of
 * a wrong reading, and a body that does not fit is a reply this client does
 * not understand. 16 KB because the device catalog is the largest response and
 * a page of it is far bigger than a Shelly status. Responses announcing more
 * are refused outright before parsing (see perform()).
 */
#define BODY_MAX (16 * 1024)

/*
 * Cloud + TLS handshake, from the poller task — not the LVGL task, so this is
 * not racing a UI deadline the way slate_shelly's 2 s is. Long enough that a
 * congested uplink is a retry, not a false "offline".
 */
#define HTTP_TIMEOUT_MS 10000

#define PATH_MAX        384 /* path including the query string */
#define T_STRING_MAX    16  /* 13-digit ms timestamp + NUL */
#define SIGN_HEX_MAX    64  /* hex of one SHA-256 */
#define TOKEN_MAX       96  /* Tuya tokens seen are 32 hex chars; refuse longer, never truncate */
#define QUERY_PAIRS_MAX 8   /* per signed URL — the paths used carry at most three */

/* Production omits nonce/signature headers. The extra fixture room lets the
 * selftest reproduce Tuya's official worked examples byte-for-byte. */
#define STR_MAX 1024

struct slate_tuya_client {
    char host[48];
    char access_id[SLATE_TUYA_ACCESS_ID_MAX_LEN + 1];
    char secret[SLATE_TUYA_SECRET_MAX_LEN + 1];
    /* Stored because the brief says the config is copied; the client itself
     * never puts it on the wire — callers name it in request paths. */
    char uid[SLATE_TUYA_UID_MAX_LEN + 1];
    SemaphoreHandle_t lock;
    char access_token[TOKEN_MAX + 1];
    char refresh_token[TOKEN_MAX + 1];
    int64_t token_obtained_us; /* esp_timer clock, not wall time */
    int64_t token_lifetime_ms;
};

/* Endpoints per Tuya's "Request Structure" doc (doc id Ka4a8uuo1j4t4). */
static const struct {
    const char *region;
    const char *host;
} REGIONS[] = {
    {"us", "openapi.tuyaus.com"},
    {"eu", "openapi.tuyaeu.com"},
    {"cn", "openapi.tuyacn.com"},
    {"in", "openapi.tuyain.com"},
    {"ueaz", "openapi-ueaz.tuyaus.com"},
    {"weaz", "openapi-weaz.tuyaeu.com"},
};

/* --- Signing -------------------------------------------------------------- */

static void hex_encode(const uint8_t *digest, size_t len, char *out, bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/** @brief Compare two query-pair keys (the part before '='), byte order. */
static int query_key_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        char ca = *a == '=' ? '\0' : *a;
        char cb = *b == '=' ? '\0' : *b;
        if (ca != cb || ca == '\0') {
            return (int) (unsigned char) ca - (int) (unsigned char) cb;
        }
    }
}

/*
 * The URL as the signature sees it: path, then the query parameters sorted by
 * key in ascending byte order. Tuya's doc is explicit that the sort is
 * alphabetical on the whole key, not on the pair string — values do not
 * participate in the ordering.
 */
static esp_err_t canonical_url(const char *path, char *out, size_t out_len)
{
    const char *query = strchr(path, '?');
    if (query == NULL || query[1] == '\0') {
        if (strlen(path) >= out_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        strcpy(out, path);
        return ESP_OK;
    }

    size_t base_len = (size_t) (query - path);
    size_t query_len = strlen(query + 1);
    char copy[PATH_MAX + 1];
    if (base_len >= out_len || query_len >= sizeof(copy)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(copy, query + 1, query_len + 1);

    char *pairs[QUERY_PAIRS_MAX];
    size_t count = 0;
    char *p = copy;
    for (;;) {
        if (count == QUERY_PAIRS_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        pairs[count++] = p;
        char *amp = strchr(p, '&');
        if (amp == NULL) {
            break;
        }
        *amp = '\0';
        p = amp + 1;
    }
    /* Insertion sort; stable, so duplicate keys keep their relative order. */
    for (size_t i = 1; i < count; i++) {
        char *pair = pairs[i];
        size_t j = i;
        while (j > 0 && query_key_cmp(pairs[j - 1], pair) > 0) {
            pairs[j] = pairs[j - 1];
            j--;
        }
        pairs[j] = pair;
    }

    memcpy(out, path, base_len);
    size_t used = base_len;
    out[used++] = '?';
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(pairs[i]);
        if (used + len + 2 > out_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out + used, pairs[i], len);
        used += len;
        out[used++] = i + 1 < count ? '&' : '\0';
    }
    return ESP_OK;
}

/* Signed paths are relative to the selected allowlisted origin. Restrict path
 * segments to RFC 3986 unreserved bytes and query strings to the small form
 * vocabulary Tuya uses, so a cloud/user identifier cannot inject another
 * authority, fragment, header, or path traversal segment. */
static bool request_path_valid(const char *path)
{
    if (path == NULL || path[0] != '/' || path[1] == '/' || strlen(path) > PATH_MAX) {
        return false;
    }
    const char *query = strchr(path, '?');
    const char *path_end = query != NULL ? query : path + strlen(path);
    const char *segment = path + 1;
    for (const char *p = path + 1; p < path_end; ++p) {
        unsigned char c = (unsigned char) *p;
        if (c == '/') {
            size_t len = (size_t) (p - segment);
            if (len == 0 || (len == 1 && segment[0] == '.') ||
                (len == 2 && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }
            segment = p + 1;
        } else if (!(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')) {
            return false;
        }
    }
    size_t len = (size_t) (path_end - segment);
    if (len == 0 || (len == 1 && segment[0] == '.') ||
        (len == 2 && segment[0] == '.' && segment[1] == '.')) {
        return false;
    }
    if (query == NULL) {
        return true;
    }
    if (query[1] == '\0' || strchr(query + 1, '?') != NULL) {
        return false;
    }
    const char *key = query + 1;
    for (const char *p = query + 1; *p != '\0'; ++p) {
        unsigned char c = (unsigned char) *p;
        if ((c == '&' || c == '=') && p == key) {
            return false;
        }
        if (c == '&') {
            key = p + 1;
        }
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '=' ||
              c == '&' || c == '%')) {
            return false;
        }
    }
    if (*key == '\0') {
        return false;
    }
    return true;
}

static bool path_segment_valid(const char *value)
{
    if (value == NULL || value[0] == '\0' || strcmp(value, ".") == 0 ||
        strcmp(value, "..") == 0) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *) value; *p != '\0'; ++p) {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~')) {
            return false;
        }
    }
    return true;
}

/**
 * Fill t_str with the ms timestamp and sign_hex with the request signature.
 *
 * `t` wants wall-clock milliseconds, and the only wall clock this panel has is
 * SNTP via slate_time. Before the first sync, time(NULL) is seconds since boot
 * and Tuya answers 1013 ("request time is invalid") — classified NETWORK
 * below, because on this device that code means "the clock is still syncing",
 * a transient state the next sweep recovers from on its own.
 */
static esp_err_t build_sign_parts(const char *access_id, const char *access_token,
                                  const char *secret, const char *method_name,
                                  const char *path, const char *body, const char *timestamp,
                                  const char *nonce, const char *signature_headers,
                                  char sign_hex[SIGN_HEX_MAX + 1])
{
    uint8_t body_digest[32];
    char body_hex[SIGN_HEX_MAX + 1];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    if (body != NULL) {
        mbedtls_sha256_update(&sha, (const unsigned char *) body, strlen(body));
    }
    mbedtls_sha256_finish(&sha, body_digest);
    mbedtls_sha256_free(&sha);
    /* Content-SHA256 is lowercase hex in the docs' worked examples. */
    hex_encode(body_digest, sizeof(body_digest), body_hex, false);

    char url[PATH_MAX + 1];
    esp_err_t err = canonical_url(path, url, sizeof(url));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: query string cannot be canonicalised", path);
        return err;
    }

    /* The blank line between body hash and URL is the empty HEADERS section
     * when production passes signature_headers="". Official fixtures below
     * exercise the same primitive with non-empty headers and nonce. */
    char str[STR_MAX];
    int n = snprintf(str, sizeof(str), "%s%s%s%s%s\n%s\n%s\n%s", access_id,
                     access_token != NULL ? access_token : "", timestamp,
                     nonce != NULL ? nonce : "", method_name, body_hex,
                     signature_headers != NULL ? signature_headers : "", url);
    if (n <= 0 || (size_t) n >= sizeof(str)) {
        ESP_LOGE(TAG, "%s: string-to-sign does not fit %u B", path, (unsigned) sizeof(str));
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[32];
    int rc = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                             (const unsigned char *) secret, strlen(secret),
                             (const unsigned char *) str, (size_t) n, digest);
    if (rc != 0) {
        ESP_LOGE(TAG, "mbedtls_md_hmac: -0x%04x", -rc);
        return ESP_FAIL;
    }
    /* The sign header is uppercase hex; the docs' examples capitalise it. */
    hex_encode(digest, sizeof(digest), sign_hex, true);
    return ESP_OK;
}

static esp_err_t build_sign(const struct slate_tuya_client *h, const char *method_name,
                            const char *path, const char *body, bool use_token,
                            char t_str[T_STRING_MAX], char sign_hex[SIGN_HEX_MAX + 1])
{
    int n = snprintf(t_str, T_STRING_MAX, "%" PRId64, (int64_t) time(NULL) * 1000);
    if (n <= 0 || n >= T_STRING_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    return build_sign_parts(h->access_id, use_token ? h->access_token : NULL, h->secret,
                            method_name, path, body, t_str, "", "", sign_hex);
}

/* --- Error classification --------------------------------------------------- */

/*
 * The numbers are Tuya's "Global Error Codes" (doc id K989ruxx88swc), and the
 * kind decides what the lifecycle tells the user: NETWORK stales tiles and
 * recovers, AUTH and QUOTA need a person at iot.tuya.com.
 */
static slate_tuya_error_kind_t classify_code(long code)
{
    switch (code) {
    case 1001: /* secret invalid */
    case 1002: /* access_token is null */
    case 1004: /* sign invalid */
    case 1005: /* clientId invalid */
    case 1010: /* token is expired — retried once by the caller before this is believed */
    case 1011: /* token invalid */
    case 1012: /* token status is invalid */
    case 1400: /* token invalid */
        return SLATE_TUYA_ERR_AUTH;
    case 1013: /* request time is invalid — on this panel, SNTP still syncing */
        return SLATE_TUYA_ERR_NETWORK;
    case 1106: /* permission deny */
        return SLATE_TUYA_ERR_QUOTA;
    default:
        /* 28841001–28841004: cloud development plan missing/expired/overdue/
         * quota exhausted. 28841101–28841106: per-API subscription missing/
         * expired/overdue/exhausted/unauthorised. */
        if ((code >= 28841001 && code <= 28841004) || (code >= 28841101 && code <= 28841106)) {
            return SLATE_TUYA_ERR_QUOTA;
        }
        return SLATE_TUYA_ERR_API;
    }
}

/** @brief Whether the envelope code means the held token stopped being believed. */
static bool is_token_invalid(long code)
{
    return code == 1010 || code == 1011 || code == 1012 || code == 1400;
}

/** @brief Classification for an HTTP-level failure that carried no Tuya envelope. */
static slate_tuya_error_kind_t classify_status(int status)
{
    if (status == 401 || status == 403) {
        return SLATE_TUYA_ERR_AUTH;
    }
    /* 5xx and 429 are "try later" — closer to a network event than to a
     * mistake a person has to fix. */
    if (status == 429 || status >= 500) {
        return SLATE_TUYA_ERR_NETWORK;
    }
    return SLATE_TUYA_ERR_API;
}

/* --- One round trip --------------------------------------------------------- */

/*
 * Unwrap {"success", "code", "msg", "result"}. On success the caller gets the
 * detached "result" child (the envelope itself is deleted here); a success
 * envelope with no result is a malformed reply, not an empty one, because
 * Tuya documents result as always present on success.
 */
static esp_err_t parse_envelope(const char *resp, size_t length, int status, const char *path,
                                cJSON **out_body, slate_tuya_error_kind_t *kind,
                                bool *token_invalid)
{
    if (status < 200 || status >= 300) {
        *kind = classify_status(status);
        ESP_LOGW(TAG, "%s: refusing HTTP %d response", path, status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *doc = cJSON_ParseWithLength(resp, length);
    if (doc == NULL) {
        *kind = classify_status(status);
        ESP_LOGW(TAG, "%s: HTTP %d with an unparseable body", path, status);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *success = cJSON_GetObjectItemCaseSensitive(doc, "success");
    if (!cJSON_IsBool(success)) {
        ESP_LOGW(TAG, "%s: HTTP %d, reply has no \"success\" field", path, status);
        cJSON_Delete(doc);
        *kind = classify_status(status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!cJSON_IsTrue(success)) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(doc, "code");
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(doc, "msg");
        long c = cJSON_IsNumber(code) ? (long) code->valuedouble : -1;
        *token_invalid = is_token_invalid(c);
        *kind = classify_code(c);
        ESP_LOGW(TAG, "%s: Tuya error %ld (%s)", path, c,
                 cJSON_IsString(msg) ? msg->valuestring : "no message");
        cJSON_Delete(doc);
        return ESP_FAIL;
    }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(doc, "result");
    if (result == NULL) {
        ESP_LOGW(TAG, "%s: success with no \"result\"", path);
        cJSON_Delete(doc);
        *kind = SLATE_TUYA_ERR_API;
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (out_body != NULL) {
        *out_body = cJSON_DetachItemViaPointer(doc, result);
    }
    cJSON_Delete(doc);
    return ESP_OK;
}

/**
 * Sign and send one request, read the whole body into resp, parse the
 * envelope. resp must hold BODY_MAX bytes. On ESP_OK and out_body != NULL,
 * *out_body is the detached "result" the caller deletes.
 */
static esp_err_t perform(struct slate_tuya_client *h, int method, const char *method_name,
                         const char *path, const char *body, bool use_token, char *resp,
                         cJSON **out_body, slate_tuya_error_kind_t *kind, bool *token_invalid)
{
    *token_invalid = false;

    char t_str[T_STRING_MAX];
    char sign[SIGN_HEX_MAX + 1];
    esp_err_t err = build_sign(h, method_name, path, body, use_token, t_str, sign);
    if (err != ESP_OK) {
        /* A local construction failure (oversized path/query): not the cloud's
         * answer and not the network's silence — reported as API so the
         * lifecycle calls it an error rather than "offline". */
        *kind = SLATE_TUYA_ERR_API;
        return err;
    }

    char url[sizeof("https://") - 1 + 48 + PATH_MAX + 1];
    int n = snprintf(url, sizeof(url), "https://%s%s", h->host, path);
    if (n <= 0 || (size_t) n >= sizeof(url)) {
        ESP_LOGE(TAG, "%s: URL does not fit %u B", path, (unsigned) sizeof(url));
        *kind = SLATE_TUYA_ERR_API;
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_http_client_config_t config = {
        .url = url,
        .method = (esp_http_client_method_t) method,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        /* One request per connection. Calls are seconds-to-minutes apart, and
         * a kept-alive TLS session is memory held the whole time — the same
         * line slate_update draws for its release channel. */
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "no memory for the HTTP client");
        *kind = SLATE_TUYA_ERR_API;
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "client_id", h->access_id);
    esp_http_client_set_header(client, "sign", sign);
    esp_http_client_set_header(client, "sign_method", "HMAC-SHA256");
    esp_http_client_set_header(client, "t", t_str);
    if (use_token) {
        esp_http_client_set_header(client, "access_token", h->access_token);
    }
    if (body != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
    }

    int status = 0;
    int length = -1;
    err = esp_http_client_open(client, body != NULL ? (int) strlen(body) : 0);
    if (err == ESP_OK && body != NULL) {
        int written = esp_http_client_write(client, body, (int) strlen(body));
        if (written != (int) strlen(body)) {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        int64_t announced = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (announced > (int64_t) BODY_MAX - 1) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            length = esp_http_client_read_response(client, resp, BODY_MAX - 1);
            if (length < 0) {
                err = ESP_FAIL;
            } else if (announced >= 0 && length < announced) {
                err = ESP_ERR_INVALID_SIZE;
            } else if (length == BODY_MAX - 1) {
                char extra;
                int tail = esp_http_client_read(client, &extra, 1);
                if (tail != 0) {
                    err = tail < 0 ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
                }
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err == ESP_ERR_INVALID_SIZE) {
        /* The reply outgrew the cap. Refused outright rather than parsed
         * truncated — a partial device list is a wrong picture of the house. */
        ESP_LOGE(TAG, "%s %s: response over the %u B cap", method_name, path,
                 (unsigned) (BODY_MAX - 1));
        *kind = SLATE_TUYA_ERR_API;
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s: transport failed: %s", method_name, path, esp_err_to_name(err));
        *kind = SLATE_TUYA_ERR_NETWORK;
        return ESP_FAIL;
    }
    resp[length] = '\0';
    return parse_envelope(resp, (size_t) length, status, path, out_body, kind, token_invalid);
}

/* --- Token lifecycle -------------------------------------------------------- */

/** @brief Whether the held token has at least 20% of its lifetime left. */
static bool token_fresh(const struct slate_tuya_client *h)
{
    if (h->access_token[0] == '\0' || h->token_lifetime_ms <= 0) {
        return false;
    }
    int64_t elapsed_ms = (esp_timer_get_time() - h->token_obtained_us) / 1000;
    /* Refresh at ~80% consumed (spec §Lifecycle): keep using the token while
     * elapsed < 4/5 of its lifetime. */
    int64_t refresh_at_ms = h->token_lifetime_ms - h->token_lifetime_ms / 5;
    return elapsed_ms >= 0 && elapsed_ms < refresh_at_ms;
}

/**
 * GET a token-management path (grant or refresh), signed with the secret
 * alone, and adopt the tokens it returns. A refresh answer carries a new
 * refresh_token too; if it omits one, the old refresh token is kept.
 */
static esp_err_t token_call(struct slate_tuya_client *h, const char *path, char *resp,
                            slate_tuya_error_kind_t *kind)
{
    cJSON *result = NULL;
    bool ignored = false;
    esp_err_t err = perform(h, HTTP_METHOD_GET, "GET", path, NULL, false, resp, &result, kind,
                            &ignored);
    if (err != ESP_OK) {
        return err;
    }

    const cJSON *access = cJSON_GetObjectItemCaseSensitive(result, "access_token");
    const cJSON *refresh = cJSON_GetObjectItemCaseSensitive(result, "refresh_token");
    const cJSON *expires = cJSON_GetObjectItemCaseSensitive(result, "expire_time");
    /* A token that does not fit is refused rather than truncated: a truncated
     * credential fails as 1004 "sign invalid" on every subsequent request and
     * reads exactly like a signing bug. */
    bool ok = cJSON_IsString(access) && access->valuestring != NULL &&
              strlen(access->valuestring) <= TOKEN_MAX && cJSON_IsNumber(expires) &&
              expires->valuedouble > 0 && expires->valuedouble <= (double) INT64_MAX / 1000.0;
    bool has_refresh = cJSON_IsString(refresh) && refresh->valuestring != NULL;
    if (has_refresh && strlen(refresh->valuestring) > TOKEN_MAX) {
        ok = false;
    }
    if (!ok) {
        ESP_LOGE(TAG, "%s: token response is missing fields or they are oversized", path);
        cJSON_Delete(result);
        *kind = SLATE_TUYA_ERR_API;
        return ESP_ERR_INVALID_RESPONSE;
    }

    strcpy(h->access_token, access->valuestring);
    if (has_refresh) {
        strcpy(h->refresh_token, refresh->valuestring);
    }
    h->token_obtained_us = esp_timer_get_time();
    h->token_lifetime_ms = (int64_t) (expires->valuedouble * 1000.0);
    cJSON_Delete(result);
    ESP_LOGI(TAG, "token acquired, lifetime %" PRId64 " s", h->token_lifetime_ms / 1000);
    return ESP_OK;
}

/** @brief Make sure a usable token is held: keep, refresh, or re-acquire. */
static esp_err_t ensure_token(struct slate_tuya_client *h, char *resp,
                              slate_tuya_error_kind_t *kind)
{
    if (token_fresh(h)) {
        return ESP_OK;
    }
    if (h->refresh_token[0] != '\0') {
        char path[sizeof("/v1.0/token/") - 1 + TOKEN_MAX + 1];
        snprintf(path, sizeof(path), "/v1.0/token/%s", h->refresh_token);
        if (token_call(h, path, resp, kind) == ESP_OK) {
            return ESP_OK;
        }
        /* A refused refresh is not the end of the session — the grant path is
         * still open, and it is cheaper to take it than to fail the sweep. */
        ESP_LOGW(TAG, "refresh refused; requesting a fresh token");
        h->access_token[0] = '\0';
        h->refresh_token[0] = '\0';
    }
    return token_call(h, "/v1.0/token?grant_type=1", resp, kind);
}

/* --- Public API ------------------------------------------------------------- */

esp_err_t slate_tuya_client_create(const char *region, const char *access_id,
                                   const char *secret, const char *uid,
                                   slate_tuya_client_handle_t *out)
{
    if (region == NULL || access_id == NULL || secret == NULL || uid == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *host = NULL;
    for (size_t i = 0; i < sizeof(REGIONS) / sizeof(REGIONS[0]); i++) {
        if (strcmp(region, REGIONS[i].region) == 0) {
            host = REGIONS[i].host;
            break;
        }
    }
    if (host == NULL) {
        ESP_LOGE(TAG, "unknown Tuya region \"%s\"", region);
        return ESP_ERR_INVALID_ARG;
    }
    /* The same ceilings the route validates and the store enforces: the client
     * must not accept a credential the store would refuse to keep. */
    if (access_id[0] == '\0' || strlen(access_id) > SLATE_TUYA_ACCESS_ID_MAX_LEN ||
        secret[0] == '\0' || strlen(secret) > SLATE_TUYA_SECRET_MAX_LEN || uid[0] == '\0' ||
        strlen(uid) > SLATE_TUYA_UID_MAX_LEN || !path_segment_valid(uid)) {
        return ESP_ERR_INVALID_ARG;
    }

    struct slate_tuya_client *h = calloc(1, sizeof(*h));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }
    h->lock = xSemaphoreCreateMutex();
    if (h->lock == NULL) {
        free(h);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(h->host, host, sizeof(h->host));
    strlcpy(h->access_id, access_id, sizeof(h->access_id));
    strlcpy(h->secret, secret, sizeof(h->secret));
    strlcpy(h->uid, uid, sizeof(h->uid));
    *out = h;
    return ESP_OK;
}

void slate_tuya_client_destroy(slate_tuya_client_handle_t h)
{
    if (h == NULL) {
        return;
    }
    /* Credentials should not linger in a freed heap block. */
    memset(h->secret, 0, sizeof(h->secret));
    memset(h->access_token, 0, sizeof(h->access_token));
    memset(h->refresh_token, 0, sizeof(h->refresh_token));
    vSemaphoreDelete(h->lock);
    free(h);
}

esp_err_t slate_tuya_client_request(slate_tuya_client_handle_t h, int method, const char *path,
                                    const char *body, cJSON **out_body,
                                    slate_tuya_error_kind_t *err_kind)
{
    if (out_body != NULL) {
        *out_body = NULL;
    }
    slate_tuya_error_kind_t local_kind = SLATE_TUYA_ERR_NONE;
    slate_tuya_error_kind_t *kind = err_kind != NULL ? err_kind : &local_kind;
    *kind = SLATE_TUYA_ERR_NONE;

    const char *method_name = method == HTTP_METHOD_GET    ? "GET"
                              : method == HTTP_METHOD_POST ? "POST"
                              : method == HTTP_METHOD_DELETE
                                                           ? "DELETE"
                                                           : NULL;
    if (h == NULL || !request_path_valid(path) || method_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Allocated per call rather than held by the handle: 16 KB is too much to
     * park for the lifetime of a configuration that may be exercised once an
     * hour, and the mutex below already serialises callers, so at most one
     * buffer exists at a time either way. */
    char *resp = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM);
    if (resp == NULL) {
        resp = malloc(BODY_MAX);
    }
    if (resp == NULL) {
        ESP_LOGE(TAG, "no %u B for a response buffer", (unsigned) BODY_MAX);
        *kind = SLATE_TUYA_ERR_API;
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2; attempt++) {
        err = ensure_token(h, resp, kind);
        if (err != ESP_OK) {
            break;
        }
        bool token_invalid = false;
        err = perform(h, method, method_name, path, body, true, resp, out_body, kind,
                      &token_invalid);
        if (err == ESP_OK || !token_invalid || attempt == 1) {
            break;
        }
        /* 1010/1011/1012/1400: the token stopped being believed between the
         * freshness check and the server. Drop it and retry exactly once; a
         * second rejection classifies AUTH and lands on a person. */
        ESP_LOGW(TAG, "%s: token rejected; re-acquiring and retrying once", path);
        h->access_token[0] = '\0';
    }
    xSemaphoreGive(h->lock);
    free(resp);
    return err;
}

esp_err_t slate_tuya_client_test(const char *region, const char *access_id, const char *secret,
                                 const char *uid, slate_tuya_error_kind_t *err_kind)
{
    if (err_kind != NULL) {
        *err_kind = SLATE_TUYA_ERR_NONE;
    }
    slate_tuya_client_handle_t h = NULL;
    /* A create() failure here is a malformed field, not a Tuya answer:
     * ESP_ERR_INVALID_ARG comes back with err_kind still NONE, which the route
     * maps to its invalid_config refusal. */
    esp_err_t err = slate_tuya_client_create(region, access_id, secret, uid, &h);
    if (err != ESP_OK) {
        return err;
    }

    /* The token fetch proves the key pair; one page of the device list proves
     * the uid is actually linked to the project. */
    char path[sizeof("/v1.0/users//devices") - 1 + SLATE_TUYA_UID_MAX_LEN + 1];
    snprintf(path, sizeof(path), "/v1.0/users/%s/devices", uid);
    cJSON *doc = NULL;
    err = slate_tuya_client_request(h, HTTP_METHOD_GET, path, NULL, &doc, err_kind);
    cJSON_Delete(doc);
    slate_tuya_client_destroy(h);
    return err;
}

#ifdef SLATE_TUYA_SELFTEST

esp_err_t slate_tuya_client_selftest(void)
{
    static const char CLIENT_ID[] = "1KAD46OrT9HafiKdsXeg";
    static const char SECRET[] = "4OHBOnWOqaEC1mWXOpVL3yV50s0qGSRC";
    static const char TOKEN[] = "3f4eda2bdec17232f67c0b188af3eec1";
    static const char T[] = "1588925778000";
    static const char NONCE[] = "5138cc3a9033d69856923fd07b491173";
    static const char HEADERS[] =
        "area_id:29a33e8796834b1efa6\n"
        "call_id:8afdb70ab2ed11eb85290242ac130003\n";
    char sign[SIGN_HEX_MAX + 1];
    bool ok = build_sign_parts(CLIENT_ID, NULL, SECRET, "GET",
                               "/v1.0/token?grant_type=1", NULL, T, NONCE, HEADERS,
                               sign) == ESP_OK &&
              strcmp(sign, "9E48A3E93B302EEECC803C7241985D0A34EB944F40FB573C7B5C2A82158AF13E") == 0;
    ok = ok && build_sign_parts(CLIENT_ID, TOKEN, SECRET, "GET",
                                "/v2.0/apps/schema/users?page_size=50&page_no=1", NULL,
                                T, NONCE, HEADERS, sign) == ESP_OK &&
         strcmp(sign, "AE4481C692AA80B25F3A7E12C3A5FD9BBF6251539DD78E565A1A72A508A88784") == 0;
    static const char BODY[] =
        "{\"commands\":[{\"code\":\"switch_led\",\"value\":false}]}";
    ok = ok && build_sign_parts(CLIENT_ID, TOKEN, SECRET, "POST",
                                "/v1.0/devices/bf2c1e00ab0f12face/commands", BODY, T,
                                "", "", sign) == ESP_OK &&
         strcmp(sign, "F27DD95A15E84C954DB2ED016096D517681FE79853B6CDAD446927352691804E") == 0;

    slate_tuya_error_kind_t kind = SLATE_TUYA_ERR_NONE;
    bool token_invalid = false;
    static const char SUCCESS[] = "{\"success\":true,\"result\":true}";
    static const char MISSING_RESULT[] = "{\"success\":true}";
    ok = ok && parse_envelope(SUCCESS, strlen(SUCCESS), 302,
                              "/fixture", NULL, &kind, &token_invalid) != ESP_OK &&
         kind == SLATE_TUYA_ERR_API;
    kind = SLATE_TUYA_ERR_NONE;
    ok = ok && parse_envelope(SUCCESS, strlen(SUCCESS), 429,
                              "/fixture", NULL, &kind, &token_invalid) != ESP_OK &&
         kind == SLATE_TUYA_ERR_NETWORK;
    kind = SLATE_TUYA_ERR_NONE;
    ok = ok && parse_envelope(MISSING_RESULT, strlen(MISSING_RESULT), 200, "/fixture", NULL,
                              &kind, &token_invalid) != ESP_OK &&
         kind == SLATE_TUYA_ERR_API;
    ok = ok && classify_code(1011) == SLATE_TUYA_ERR_AUTH &&
         classify_code(1106) == SLATE_TUYA_ERR_QUOTA &&
         classify_code(28841004) == SLATE_TUYA_ERR_QUOTA &&
         classify_code(1013) == SLATE_TUYA_ERR_NETWORK &&
         request_path_valid("/v1.0/users/safe_UID-1/devices") &&
         !request_path_valid("//attacker.invalid/v1.0/devices") &&
         !request_path_valid("/v1.0/users/../devices") &&
         !request_path_valid("/v1.0/users/id%2Fescape/devices");
    return ok ? ESP_OK : ESP_FAIL;
}

#endif /* SLATE_TUYA_SELFTEST */
