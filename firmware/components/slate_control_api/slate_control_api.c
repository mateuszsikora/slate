/* Slate — authenticated device-mode and recovery controls. */

#include "slate_control_api.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "slate_api.h"
#include "slate_display.h"
#include "slate_store.h"
#include "slate_touch.h"
#include "slate_ui.h"
#include "slate_ws.h"

static const char *TAG = "control_api";

#define MODE_BODY_MAX       64
#define FACTORY_REBOOT_US   (750 * 1000)
#define PANEL_RESET_HOLD_MS 10000
#define RESET_TASK_STACK    4096
#define RESET_TASK_PRIORITY 6

static esp_timer_handle_t s_reboot_timer;
static TaskHandle_t s_reset_task;
static atomic_bool s_resetting = ATOMIC_VAR_INIT(false);
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

static esp_err_t perform_factory_reset(void)
{
    /* Suppression is synchronous even though its system-bar redraw is queued,
     * so the held finger cannot become a provider action on its eventual lift. */
    slate_ui_mode_set(true);
    esp_err_t display_err = slate_display_factory_reset_show();
    if (display_err != ESP_OK) {
        ESP_LOGW(TAG, "factory-reset presentation: %s", esp_err_to_name(display_err));
    }

    esp_err_t reset_err = slate_store_factory_reset();
    /* The store has invalidated handles held by other subsystems. From here a
     * reboot is mandatory even when one erase or format reported a failure. */
    schedule_reboot();
    return reset_err;
}

static void reset_task(void *ctx)
{
    (void) ctx;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGW(TAG, "factory reset requested by a ten-second panel hold");
        esp_err_t err = perform_factory_reset();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "panel factory reset incomplete: %s", esp_err_to_name(err));
        }
    }
}

static void panel_hold_complete(void *ctx)
{
    (void) ctx;
    if (atomic_exchange_explicit(&s_resetting, true, memory_order_acq_rel)) {
        return;
    }
    /* Called by the input reader on LVGL's task. Every erase and filesystem
     * operation belongs on the dedicated worker, never on the renderer. */
    xTaskNotifyGive(s_reset_task);
}

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        return slate_api_refuse(req, "400 Bad Request", "unexpected_body");
    }

    if (atomic_exchange_explicit(&s_resetting, true, memory_order_acq_rel)) {
        return slate_api_refuse(req, "409 Conflict", "reset_in_progress");
    }
    esp_err_t reset_err = perform_factory_reset();

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

    if (xTaskCreate(reset_task, "slate_reset", RESET_TASK_STACK, NULL,
                    RESET_TASK_PRIORITY, &s_reset_task) != pdPASS) {
        esp_timer_delete(s_reboot_timer);
        s_reboot_timer = NULL;
        return ESP_ERR_NO_MEM;
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

    err = slate_touch_set_hold_observer(PANEL_RESET_HOLD_MS, panel_hold_complete, NULL);
    if (err != ESP_OK) {
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "mode, identify and factory-reset controls ready; panel hold %d s",
             PANEL_RESET_HOLD_MS / 1000);
    return ESP_OK;
}
