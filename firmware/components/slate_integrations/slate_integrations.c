/*
 * Slate — integration management.
 *
 * The browser session is an administrator credential and never becomes a
 * secret a script has to keep. This component creates the narrower credential
 * scripts and Node-RED use for the External API instead. The plaintext is sent
 * once; slate_store persists only its SHA-256 digest.
 */

#include "slate_integrations.h"

#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"

#include "slate_api.h"
#include "slate_store.h"
#include "slate_ws.h"

#define KEY_BODY_MAX 96
#define KEY_QUERY_MAX 40

static esp_err_t read_body(httpd_req_t *req, char *out, size_t out_size)
{
    if (req->content_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t) req->content_len >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    while (received < (size_t) req->content_len) {
        int got = httpd_req_recv(req, out + received,
                                 (size_t) req->content_len - received);
        if (got <= 0) {
            return ESP_FAIL;
        }
        received += (size_t) got;
    }
    out[received] = '\0';
    return ESP_OK;
}

static esp_err_t list_keys(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *keys = root != NULL ? cJSON_AddArrayToObject(root, "keys") : NULL;
    bool ok = keys != NULL;

    size_t count = slate_store_integration_key_count();
    for (size_t i = 0; ok && i < count; i++) {
        slate_integration_key_info_t info;
        cJSON *item = cJSON_CreateObject();
        ok = slate_store_integration_key_at(i, &info) == ESP_OK && item != NULL &&
             cJSON_AddStringToObject(item, "id", info.id) != NULL &&
             cJSON_AddStringToObject(item, "name", info.name) != NULL &&
             cJSON_AddItemToArray(keys, item);
        if (!ok) {
            cJSON_Delete(item);
        }
    }

    if (!ok) {
        cJSON_Delete(root);
        return slate_api_send_json(req, NULL);
    }
    return slate_api_send_json(req, root);
}

static esp_err_t send_created(httpd_req_t *req,
                              const slate_integration_key_info_t *info,
                              char *token)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *token_item = cJSON_CreateStringReference(token);
    bool ok = root != NULL && token_item != NULL &&
              cJSON_AddStringToObject(root, "id", info->id) != NULL &&
              cJSON_AddStringToObject(root, "name", info->name) != NULL &&
              cJSON_AddItemToObject(root, "token", token_item);
    if (!ok) {
        cJSON_Delete(token_item);
        cJSON_Delete(root);
        explicit_bzero(token, SLATE_INTEGRATION_KEY_TOKEN_LEN + 1);
        return slate_api_send_json(req, NULL);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        explicit_bzero(token, SLATE_INTEGRATION_KEY_TOKEN_LEN + 1);
        return slate_api_send_json(req, NULL);
    }

    httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    explicit_bzero(body, strlen(body));
    cJSON_free(body);
    explicit_bzero(token, SLATE_INTEGRATION_KEY_TOKEN_LEN + 1);
    return err;
}

static esp_err_t create_key(httpd_req_t *req)
{
    char body[KEY_BODY_MAX] = {0};
    esp_err_t read_err = read_body(req, body, sizeof(body));
    if (read_err != ESP_OK) {
        explicit_bzero(body, sizeof(body));
        return slate_api_refuse(req,
                                read_err == ESP_ERR_INVALID_SIZE
                                    ? "413 Payload Too Large"
                                    : "400 Bad Request",
                                read_err == ESP_ERR_INVALID_SIZE ? "too_large"
                                                                 : "invalid_json");
    }

    cJSON *root = cJSON_Parse(body);
    explicit_bzero(body, sizeof(body));
    cJSON *name = cJSON_IsObject(root)
                      ? cJSON_GetObjectItemCaseSensitive(root, "name")
                      : NULL;
    if (!cJSON_IsString(name)) {
        cJSON_Delete(root);
        return slate_api_refuse(req, "400 Bad Request", "name_required");
    }

    slate_integration_key_info_t info = {0};
    char token[SLATE_INTEGRATION_KEY_TOKEN_LEN + 1] = {0};
    esp_err_t err = slate_store_integration_key_create(name->valuestring, &info,
                                                        token, sizeof(token));
    cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        explicit_bzero(token, sizeof(token));
        return slate_api_refuse(req, "400 Bad Request", "name_invalid");
    }
    if (err == ESP_ERR_NO_MEM) {
        explicit_bzero(token, sizeof(token));
        return slate_api_refuse(req, "409 Conflict", "key_limit");
    }
    if (err != ESP_OK) {
        explicit_bzero(token, sizeof(token));
        return slate_api_refuse(req, "500 Internal Server Error", "store_failed");
    }
    return send_created(req, &info, token);
}

static esp_err_t revoke_key(httpd_req_t *req)
{
    char query[KEY_QUERY_MAX] = {0};
    char id[SLATE_INTEGRATION_KEY_ID_LEN + 1] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id, sizeof(id)) != ESP_OK) {
        return slate_api_refuse(req, "400 Bad Request", "key_id_required");
    }

    esp_err_t err = slate_store_integration_key_revoke(id);
    if (err == ESP_ERR_INVALID_ARG) {
        return slate_api_refuse(req, "400 Bad Request", "key_id_invalid");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return slate_api_refuse(req, "404 Not Found", "key_not_found");
    }
    if (err != ESP_OK) {
        return slate_api_refuse(req, "500 Internal Server Error", "store_failed");
    }

    slate_ws_external_keys_changed();

    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t slate_integrations_init(void)
{
    static const httpd_uri_t list = {
        .uri = SLATE_API_BASE_PATH "/integration-keys",
        .method = HTTP_GET,
        .handler = list_keys,
    };
    static const httpd_uri_t create = {
        .uri = SLATE_API_BASE_PATH "/integration-keys",
        .method = HTTP_POST,
        .handler = create_key,
    };
    static const httpd_uri_t revoke = {
        .uri = SLATE_API_BASE_PATH "/integration-keys",
        .method = HTTP_DELETE,
        .handler = revoke_key,
    };

    esp_err_t err = slate_api_register_uri(&list, SLATE_API_AUTH_DEVICE_TOKEN);
    if (err == ESP_OK) {
        err = slate_api_register_uri(&create, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (err == ESP_OK) {
        err = slate_api_register_uri(&revoke, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    return err;
}
