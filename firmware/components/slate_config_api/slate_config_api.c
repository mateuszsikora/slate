/* Slate — HTTP boundary for the active dashboard configuration. */

#include "slate_config_api.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "slate_api.h"
#include "slate_config.h"
#include "slate_store.h"
#include "slate_ui.h"
#include "slate_ws.h"

static const char *TAG = "config_api";

/*
 * A transient document is the active configuration just as surely as a
 * persisted one, so GET must return it while its RAM-only tree is on screen.
 * The HTTP server serialises handlers on one task; no second lock is needed
 * around this pointer or the store/rebuild sequence below.
 */
static char *s_transient_json;
static size_t s_transient_len;
static bool s_initialized;

static bool query_flag_is_set(httpd_req_t *req, const char *key)
{
    char query[64];
    char value[8];

    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, key, value, sizeof(value)) == ESP_OK &&
           strcmp(value, "1") == 0;
}

static esp_err_t read_body(httpd_req_t *req, char **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (req->content_len == 0) {
        return slate_api_refuse(req, "400 Bad Request", "empty_body");
    }
    if (req->content_len > SLATE_CONFIG_MAX_BYTES) {
        return slate_api_refuse(req, "413 Payload Too Large", "too_large");
    }

    char *body = heap_caps_malloc_prefer(req->content_len + 1, 2,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (body == NULL) {
        return slate_api_refuse(req, "500 Internal Server Error", "out_of_memory");
    }

    size_t received = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, body + received, req->content_len - received);
        if (chunk <= 0) {
            free(body);
            return slate_api_refuse(req, "400 Bad Request", "invalid_json");
        }
        received += (size_t) chunk;
    }
    body[received] = '\0';
    *out = body;
    *out_len = received;
    return ESP_OK;
}

static esp_err_t send_parse_result(httpd_req_t *req,
                                   slate_config_parse_status_t result,
                                   const slate_config_report_t *report)
{
    switch (result) {
    case SLATE_CONFIG_PARSE_INVALID_CONFIG: {
        cJSON *response = slate_config_report_json(report);
        if (response != NULL) {
            httpd_resp_set_status(req, "400 Bad Request");
        }
        return slate_api_send_json(req, response);
    }
    case SLATE_CONFIG_PARSE_TOO_LARGE:
        return slate_api_refuse(req, "413 Payload Too Large", "too_large");
    case SLATE_CONFIG_PARSE_OUT_OF_MEMORY:
        return slate_api_refuse(req, "500 Internal Server Error", "out_of_memory");
    case SLATE_CONFIG_PARSE_EMPTY_BODY:
        return slate_api_refuse(req, "400 Bad Request", "empty_body");
    case SLATE_CONFIG_PARSE_INVALID_JSON:
    default:
        return slate_api_refuse(req, "400 Bad Request", "invalid_json");
    }
}

static size_t active_tile_count(const slate_config_t *config)
{
    for (size_t i = 0; i < config->page_count; i++) {
        if (strcmp(config->pages[i].id, config->home_page) == 0) {
            return config->pages[i].tile_count;
        }
    }
    return 0; /* The accepted model makes this unreachable. */
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char *stored = NULL;
    const char *json = s_transient_json;
    size_t len = s_transient_len;

    if (json == NULL) {
        esp_err_t err = slate_store_config_read(&stored, &len);
        if (err == ESP_ERR_NOT_FOUND) {
            return slate_api_refuse(req, "404 Not Found", "not_found");
        }
        if (err == ESP_ERR_NO_MEM) {
            return slate_api_refuse(req, "500 Internal Server Error", "out_of_memory");
        }
        if (err != ESP_OK) {
            return slate_api_refuse(req, "500 Internal Server Error", "store_failed");
        }
        json = stored;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, len);
    free(stored);
    return err;
}

static esp_err_t config_validate_handler(httpd_req_t *req)
{
    char *body = NULL;
    size_t len = 0;
    esp_err_t err = read_body(req, &body, &len);
    if (err != ESP_OK) {
        return err;
    }

    slate_config_t *config = NULL;
    slate_config_report_t report;
    slate_config_parse_status_t result =
        slate_config_parse(body, len, &config, &report);
    free(body);

    if (result == SLATE_CONFIG_PARSE_OK) {
        slate_config_free(config);
        slate_config_report_free(&report);
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }

    err = send_parse_result(req, result, &report);
    slate_config_report_free(&report);
    return err;
}

static void replace_transient(char *json, size_t len)
{
    free(s_transient_json);
    s_transient_json = json;
    s_transient_len = json != NULL ? len : 0;
}

static esp_err_t config_put_handler(httpd_req_t *req)
{
    bool transient = query_flag_is_set(req, "transient");
    if (transient && slate_ws_mode() != SLATE_WS_MODE_EDIT) {
        return slate_api_refuse(req, "409 Conflict", "edit_mode_required");
    }

    char *body = NULL;
    size_t len = 0;
    esp_err_t err = read_body(req, &body, &len);
    if (err != ESP_OK) {
        return err;
    }

    slate_config_t *config = NULL;
    slate_config_report_t report;
    slate_config_parse_status_t result =
        slate_config_parse(body, len, &config, &report);
    if (result != SLATE_CONFIG_PARSE_OK) {
        free(body);
        err = send_parse_result(req, result, &report);
        slate_config_report_free(&report);
        return err;
    }
    slate_config_report_free(&report);

    unsigned schema = (unsigned) config->schema;
    size_t tiles = active_tile_count(config);
    err = slate_ui_rebuild(config);
    slate_config_free(config);
    if (err != ESP_OK) {
        free(body);
        ESP_LOGE(TAG, "dashboard activation failed: %s", esp_err_to_name(err));
        return slate_api_refuse(req, "500 Internal Server Error",
                                err == ESP_ERR_NO_MEM ? "out_of_memory" : "apply_failed");
    }

    if (transient) {
        replace_transient(body, len);
    } else {
        err = slate_store_config_write(body, len);
        if (err != ESP_OK) {
            /*
             * The rebuild happens before persistence by contract. If the
             * atomic temp-file write fails, the old file is intact while this
             * already-active document becomes RAM-only. Keeping it here makes
             * GET truthful and a reboot restores the last committed document.
             */
            replace_transient(body, len);
            ESP_LOGE(TAG, "persisting active dashboard failed: %s", esp_err_to_name(err));
            return slate_api_refuse(req, "500 Internal Server Error", "store_failed");
        }
        free(body);
        replace_transient(NULL, 0);
    }

    err = slate_ws_publish_reloaded(schema, tiles);
    if (err != ESP_OK) {
        /* The configuration is already active. A diagnostics transport that
         * failed to start cannot turn a successful replacement into a lie. */
        ESP_LOGW(TAG, "reloaded event unavailable: %s", esp_err_to_name(err));
    }

    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t slate_config_api_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const httpd_uri_t get_config = {
        .uri = SLATE_API_BASE_PATH "/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
    };
    const httpd_uri_t put_config = {
        .uri = SLATE_API_BASE_PATH "/config",
        .method = HTTP_PUT,
        .handler = config_put_handler,
    };
    const httpd_uri_t validate = {
        .uri = SLATE_API_BASE_PATH "/config/validate",
        .method = HTTP_POST,
        .handler = config_validate_handler,
    };

    esp_err_t err = slate_api_register_uri(&get_config, SLATE_API_AUTH_DEVICE_TOKEN);
    if (err == ESP_OK) {
        err = slate_api_register_uri(&put_config, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (err == ESP_OK) {
        err = slate_api_register_uri(&validate, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (err == ESP_OK) {
        s_initialized = true;
        ESP_LOGI(TAG, "configuration API ready");
    }
    return err;
}
