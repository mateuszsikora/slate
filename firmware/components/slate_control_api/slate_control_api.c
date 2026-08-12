/* Slate — authenticated device-mode and recovery controls. */

#include "slate_control_api.h"

#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "slate_api.h"
#include "slate_display.h"
#include "slate_store.h"
#include "slate_ws.h"

static const char *TAG = "control_api";

#define MODE_BODY_MAX       64
#define FACTORY_REBOOT_US   (750 * 1000)

static esp_timer_handle_t s_reboot_timer;
static bool s_initialized;

static esp_err_t send_no_content(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t read_mode(httpd_req_t *req, slate_ws_mode_t *out, bool *accepted)
{
    *accepted = false;
    if (req->content_len == 0) {
        return slate_api_refuse(req, "400 Bad Request", "empty_body");
    }
    if (req->content_len > MODE_BODY_MAX) {
        return slate_api_refuse(req, "413 Payload Too Large", "too_large");
    }

    char body[MODE_BODY_MAX + 1];
    size_t received = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, body + received, req->content_len - received);
        if (chunk <= 0) {
            return slate_api_refuse(req, "400 Bad Request", "invalid_json");
        }
        received += (size_t) chunk;
    }
    body[received] = '\0';

    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(body, &end, true);
    cJSON *mode = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "mode") : NULL;
    bool valid_json = root != NULL && cJSON_IsObject(root) && end == body + received;
    bool valid_mode = valid_json && cJSON_IsString(mode);
    if (valid_mode && strcmp(mode->valuestring, "normal") == 0) {
        *out = SLATE_WS_MODE_NORMAL;
    } else if (valid_mode && strcmp(mode->valuestring, "edit") == 0) {
        *out = SLATE_WS_MODE_EDIT;
    } else {
        valid_mode = false;
    }
    cJSON_Delete(root);
    explicit_bzero(body, sizeof(body));

    if (!valid_json) {
        return slate_api_refuse(req, "400 Bad Request", "invalid_json");
    }
    if (!valid_mode) {
        return slate_api_refuse(req, "400 Bad Request", "invalid_mode");
    }
    *accepted = true;
    return ESP_OK;
}

static esp_err_t mode_handler(httpd_req_t *req)
{
    slate_ws_mode_t mode;
    bool accepted;
    esp_err_t err = read_mode(req, &mode, &accepted);
    if (!accepted) {
        return err;
    }
    err = slate_ws_mode_set(mode);
    if (err != ESP_OK) {
        return slate_api_refuse(req, "503 Service Unavailable", "mode_unavailable");
    }
    return send_no_content(req);
}

static esp_err_t identify_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        return slate_api_refuse(req, "400 Bad Request", "unexpected_body");
    }
    esp_err_t err = slate_display_identify();
    if (err != ESP_OK) {
        return slate_api_refuse(req, "503 Service Unavailable", "display_unavailable");
    }
    return send_no_content(req);
}

static void reboot(void *ctx)
{
    (void) ctx;
    esp_restart();
}

static void schedule_reboot(void)
{
    esp_err_t err = esp_timer_start_once(s_reboot_timer, FACTORY_REBOOT_US);
    if (err == ESP_OK) {
        return;
    }

    /* The store has invalidated handles held by other subsystems. Rebooting is
     * no longer optional, even if it costs the HTTP response. */
    ESP_LOGE(TAG, "scheduling reboot after factory reset: %s", esp_err_to_name(err));
    esp_restart();
}

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        return slate_api_refuse(req, "400 Bad Request", "unexpected_body");
    }

    esp_err_t reset_err = slate_store_factory_reset();
    /* Arm the reboot before touching response I/O. A peer that stopped reading
     * can block httpd_resp_send() up to the server's socket timeout, but cannot
     * be allowed to keep the device running with invalidated NVS handles. */
    schedule_reboot();

    if (reset_err == ESP_OK) {
        return send_no_content(req);
    }
    return slate_api_refuse(req, "500 Internal Server Error", "reset_failed");
}

esp_err_t slate_control_api_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = reboot,
        .name = "factory_reboot",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_reboot_timer);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t mode = {
        .uri = SLATE_API_BASE_PATH "/mode",
        .method = HTTP_POST,
        .handler = mode_handler,
    };
    const httpd_uri_t identify = {
        .uri = SLATE_API_BASE_PATH "/identify",
        .method = HTTP_POST,
        .handler = identify_handler,
    };
    const httpd_uri_t factory_reset = {
        .uri = SLATE_API_BASE_PATH "/factory_reset",
        .method = HTTP_POST,
        .handler = factory_reset_handler,
    };

    err = slate_api_register_uri(&mode, SLATE_API_AUTH_DEVICE_TOKEN);
    if (err == ESP_OK) {
        err = slate_api_register_uri(&identify, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (err == ESP_OK) {
        err = slate_api_register_uri(&factory_reset, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (err != ESP_OK) {
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "mode, identify and factory-reset controls ready");
    return ESP_OK;
}
