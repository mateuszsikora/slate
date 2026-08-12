/* Home Assistant action mapping. Provider-native names terminate in this file. */

#include "slate_ha_actions.h"

#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "slate_ha.h"

#ifdef SLATE_HA_SELFTEST
#include "esp_log.h"

static const char *TAG = "slate_ha_actions";
#endif

/* The common bus stops waiting after three seconds. Retain a command mapping
 * slightly longer so a result on the boundary can still be delivered; later
 * results are deliberately unknown and the common bus has already reverted. */
#define ACTION_CORRELATION_US (3500 * 1000LL)

typedef struct {
    uint32_t generation;
    uint32_t command_id;
    uint32_t bus_id;
    int64_t deadline_us;
} action_correlation_t;

static action_correlation_t *s_correlations;

typedef enum {
    HA_ACTION_DOMAIN_INVALID = 0,
    HA_ACTION_DOMAIN_LIGHT,
    HA_ACTION_DOMAIN_SCENE,
} ha_action_domain_t;

static ha_action_domain_t action_domain(const char *resource)
{
    if (resource == NULL || strnlen(resource, SLATE_RESOURCE_ID_MAX + 1) >
                                SLATE_RESOURCE_ID_MAX) {
        return HA_ACTION_DOMAIN_INVALID;
    }
    static const char light[] = "light.";
    if (strncmp(resource, light, sizeof(light) - 1) == 0 &&
        resource[sizeof(light) - 1] != '\0') {
        return HA_ACTION_DOMAIN_LIGHT;
    }
    static const char scene[] = "scene.";
    if (strncmp(resource, scene, sizeof(scene) - 1) == 0 &&
        resource[sizeof(scene) - 1] != '\0') {
        return HA_ACTION_DOMAIN_SCENE;
    }
    return HA_ACTION_DOMAIN_INVALID;
}

esp_err_t slate_ha_action_request_copy(slate_ha_action_request_t *out, uint32_t bus_id,
                                       const slate_action_request_t *request)
{
    if (out == NULL || bus_id == 0 || request == NULL ||
        request->provider == NULL || strcmp(request->provider, SLATE_HA_PROVIDER_ID) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ha_action_domain_t domain = action_domain(request->resource);
    if (domain == HA_ACTION_DOMAIN_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (request->action) {
    case SLATE_ACTION_TOGGLE:
        if (request->value_type != SLATE_ACTION_VALUE_NONE) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case SLATE_ACTION_SET_POWER:
        if (request->value_type != SLATE_ACTION_VALUE_BOOL) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case SLATE_ACTION_SET_BRIGHTNESS:
        if (request->value_type != SLATE_ACTION_VALUE_NUMBER ||
            request->value.number < 0 || request->value.number > 100) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case SLATE_ACTION_SET_COLOR_TEMPERATURE:
        if (request->value_type != SLATE_ACTION_VALUE_NUMBER ||
            request->value.number <= 0 || request->value.number > INT16_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case SLATE_ACTION_ACTIVATE:
        if (domain != HA_ACTION_DOMAIN_SCENE ||
            request->value_type != SLATE_ACTION_VALUE_NONE) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (domain == HA_ACTION_DOMAIN_SCENE && request->action != SLATE_ACTION_ACTIVATE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (domain == HA_ACTION_DOMAIN_LIGHT && request->action == SLATE_ACTION_ACTIVATE) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(out, 0, sizeof(*out));
    out->bus_id = bus_id;
    strlcpy(out->resource, request->resource, sizeof(out->resource));
    out->action = request->action;
    out->value_type = request->value_type;
    if (request->value_type == SLATE_ACTION_VALUE_BOOL) {
        out->value.boolean = request->value.boolean;
    } else if (request->value_type == SLATE_ACTION_VALUE_NUMBER) {
        out->value.number = request->value.number;
    }
    return ESP_OK;
}

esp_err_t slate_ha_action_frame(uint32_t command_id,
                                const slate_ha_action_request_t *request,
                                cJSON **out)
{
    if (command_id == 0 || request == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    ha_action_domain_t action_resource_domain = action_domain(request->resource);
    if (action_resource_domain == HA_ACTION_DOMAIN_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *domain = action_resource_domain == HA_ACTION_DOMAIN_SCENE ? "scene" : "light";
    const char *service = NULL;
    const char *value_name = NULL;
    int32_t value = 0;
    switch (request->action) {
    case SLATE_ACTION_TOGGLE:
        if (request->value_type != SLATE_ACTION_VALUE_NONE) {
            return ESP_ERR_INVALID_ARG;
        }
        service = "toggle";
        break;
    case SLATE_ACTION_SET_POWER:
        if (request->value_type != SLATE_ACTION_VALUE_BOOL) {
            return ESP_ERR_INVALID_ARG;
        }
        service = request->value.boolean ? "turn_on" : "turn_off";
        break;
    case SLATE_ACTION_SET_BRIGHTNESS:
        if (request->value_type != SLATE_ACTION_VALUE_NUMBER ||
            request->value.number < 0 || request->value.number > 100) {
            return ESP_ERR_INVALID_ARG;
        }
        service = "turn_on";
        value_name = "brightness_pct";
        value = request->value.number;
        break;
    case SLATE_ACTION_SET_COLOR_TEMPERATURE:
        if (request->value_type != SLATE_ACTION_VALUE_NUMBER ||
            request->value.number <= 0 || request->value.number > INT16_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        service = "turn_on";
        value_name = "color_temp_kelvin";
        value = request->value.number;
        break;
    case SLATE_ACTION_ACTIVATE:
        if (action_resource_domain != HA_ACTION_DOMAIN_SCENE ||
            request->value_type != SLATE_ACTION_VALUE_NONE) {
            return ESP_ERR_INVALID_ARG;
        }
        service = "turn_on";
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (action_resource_domain == HA_ACTION_DOMAIN_SCENE &&
        request->action != SLATE_ACTION_ACTIVATE) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *target = root != NULL ? cJSON_AddObjectToObject(root, "target") : NULL;
    bool ok = target != NULL &&
              cJSON_AddNumberToObject(root, "id", command_id) != NULL &&
              cJSON_AddStringToObject(root, "type", "call_service") != NULL &&
              cJSON_AddStringToObject(root, "domain", domain) != NULL &&
              cJSON_AddStringToObject(root, "service", service) != NULL &&
              cJSON_AddStringToObject(target, "entity_id", request->resource) != NULL;

    if (ok && value_name != NULL) {
        cJSON *service_data = cJSON_AddObjectToObject(root, "service_data");
        ok = service_data != NULL &&
             cJSON_AddNumberToObject(service_data, value_name, value) != NULL;
    }
    if (!ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    *out = root;
    return ESP_OK;
}

esp_err_t slate_ha_action_result_fields(const cJSON *root, bool *success,
                                        char *error, size_t error_len)
{
    if (!cJSON_IsObject(root) || success == NULL || error == NULL || error_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    error[0] = '\0';

    const cJSON *success_item = cJSON_GetObjectItemCaseSensitive(root, "success");
    if (!cJSON_IsBool(success_item)) {
        return ESP_ERR_INVALID_ARG;
    }
    *success = cJSON_IsTrue(success_item);
    if (*success) {
        return ESP_OK;
    }

    const cJSON *failure = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *code_item = cJSON_GetObjectItemCaseSensitive(failure, "code");
    const cJSON *message_item = cJSON_GetObjectItemCaseSensitive(failure, "message");
    const char *code = cJSON_IsString(code_item) ? code_item->valuestring : NULL;
    const char *message = cJSON_IsString(message_item) ? message_item->valuestring : NULL;

    /* S-4 measured read-only service denial as the generic code with the useful
     * fact only in the message. Keep that provider-specific trap here. */
    bool unauthorized = code != NULL && strcmp(code, "unauthorized") == 0;
    unauthorized = unauthorized ||
                   (code != NULL && strcmp(code, "home_assistant_error") == 0 &&
                    message != NULL && strcasecmp(message, "Unauthorized") == 0);
    strlcpy(error, unauthorized ? "unauthorized"
                                : code != NULL && code[0] != '\0' ? code
                                                                   : "unknown_error",
            error_len);
    return ESP_OK;
}

esp_err_t slate_ha_action_tracker_init(void)
{
    if (s_correlations != NULL) {
        return ESP_OK;
    }
    s_correlations = heap_caps_calloc_prefer(
        SLATE_STATE_MAX_RESOURCES, sizeof(*s_correlations), 2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return s_correlations != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void expire_at(int64_t now_us)
{
    for (size_t i = 0; i < SLATE_STATE_MAX_RESOURCES; i++) {
        if (s_correlations[i].command_id != 0 &&
            s_correlations[i].deadline_us <= now_us) {
            memset(&s_correlations[i], 0, sizeof(s_correlations[i]));
        }
    }
}

static esp_err_t track_at(uint32_t generation, uint32_t command_id,
                          uint32_t bus_id, int64_t now_us)
{
    if (s_correlations == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (generation == 0 || command_id == 0 || bus_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    expire_at(now_us);
    for (size_t i = 0; i < SLATE_STATE_MAX_RESOURCES; i++) {
        if (s_correlations[i].command_id == 0) {
            s_correlations[i] = (action_correlation_t) {
                .generation = generation,
                .command_id = command_id,
                .bus_id = bus_id,
                .deadline_us = now_us + ACTION_CORRELATION_US,
            };
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t slate_ha_action_track(uint32_t generation, uint32_t command_id,
                                uint32_t bus_id)
{
    return track_at(generation, command_id, bus_id, esp_timer_get_time());
}

static bool take_at(uint32_t generation, uint32_t command_id,
                    uint32_t *bus_id, int64_t now_us)
{
    if (s_correlations == NULL || bus_id == NULL || generation == 0 || command_id == 0) {
        return false;
    }
    expire_at(now_us);
    for (size_t i = 0; i < SLATE_STATE_MAX_RESOURCES; i++) {
        if (s_correlations[i].generation == generation &&
            s_correlations[i].command_id == command_id) {
            *bus_id = s_correlations[i].bus_id;
            memset(&s_correlations[i], 0, sizeof(s_correlations[i]));
            return true;
        }
    }
    return false;
}

bool slate_ha_action_take(uint32_t generation, uint32_t command_id,
                          uint32_t *bus_id)
{
    return take_at(generation, command_id, bus_id, esp_timer_get_time());
}

void slate_ha_action_clear(void)
{
    if (s_correlations != NULL) {
        memset(s_correlations, 0,
               SLATE_STATE_MAX_RESOURCES * sizeof(*s_correlations));
    }
}

#ifdef SLATE_HA_SELFTEST

static bool string_field(const cJSON *root, const char *name, const char *expected)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) && strcmp(item->valuestring, expected) == 0;
}

static bool number_field(const cJSON *root, const char *name, double expected)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(item) && item->valuedouble == expected;
}

esp_err_t slate_ha_actions_selftest(void)
{
    unsigned failures = 0;
#define CHECK(condition, name) do {                                                \
        if (condition) {                                                          \
            ESP_LOGI(TAG, "selftest PASS: %s", name);                            \
        } else {                                                                  \
            ESP_LOGE(TAG, "selftest FAIL: %s", name);                            \
            failures++;                                                          \
        }                                                                         \
    } while (0)

    slate_action_request_t source = {
        .provider = SLATE_HA_PROVIDER_ID,
        .resource = "light.slate_selftest",
        .action = SLATE_ACTION_TOGGLE,
        .value_type = SLATE_ACTION_VALUE_NONE,
    };
    slate_ha_action_request_t request;
    CHECK(slate_ha_action_request_copy(&request, 41, &source) == ESP_OK &&
              request.bus_id == 41 && strcmp(request.resource, source.resource) == 0,
          "copy borrowed semantic request");

    slate_action_request_t invalid = source;
    invalid.provider = "direct";
    CHECK(slate_ha_action_request_copy(&request, 45, &invalid) == ESP_ERR_INVALID_ARG,
          "reject request for another provider");
    invalid = source;
    invalid.resource = "switch.slate_selftest";
    CHECK(slate_ha_action_request_copy(&request, 45, &invalid) == ESP_ERR_INVALID_ARG,
          "reject unsupported resource domain");
    invalid = source;
    invalid.action = SLATE_ACTION_OPEN;
    CHECK(slate_ha_action_request_copy(&request, 45, &invalid) == ESP_ERR_NOT_SUPPORTED,
          "reject unsupported light action");

    cJSON *frame = NULL;
    CHECK(slate_ha_action_frame(101, &request, &frame) == ESP_OK &&
              number_field(frame, "id", 101) &&
              string_field(frame, "type", "call_service") &&
              string_field(frame, "domain", "light") &&
              string_field(frame, "service", "toggle") &&
              string_field(cJSON_GetObjectItemCaseSensitive(frame, "target"),
                           "entity_id", source.resource) &&
              cJSON_GetObjectItemCaseSensitive(frame, "service_data") == NULL,
          "toggle call_service frame");
    cJSON_Delete(frame);

    source.action = SLATE_ACTION_SET_POWER;
    source.value_type = SLATE_ACTION_VALUE_BOOL;
    source.value.boolean = false;
    CHECK(slate_ha_action_request_copy(&request, 42, &source) == ESP_OK &&
              slate_ha_action_frame(102, &request, &frame) == ESP_OK &&
              string_field(frame, "service", "turn_off"),
          "set_power false maps to turn_off");
    cJSON_Delete(frame);

    source.action = SLATE_ACTION_SET_BRIGHTNESS;
    source.value_type = SLATE_ACTION_VALUE_NUMBER;
    source.value.number = 62;
    CHECK(slate_ha_action_request_copy(&request, 43, &source) == ESP_OK &&
              slate_ha_action_frame(103, &request, &frame) == ESP_OK &&
              string_field(frame, "service", "turn_on") &&
              number_field(cJSON_GetObjectItemCaseSensitive(frame, "service_data"),
                           "brightness_pct", 62),
          "brightness maps to turn_on percentage");
    cJSON_Delete(frame);
    source.value.number = 101;
    CHECK(slate_ha_action_request_copy(&request, 45, &source) == ESP_ERR_INVALID_ARG,
          "reject brightness above percentage range");

    source.action = SLATE_ACTION_SET_COLOR_TEMPERATURE;
    source.value.number = 2700;
    CHECK(slate_ha_action_request_copy(&request, 44, &source) == ESP_OK &&
              slate_ha_action_frame(104, &request, &frame) == ESP_OK &&
              number_field(cJSON_GetObjectItemCaseSensitive(frame, "service_data"),
                           "color_temp_kelvin", 2700),
          "colour temperature maps to kelvin");
    cJSON_Delete(frame);
    source.value.number = 0;
    CHECK(slate_ha_action_request_copy(&request, 45, &source) == ESP_ERR_INVALID_ARG,
          "reject non-positive colour temperature");

    source.resource = "scene.relax";
    source.action = SLATE_ACTION_ACTIVATE;
    source.value_type = SLATE_ACTION_VALUE_NONE;
    CHECK(slate_ha_action_request_copy(&request, 46, &source) == ESP_OK &&
              slate_ha_action_frame(105, &request, &frame) == ESP_OK &&
              string_field(frame, "domain", "scene") &&
              string_field(frame, "service", "turn_on") &&
              string_field(cJSON_GetObjectItemCaseSensitive(frame, "target"),
                           "entity_id", source.resource) &&
              cJSON_GetObjectItemCaseSensitive(frame, "service_data") == NULL,
          "scene activation maps to scene.turn_on");
    cJSON_Delete(frame);
    source.action = SLATE_ACTION_TOGGLE;
    CHECK(slate_ha_action_request_copy(&request, 47, &source) == ESP_ERR_NOT_SUPPORTED,
          "reject light action for scene resource");

    cJSON *accepted = cJSON_Parse("{\"success\":true,\"result\":null}");
    bool success = false;
    char error[SLATE_HA_ACTION_ERROR_MAX + 1] = "stale";
    CHECK(accepted != NULL &&
              slate_ha_action_result_fields(accepted, &success, error, sizeof(error)) == ESP_OK &&
              success && error[0] == '\0',
          "successful HA result has no failure reason");
    cJSON_Delete(accepted);

    cJSON *denied = cJSON_Parse(
        "{\"success\":false,\"error\":{\"code\":\"home_assistant_error\","
        "\"message\":\"Unauthorized\"}}");
    success = true;
    CHECK(denied != NULL &&
              slate_ha_action_result_fields(denied, &success, error, sizeof(error)) == ESP_OK &&
              !success && strcmp(error, "unauthorized") == 0,
          "read-only denial taxonomy");
    cJSON_Delete(denied);

    cJSON *failed = cJSON_Parse(
        "{\"success\":false,\"error\":{\"code\":\"service_not_found\","
        "\"message\":\"not found\"}}");
    CHECK(failed != NULL &&
              slate_ha_action_result_fields(failed, &success, error, sizeof(error)) == ESP_OK &&
              !success && strcmp(error, "service_not_found") == 0,
          "ordinary HA failure code preserved");
    cJSON_Delete(failed);

    cJSON *malformed = cJSON_Parse("{\"success\":\"yes\"}");
    CHECK(malformed != NULL &&
              slate_ha_action_result_fields(malformed, &success, error, sizeof(error)) ==
                  ESP_ERR_INVALID_ARG,
          "reject malformed HA result");
    cJSON_Delete(malformed);

    slate_ha_action_clear();
    uint32_t bus_id = 0;
    CHECK(track_at(7, 201, 51, 1000) == ESP_OK &&
              !take_at(6, 201, &bus_id, 1100) &&
              take_at(7, 201, &bus_id, 1100) && bus_id == 51 &&
              !take_at(7, 201, &bus_id, 1100),
          "generation-safe one-shot correlation");
    CHECK(track_at(7, 202, 52, 1000) == ESP_OK &&
              !take_at(7, 202, &bus_id, 1000 + ACTION_CORRELATION_US),
          "late result correlation expires");
    slate_ha_action_clear();

    bool filled = true;
    for (uint32_t i = 1; i <= SLATE_STATE_MAX_RESOURCES; i++) {
        if (track_at(8, i, i, 2000) != ESP_OK) {
            filled = false;
            break;
        }
    }
    CHECK(filled && track_at(8, SLATE_STATE_MAX_RESOURCES + 1,
                             SLATE_STATE_MAX_RESOURCES + 1, 2000) == ESP_ERR_NO_MEM,
          "correlation table enforces the resource bound");
    slate_ha_action_clear();

#undef CHECK
    ESP_LOGI(TAG, "selftest: %u failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif
