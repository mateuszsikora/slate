/* Slate — HTTP boundary for the active dashboard configuration. */

#include "slate_config_api.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "slate_api.h"
#include "slate_config.h"
#include "slate_store.h"
#include "slate_ui.h"
#include "slate_ws.h"

static const char *TAG = "config_api";

#define CONFIG_MODE_TASK_STACK    4096
#define CONFIG_MODE_TASK_PRIORITY 3

/*
 * A transient document is the active configuration just as surely as a
 * persisted one, so GET must return it while its RAM-only tree is on screen.
 * HTTP handlers are serialised by esp_http_server, but edit mode changes on
 * the WebSocket task. This mutex orders both paths so a mode fallback cannot
 * race a preview into becoming the normal dashboard.
 */
static char *s_transient_json;
static size_t s_transient_len;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_mode_task;
static bool s_initialized;

typedef enum {
    CONFIG_PUT_PERSISTENT = 0,
    CONFIG_PUT_TRANSIENT,
    CONFIG_PUT_INVALID_QUERY,
} config_put_mode_t;

static config_put_mode_t parse_put_query(const char *query, size_t len)
{
    static const char TRANSIENT_QUERY[] = "transient=1";
    if (len == 0) {
        return CONFIG_PUT_PERSISTENT;
    }
    return query != NULL && len == sizeof(TRANSIENT_QUERY) - 1 &&
                   memcmp(query, TRANSIENT_QUERY, sizeof(TRANSIENT_QUERY) - 1) == 0
               ? CONFIG_PUT_TRANSIENT
               : CONFIG_PUT_INVALID_QUERY;
}

static config_put_mode_t config_put_mode(httpd_req_t *req)
{
    size_t len = httpd_req_get_url_query_len(req);
    if (len == 0) {
        return parse_put_query(NULL, 0);
    }
    if (len >= 32) {
        return CONFIG_PUT_INVALID_QUERY;
    }

    char query[32];
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK
               ? parse_put_query(query, len)
               : CONFIG_PUT_INVALID_QUERY;
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
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *stored = NULL;
    const char *json = s_transient_json;
    size_t len = s_transient_len;

    if (json == NULL) {
        esp_err_t err = slate_store_config_read(&stored, &len);
        if (err == ESP_ERR_NOT_FOUND) {
            xSemaphoreGive(s_lock);
            return slate_api_refuse(req, "404 Not Found", "not_found");
        }
        if (err == ESP_ERR_NO_MEM) {
            xSemaphoreGive(s_lock);
            return slate_api_refuse(req, "500 Internal Server Error", "out_of_memory");
        }
        if (err != ESP_OK) {
            xSemaphoreGive(s_lock);
            return slate_api_refuse(req, "500 Internal Server Error", "store_failed");
        }
        json = stored;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, len);
    free(stored);
    xSemaphoreGive(s_lock);
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

static void publish_reloaded(unsigned schema, size_t tiles)
{
    esp_err_t err = slate_ws_publish_reloaded(schema, tiles);
    if (err != ESP_OK) {
        /* The configuration is already active. A diagnostics transport that
         * failed to start cannot turn a completed replacement into a lie. */
        ESP_LOGW(TAG, "reloaded event unavailable: %s", esp_err_to_name(err));
    }
}

static void restore_on_mode_task(void *ctx)
{
    (void) ctx;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (slate_ws_mode() != SLATE_WS_MODE_NORMAL) {
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_transient_json != NULL && slate_ws_mode() == SLATE_WS_MODE_NORMAL) {
            slate_ui_config_info_t info;
            esp_err_t err = slate_ui_restore_stored(&info);
            if (err == ESP_OK) {
                replace_transient(NULL, 0);
                if (info.configured) {
                    publish_reloaded(info.schema, info.tiles);
                }
                ESP_LOGI(TAG, "discarded transient dashboard on leaving edit mode");
            } else {
                /* The old presentation stays live when even its replacement could
                 * not be built; retain its JSON so GET continues to tell the truth. */
                ESP_LOGE(TAG, "restoring persisted dashboard failed: %s",
                         esp_err_to_name(err));
            }
        }
        xSemaphoreGive(s_lock);
    }
}

static void mode_changed(void *ctx, slate_ws_mode_t mode)
{
    (void) ctx;
    if (mode == SLATE_WS_MODE_NORMAL && s_mode_task != NULL) {
        xTaskNotifyGive(s_mode_task);
    }
}

static esp_err_t config_put_handler(httpd_req_t *req)
{
    config_put_mode_t mode = config_put_mode(req);
    if (mode == CONFIG_PUT_INVALID_QUERY) {
        return slate_api_refuse(req, "400 Bad Request", "invalid_query");
    }
    bool transient = mode == CONFIG_PUT_TRANSIENT;

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

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (transient && slate_ws_mode() != SLATE_WS_MODE_EDIT) {
        xSemaphoreGive(s_lock);
        free(body);
        slate_config_free(config);
        return slate_api_refuse(req, "409 Conflict", "edit_mode_required");
    }

    unsigned schema = (unsigned) config->schema;
    size_t tiles = active_tile_count(config);
    err = slate_ui_rebuild(config);
    slate_config_free(config);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        free(body);
        ESP_LOGE(TAG, "dashboard activation failed: %s", esp_err_to_name(err));
        return slate_api_refuse(req, "500 Internal Server Error",
                                err == ESP_ERR_NO_MEM ? "out_of_memory" : "apply_failed");
    }

    /* Reload describes activation, not persistence. Queue it now so a later
     * store failure cannot hide a tree and subscription set already in use. */
    publish_reloaded(schema, tiles);

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
            xSemaphoreGive(s_lock);
            return slate_api_refuse(req, "500 Internal Server Error", "store_failed");
        }
        free(body);
        replace_transient(NULL, 0);
    }

    xSemaphoreGive(s_lock);

    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t slate_config_api_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(restore_on_mode_task, "config_mode", CONFIG_MODE_TASK_STACK,
                    NULL, CONFIG_MODE_TASK_PRIORITY, &s_mode_task) != pdPASS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t observer_err = slate_ws_mode_observer_set(mode_changed, NULL);
    if (observer_err == ESP_ERR_NOT_FOUND) {
        /* Without a WebSocket task edit mode is unreachable, so transient PUT
         * remains safely disabled while persistent GET/PUT stay available. */
        ESP_LOGW(TAG, "WebSocket mode unavailable; transient previews disabled");
        vTaskDelete(s_mode_task);
        s_mode_task = NULL;
    } else if (observer_err != ESP_OK) {
        vTaskDelete(s_mode_task);
        s_mode_task = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return observer_err;
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

#ifdef SLATE_CONFIG_API_SELFTEST

static bool config_report_has(const slate_config_report_t *report,
                              const char *code, const char *path)
{
    for (size_t i = 0; i < report->error_count; i++) {
        if (strcmp(report->errors[i].code, code) == 0 &&
            strcmp(report->errors[i].path, path) == 0) {
            return true;
        }
    }
    return false;
}

static slate_config_parse_status_t parse_scene_fixture(const char *size,
                                                       const char *bindings,
                                                       slate_config_report_t *report)
{
    static const char PREFIX[] =
        "{\"schema\":1,\"theme\":\"midnight\",\"home_page\":\"home\","
        "\"pages\":[{\"id\":\"home\",\"tiles\":[{\"id\":\"scene\","
        "\"type\":\"scene\",\"pos\":[0,0],\"size\":";
    static const char MIDDLE[] = ",\"bindings\":";
    static const char SUFFIX[] = "}]}]}";
    size_t len = strlen(PREFIX) + strlen(size) + strlen(MIDDLE) +
                 strlen(bindings) + strlen(SUFFIX);
    char *json = malloc(len + 1);
    if (json == NULL) {
        memset(report, 0, sizeof(*report));
        report->status = SLATE_CONFIG_PARSE_OUT_OF_MEMORY;
        return report->status;
    }
    snprintf(json, len + 1, "%s%s%s%s%s", PREFIX, size, MIDDLE, bindings, SUFFIX);
    slate_config_t *config = NULL;
    slate_config_parse_status_t status = slate_config_parse(json, len, &config, report);
    slate_config_free(config);
    free(json);
    return status;
}

static slate_config_parse_status_t parse_cover_fixture(const char *size,
                                                       slate_config_report_t *report)
{
    static const char PREFIX[] =
        "{\"schema\":1,\"theme\":\"midnight\",\"home_page\":\"home\","
        "\"pages\":[{\"id\":\"home\",\"tiles\":[{\"id\":\"cover\","
        "\"type\":\"cover\",\"pos\":[0,0],\"size\":";
    static const char SUFFIX[] =
        ",\"binding\":{\"provider\":\"direct\",\"resource\":\"blind\"}}]}]}";
    size_t len = strlen(PREFIX) + strlen(size) + strlen(SUFFIX);
    char *json = malloc(len + 1);
    if (json == NULL) {
        memset(report, 0, sizeof(*report));
        report->status = SLATE_CONFIG_PARSE_OUT_OF_MEMORY;
        return report->status;
    }
    snprintf(json, len + 1, "%s%s%s", PREFIX, size, SUFFIX);
    slate_config_t *config = NULL;
    slate_config_parse_status_t status = slate_config_parse(json, len, &config, report);
    slate_config_free(config);
    free(json);
    return status;
}

esp_err_t slate_config_api_selftest(void)
{
    int failures = 0;
#define CONFIG_API_CHECK(condition, name)                                      \
    do {                                                                       \
        bool passed_ = (condition);                                            \
        failures += !passed_;                                                  \
        ESP_LOGI(TAG, "selftest: %-34s %s", name, passed_ ? "PASS" : "FAIL"); \
    } while (0)

    CONFIG_API_CHECK(parse_put_query(NULL, 0) == CONFIG_PUT_PERSISTENT,
                     "missing query is persistent");
    CONFIG_API_CHECK(parse_put_query("transient=1", strlen("transient=1")) ==
                         CONFIG_PUT_TRANSIENT,
                     "exact transient query accepted");
    CONFIG_API_CHECK(parse_put_query("transient=true", strlen("transient=true")) ==
                         CONFIG_PUT_INVALID_QUERY,
                     "alternate transient value refused");
    CONFIG_API_CHECK(parse_put_query("transient=1&x=1", strlen("transient=1&x=1")) ==
                         CONFIG_PUT_INVALID_QUERY,
                     "extra query field refused");
    CONFIG_API_CHECK(parse_put_query("transient=0&transient=1",
                                     strlen("transient=0&transient=1")) ==
                         CONFIG_PUT_INVALID_QUERY,
                     "duplicate transient field refused");

    static const char ONE[] =
        "[{\"provider\":\"direct\",\"resource\":\"scene-one\"}]";
    static const char TWO[] =
        "[{\"provider\":\"direct\",\"resource\":\"scene-one\"},"
        "{\"provider\":\"direct\",\"resource\":\"scene-two\"}]";
    static const char FIVE[] =
        "[{\"provider\":\"direct\",\"resource\":\"scene-one\"},"
        "{\"provider\":\"direct\",\"resource\":\"scene-two\"},"
        "{\"provider\":\"direct\",\"resource\":\"scene-three\"},"
        "{\"provider\":\"direct\",\"resource\":\"scene-four\"},"
        "{\"provider\":\"direct\",\"resource\":\"scene-five\"}]";
    static const char SIX[] =
        "[{\"provider\":\"direct\",\"resource\":\"scene-one\"},"
        "{\"provider\":\"direct\",\"resource\":\"scene-two\"},"
        "{\"provider\":\"direct\",\"resource\":\"scene-three\"},"
        "{\"provider\":\"direct\",\"resource\":\"scene-four\"},"
        "{\"provider\":\"direct\",\"resource\":\"scene-five\"},"
        "{\"provider\":\"direct\",\"resource\":\"scene-six\"}]";
    slate_config_report_t report;
    CONFIG_API_CHECK(parse_scene_fixture("[1,1]", ONE, &report) ==
                         SLATE_CONFIG_PARSE_OK,
                     "1x1 scene accepts one binding");
    slate_config_report_free(&report);
    CONFIG_API_CHECK(parse_scene_fixture("[4,1]", TWO, &report) ==
                         SLATE_CONFIG_PARSE_OK,
                     "4x1 scene accepts two bindings");
    slate_config_report_free(&report);
    CONFIG_API_CHECK(parse_scene_fixture("[4,1]", FIVE, &report) ==
                         SLATE_CONFIG_PARSE_OK,
                     "4x1 scene accepts five bindings");
    slate_config_report_free(&report);
    CONFIG_API_CHECK(parse_scene_fixture("[1,1]", TWO, &report) ==
                         SLATE_CONFIG_PARSE_INVALID_CONFIG &&
                         config_report_has(&report, "binding_required",
                                           "/pages/0/tiles/0/bindings"),
                     "1x1 scene refuses multiple bindings");
    slate_config_report_free(&report);
    CONFIG_API_CHECK(parse_scene_fixture("[4,1]", ONE, &report) ==
                         SLATE_CONFIG_PARSE_INVALID_CONFIG &&
                         config_report_has(&report, "binding_required",
                                           "/pages/0/tiles/0/bindings"),
                     "4x1 scene refuses one binding");
    slate_config_report_free(&report);
    CONFIG_API_CHECK(parse_scene_fixture("[4,1]", SIX, &report) ==
                         SLATE_CONFIG_PARSE_INVALID_CONFIG &&
                         config_report_has(&report, "binding_required",
                                           "/pages/0/tiles/0/bindings"),
                     "4x1 scene refuses six bindings");
    slate_config_report_free(&report);
    CONFIG_API_CHECK(parse_scene_fixture("[2,1]", "[]", &report) ==
                         SLATE_CONFIG_PARSE_INVALID_CONFIG &&
                         config_report_has(&report, "invalid_size",
                                           "/pages/0/tiles/0/size") &&
                         config_report_has(&report, "binding_required",
                                           "/pages/0/tiles/0/bindings"),
                     "scene reports size and binding errors together");
    slate_config_report_free(&report);

    static const char *const COVER_SIZES[] = {"[1,1]", "[1,2]", "[2,1]"};
    bool cover_sizes_ok = true;
    for (size_t i = 0; i < sizeof(COVER_SIZES) / sizeof(COVER_SIZES[0]); i++) {
        slate_config_parse_status_t status =
            parse_cover_fixture(COVER_SIZES[i], &report);
        cover_sizes_ok = cover_sizes_ok && status == SLATE_CONFIG_PARSE_OK;
        slate_config_report_free(&report);
    }
    CONFIG_API_CHECK(cover_sizes_ok, "cover accepts its three layouts");
    CONFIG_API_CHECK(parse_cover_fixture("[2,2]", &report) ==
                             SLATE_CONFIG_PARSE_INVALID_CONFIG &&
                         config_report_has(&report, "invalid_size",
                                           "/pages/0/tiles/0/size"),
                     "cover refuses 2x2 layout");
    slate_config_report_free(&report);
    CONFIG_API_CHECK(parse_cover_fixture("[4,1]", &report) ==
                             SLATE_CONFIG_PARSE_INVALID_CONFIG &&
                         config_report_has(&report, "invalid_size",
                                           "/pages/0/tiles/0/size"),
                     "cover refuses 4x1 layout");
    slate_config_report_free(&report);

    ESP_LOGI(TAG, "selftest: %d failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
#undef CONFIG_API_CHECK
}

#endif
