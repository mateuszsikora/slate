/* Home Assistant subscribe_entities diff expansion and neutral state mapping. */

#include "slate_ha_entities.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "slate_ha.h"
#include "slate_state.h"

#define HA_STATE_MAX        31
#define HA_DEVICE_CLASS_MAX 31

static const char *TAG = "slate_ha_entities";

typedef struct {
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    bool present;
    bool has_state;
    bool raw_state_numeric;
    double raw_state_number;
    char raw_state[HA_STATE_MAX + 1];

    bool has_friendly_name;
    char friendly_name[SLATE_RESOURCE_NAME_MAX + 1];
    bool has_unit;
    char unit[SLATE_SENSOR_UNIT_MAX + 1];
    bool has_device_class;
    char device_class[HA_DEVICE_CLASS_MAX + 1];

    bool has_brightness;
    double brightness;
    bool has_color_temp_kelvin;
    double color_temp_kelvin;
    bool has_color_temp_mired;
    double color_temp_mired;
    bool has_min_color_temp_kelvin;
    double min_color_temp_kelvin;
    bool has_max_color_temp_kelvin;
    double max_color_temp_kelvin;
    bool has_min_mireds;
    double min_mireds;
    bool has_max_mireds;
    double max_mireds;
    bool has_supported_color_modes;
    bool brightness_mode;
    bool color_temp_mode;

    char normalized_name[SLATE_RESOURCE_NAME_MAX + 1];
    slate_capabilities_t normalized_capabilities;
    slate_light_state_t normalized_light;
    slate_sensor_state_t normalized_sensor;
    bool normalized_light_ready;
    bool normalized_sensor_ready;
} ha_entity_t;

static SemaphoreHandle_t s_lock;
static ha_entity_t *s_entities;
static size_t s_count;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static bool resource_ok(const char *resource)
{
    return resource != NULL && resource[0] != '\0' &&
           strnlen(resource, SLATE_RESOURCE_ID_MAX + 1) <= SLATE_RESOURCE_ID_MAX;
}

static ha_entity_t *find_entity(const char *resource)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_entities[i].resource, resource) == 0) {
            return &s_entities[i];
        }
    }
    return NULL;
}

static bool parse_sensor_number(const char *text, double *out);

static bool number_value(const cJSON *item, double *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

static void clear_raw_attributes(ha_entity_t *entity)
{
    entity->has_friendly_name = false;
    entity->has_unit = false;
    entity->has_device_class = false;
    entity->has_brightness = false;
    entity->has_color_temp_kelvin = false;
    entity->has_color_temp_mired = false;
    entity->has_min_color_temp_kelvin = false;
    entity->has_max_color_temp_kelvin = false;
    entity->has_min_mireds = false;
    entity->has_max_mireds = false;
    entity->has_supported_color_modes = false;
    entity->brightness_mode = false;
    entity->color_temp_mode = false;
}

static void set_supported_color_modes(ha_entity_t *entity, const cJSON *item)
{
    if (!cJSON_IsArray(item)) {
        entity->has_supported_color_modes = false;
        entity->brightness_mode = false;
        entity->color_temp_mode = false;
        return;
    }

    entity->has_supported_color_modes = true;
    entity->brightness_mode = false;
    entity->color_temp_mode = false;
    const cJSON *mode = NULL;
    cJSON_ArrayForEach(mode, item) {
        if (!cJSON_IsString(mode)) {
            continue;
        }
        if (strcmp(mode->valuestring, "color_temp") == 0) {
            entity->color_temp_mode = true;
            entity->brightness_mode = true;
        } else if (strcmp(mode->valuestring, "onoff") != 0 &&
                   strcmp(mode->valuestring, "unknown") != 0) {
            entity->brightness_mode = true;
        }
    }
}

static void set_attribute(ha_entity_t *entity, const char *name, const cJSON *item)
{
    if (strcmp(name, "friendly_name") == 0) {
        entity->has_friendly_name = cJSON_IsString(item);
        if (entity->has_friendly_name) {
            strlcpy(entity->friendly_name, item->valuestring,
                    sizeof(entity->friendly_name));
        } else {
            entity->normalized_name[0] = '\0';
        }
    } else if (strcmp(name, "unit_of_measurement") == 0) {
        entity->has_unit = cJSON_IsString(item);
        if (entity->has_unit) {
            strlcpy(entity->unit, item->valuestring, sizeof(entity->unit));
        }
    } else if (strcmp(name, "device_class") == 0) {
        entity->has_device_class = cJSON_IsString(item);
        if (entity->has_device_class) {
            strlcpy(entity->device_class, item->valuestring,
                    sizeof(entity->device_class));
        }
    } else if (strcmp(name, "brightness") == 0) {
        entity->has_brightness = number_value(item, &entity->brightness);
    } else if (strcmp(name, "color_temp_kelvin") == 0) {
        entity->has_color_temp_kelvin = number_value(item, &entity->color_temp_kelvin);
    } else if (strcmp(name, "color_temp") == 0) {
        entity->has_color_temp_mired = number_value(item, &entity->color_temp_mired);
    } else if (strcmp(name, "min_color_temp_kelvin") == 0) {
        entity->has_min_color_temp_kelvin =
            number_value(item, &entity->min_color_temp_kelvin);
    } else if (strcmp(name, "max_color_temp_kelvin") == 0) {
        entity->has_max_color_temp_kelvin =
            number_value(item, &entity->max_color_temp_kelvin);
    } else if (strcmp(name, "min_mireds") == 0) {
        entity->has_min_mireds = number_value(item, &entity->min_mireds);
    } else if (strcmp(name, "max_mireds") == 0) {
        entity->has_max_mireds = number_value(item, &entity->max_mireds);
    } else if (strcmp(name, "supported_color_modes") == 0) {
        set_supported_color_modes(entity, item);
    }
}

static void remove_attribute(ha_entity_t *entity, const char *name)
{
    if (strcmp(name, "friendly_name") == 0) {
        entity->has_friendly_name = false;
        entity->normalized_name[0] = '\0';
    } else if (strcmp(name, "unit_of_measurement") == 0) {
        entity->has_unit = false;
    } else if (strcmp(name, "device_class") == 0) {
        entity->has_device_class = false;
    } else if (strcmp(name, "brightness") == 0) {
        entity->has_brightness = false;
    } else if (strcmp(name, "color_temp_kelvin") == 0) {
        entity->has_color_temp_kelvin = false;
    } else if (strcmp(name, "color_temp") == 0) {
        entity->has_color_temp_mired = false;
    } else if (strcmp(name, "min_color_temp_kelvin") == 0) {
        entity->has_min_color_temp_kelvin = false;
    } else if (strcmp(name, "max_color_temp_kelvin") == 0) {
        entity->has_max_color_temp_kelvin = false;
    } else if (strcmp(name, "min_mireds") == 0) {
        entity->has_min_mireds = false;
    } else if (strcmp(name, "max_mireds") == 0) {
        entity->has_max_mireds = false;
    } else if (strcmp(name, "supported_color_modes") == 0) {
        entity->has_supported_color_modes = false;
        entity->brightness_mode = false;
        entity->color_temp_mode = false;
    }
}

static void apply_attributes(ha_entity_t *entity, const cJSON *attributes, bool replace)
{
    if (replace) {
        clear_raw_attributes(entity);
    }
    if (!cJSON_IsObject(attributes)) {
        return;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, attributes) {
        if (item->string != NULL) {
            set_attribute(entity, item->string, item);
        }
    }
}

static void apply_state_fields(ha_entity_t *entity, const cJSON *fields, bool replace)
{
    if (replace) {
        entity->present = true;
        entity->has_state = false;
        entity->raw_state_numeric = false;
        entity->normalized_name[0] = '\0';
        clear_raw_attributes(entity);
    }
    if (!cJSON_IsObject(fields)) {
        return;
    }

    const cJSON *state = cJSON_GetObjectItemCaseSensitive(fields, "s");
    if (state != NULL) {
        entity->has_state = cJSON_IsString(state);
        entity->raw_state_numeric = false;
        if (entity->has_state) {
            entity->raw_state_numeric =
                parse_sensor_number(state->valuestring, &entity->raw_state_number);
            strlcpy(entity->raw_state, state->valuestring, sizeof(entity->raw_state));
        }
    }
    const cJSON *attributes = cJSON_GetObjectItemCaseSensitive(fields, "a");
    if (attributes != NULL) {
        apply_attributes(entity, attributes, replace);
    }
}

static void apply_removals(ha_entity_t *entity, const cJSON *removals)
{
    if (!cJSON_IsObject(removals)) {
        return;
    }
    const cJSON *attributes = cJSON_GetObjectItemCaseSensitive(removals, "a");
    if (!cJSON_IsArray(attributes)) {
        return;
    }
    const cJSON *name = NULL;
    cJSON_ArrayForEach(name, attributes) {
        if (cJSON_IsString(name)) {
            remove_attribute(entity, name->valuestring);
        }
    }
}

static bool domain_is(const char *resource, const char *domain)
{
    size_t len = strlen(domain);
    return strncmp(resource, domain, len) == 0 && resource[len] == '.' &&
           resource[len + 1] != '\0';
}

static int16_t rounded_i16(double value)
{
    if (!isfinite(value) || value <= 0 || value > INT16_MAX) {
        return SLATE_STATE_ABSENT;
    }
    return (int16_t) lround(value);
}

static int16_t mired_to_kelvin(double value)
{
    return value > 0 ? rounded_i16(1000000.0 / value) : SLATE_STATE_ABSENT;
}

static slate_measurement_t measurement(const ha_entity_t *entity)
{
    if (!entity->has_device_class) {
        return SLATE_MEASUREMENT_NONE;
    }
    if (strcmp(entity->device_class, "temperature") == 0) {
        return SLATE_MEASUREMENT_TEMPERATURE;
    }
    if (strcmp(entity->device_class, "humidity") == 0) {
        return SLATE_MEASUREMENT_HUMIDITY;
    }
    if (strcmp(entity->device_class, "pressure") == 0) {
        return SLATE_MEASUREMENT_PRESSURE;
    }
    if (strcmp(entity->device_class, "power") == 0) {
        return SLATE_MEASUREMENT_POWER;
    }
    return SLATE_MEASUREMENT_NONE;
}

static bool parse_sensor_number(const char *text, double *out)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    double value = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' || !isfinite(value)) {
        return false;
    }
    *out = value;
    return true;
}

/* Mutates the last-normalized fields, then returns a self-contained copy. */
static bool normalize_entity(ha_entity_t *entity, ha_entity_t *out,
                             slate_kind_t *kind, bool *available)
{
    if (entity->has_friendly_name) {
        strlcpy(entity->normalized_name, entity->friendly_name,
                sizeof(entity->normalized_name));
    }

    if (domain_is(entity->resource, "light")) {
        *kind = SLATE_KIND_LIGHT;
        bool current = entity->present && entity->has_state &&
                       (strcmp(entity->raw_state, "on") == 0 ||
                        strcmp(entity->raw_state, "off") == 0);
        if (!entity->normalized_light_ready) {
            entity->normalized_light.brightness = SLATE_STATE_ABSENT;
            entity->normalized_light.color_temperature = SLATE_STATE_ABSENT;
            entity->normalized_light_ready = true;
        }
        if (current) {
            entity->normalized_light.on = strcmp(entity->raw_state, "on") == 0;
            entity->normalized_light.brightness =
                entity->has_brightness && entity->brightness >= 0 && entity->brightness <= 255
                    ? (int16_t) lround(entity->brightness * 100.0 / 255.0)
                    : SLATE_STATE_ABSENT;
            if (entity->has_color_temp_kelvin) {
                entity->normalized_light.color_temperature =
                    rounded_i16(entity->color_temp_kelvin);
            } else if (entity->has_color_temp_mired) {
                entity->normalized_light.color_temperature =
                    mired_to_kelvin(entity->color_temp_mired);
            } else {
                entity->normalized_light.color_temperature = SLATE_STATE_ABSENT;
            }

            slate_capabilities_t caps = {
                .actions = (uint16_t) ((1u << SLATE_ACTION_TOGGLE) |
                                       (1u << SLATE_ACTION_SET_POWER)),
            };
            if (entity->brightness_mode ||
                (!entity->has_supported_color_modes && entity->has_brightness)) {
                caps.actions |= (uint16_t) (1u << SLATE_ACTION_SET_BRIGHTNESS);
                caps.brightness_max = 100;
            }
            if (entity->color_temp_mode || entity->has_color_temp_kelvin ||
                entity->has_color_temp_mired) {
                caps.actions |= (uint16_t) (1u << SLATE_ACTION_SET_COLOR_TEMPERATURE);
                if (entity->has_min_color_temp_kelvin &&
                    entity->has_max_color_temp_kelvin) {
                    caps.color_temperature_min = rounded_i16(entity->min_color_temp_kelvin);
                    caps.color_temperature_max = rounded_i16(entity->max_color_temp_kelvin);
                } else if (entity->has_min_mireds && entity->has_max_mireds) {
                    caps.color_temperature_min = mired_to_kelvin(entity->max_mireds);
                    caps.color_temperature_max = mired_to_kelvin(entity->min_mireds);
                }
                if (caps.color_temperature_min == SLATE_STATE_ABSENT ||
                    caps.color_temperature_max == SLATE_STATE_ABSENT ||
                    caps.color_temperature_min > caps.color_temperature_max) {
                    caps.color_temperature_min = 0;
                    caps.color_temperature_max = 0;
                }
            }
            entity->normalized_capabilities = caps;
        }
        *available = current;
    } else if (domain_is(entity->resource, "sensor")) {
        *kind = SLATE_KIND_SENSOR;
        bool current = entity->present && entity->has_state &&
                       strcmp(entity->raw_state, "unavailable") != 0;
        if (!entity->normalized_sensor_ready) {
            strlcpy(entity->normalized_sensor.text, "unknown",
                    sizeof(entity->normalized_sensor.text));
            entity->normalized_sensor_ready = true;
        }
        if (current) {
            slate_sensor_state_t sensor = {0};
            sensor.numeric = entity->raw_state_numeric;
            if (sensor.numeric) {
                sensor.value = entity->raw_state_number;
            }
            if (!sensor.numeric) {
                strlcpy(sensor.text, entity->raw_state, sizeof(sensor.text));
                if (sensor.text[0] == '\0') {
                    strlcpy(sensor.text, "unknown", sizeof(sensor.text));
                }
            }
            if (entity->has_unit) {
                strlcpy(sensor.unit, entity->unit, sizeof(sensor.unit));
            }
            sensor.measurement = measurement(entity);
            entity->normalized_sensor = sensor;
            entity->normalized_capabilities = (slate_capabilities_t) {0};
        }
        *available = current;
    } else {
        return false;
    }

    *out = *entity;
    return true;
}

static esp_err_t publish_copy(const ha_entity_t *entity, slate_kind_t kind, bool available)
{
    slate_snapshot_t snapshot = {
        .resource = entity->resource,
        .kind = kind,
        .name = entity->normalized_name[0] != '\0' ? entity->normalized_name : NULL,
        .area = NULL,
        .available = available,
        .capabilities = entity->normalized_capabilities,
    };
    if (kind == SLATE_KIND_LIGHT) {
        snapshot.state.light = entity->normalized_light;
    } else {
        snapshot.state.sensor = entity->normalized_sensor;
    }

    esp_err_t err = slate_state_publish(SLATE_HA_PROVIDER_ID, &snapshot);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "discarding mapped state for %s: %s", entity->resource,
                 esp_err_to_name(err));
    }
    return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t update_one(const char *resource, const cJSON *fields,
                            const cJSON *removals, bool replace, bool removed,
                            bool publish)
{
    ha_entity_t copy;
    slate_kind_t kind = SLATE_KIND_SENSOR;
    bool available = false;

    LOCK();
    ha_entity_t *entity = find_entity(resource);
    if (entity == NULL) {
        UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    if (removed) {
        entity->present = false;
        entity->has_state = false;
        clear_raw_attributes(entity);
    } else {
        entity->present = true;
        apply_state_fields(entity, fields, replace);
        apply_removals(entity, removals);
    }
    bool supported = normalize_entity(entity, &copy, &kind, &available);
    UNLOCK();

    if (!supported || !publish) {
        return ESP_OK;
    }
    return publish_copy(&copy, kind, available);
}

static esp_err_t process_event(const cJSON *event, bool publish)
{
    if (!cJSON_IsObject(event)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t first_error = ESP_OK;

    const cJSON *added = cJSON_GetObjectItemCaseSensitive(event, "a");
    if (added != NULL && !cJSON_IsObject(added)) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, added) {
        if (item->string == NULL || !cJSON_IsObject(item)) {
            continue;
        }
        esp_err_t err = update_one(item->string, item, NULL, true, false, publish);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND && first_error == ESP_OK) {
            first_error = err;
        }
    }

    const cJSON *changed = cJSON_GetObjectItemCaseSensitive(event, "c");
    if (changed != NULL && !cJSON_IsObject(changed)) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_ArrayForEach(item, changed) {
        if (item->string == NULL || !cJSON_IsObject(item)) {
            continue;
        }
        const cJSON *plus = cJSON_GetObjectItemCaseSensitive(item, "+");
        const cJSON *minus = cJSON_GetObjectItemCaseSensitive(item, "-");
        esp_err_t err = update_one(item->string, plus, minus, false, false, publish);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND && first_error == ESP_OK) {
            first_error = err;
        }
    }

    const cJSON *removed = cJSON_GetObjectItemCaseSensitive(event, "r");
    if (removed != NULL && !cJSON_IsArray(removed)) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_ArrayForEach(item, removed) {
        if (!cJSON_IsString(item)) {
            continue;
        }
        esp_err_t err = update_one(item->valuestring, NULL, NULL, false, true, publish);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND && first_error == ESP_OK) {
            first_error = err;
        }
    }
    return first_error;
}

esp_err_t slate_ha_entities_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t slate_ha_entities_bind(const char *const *resources, size_t count,
                                 bool *changed)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (changed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *changed = false;
    if (count > SLATE_STATE_MAX_RESOURCES || (count > 0 && resources == NULL)) {
        return count > SLATE_STATE_MAX_RESOURCES ? ESP_ERR_INVALID_SIZE
                                                 : ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; i++) {
        if (!resource_ok(resources[i])) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(resources[i], resources[j]) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    /* Membership is the contract. A layout-only rebuild may reorder the same
     * ids and must not interrupt a healthy HA subscription. */
    LOCK();
    bool same = count == s_count;
    for (size_t i = 0; same && i < count; i++) {
        same = find_entity(resources[i]) != NULL;
    }
    UNLOCK();
    if (same) {
        return ESP_OK;
    }

    ha_entity_t *fresh = NULL;
    if (count > 0) {
        fresh = heap_caps_calloc_prefer(count, sizeof(*fresh), 2,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (fresh == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    LOCK();
    for (size_t i = 0; i < count; i++) {
        strlcpy(fresh[i].resource, resources[i], sizeof(fresh[i].resource));
        ha_entity_t *previous = find_entity(resources[i]);
        if (previous != NULL) {
            char resource[sizeof(fresh[i].resource)];
            strlcpy(resource, fresh[i].resource, sizeof(resource));
            fresh[i] = *previous;
            strlcpy(fresh[i].resource, resource, sizeof(fresh[i].resource));
        }
    }
    ha_entity_t *old = s_entities;
    s_entities = fresh;
    s_count = count;
    UNLOCK();
    free(old);
    *changed = true;
    return ESP_OK;
}

esp_err_t slate_ha_entities_append_ids(cJSON *array, size_t *count)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!cJSON_IsArray(array) || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    LOCK();
    *count = s_count;
    for (size_t i = 0; i < s_count; i++) {
        cJSON *id = cJSON_CreateString(s_entities[i].resource);
        if (id == NULL || !cJSON_AddItemToArray(array, id)) {
            cJSON_Delete(id);
            err = ESP_ERR_NO_MEM;
            *count = 0;
            break;
        }
    }
    UNLOCK();
    return err;
}

esp_err_t slate_ha_entities_prepare_subscription(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    typedef struct {
        ha_entity_t entity;
        slate_kind_t kind;
        bool supported;
    } prepared_t;

    LOCK();
    size_t count = s_count;
    prepared_t *prepared = count > 0
                               ? heap_caps_calloc_prefer(
                                     count, sizeof(*prepared), 2,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                               : NULL;
    if (count > 0 && prepared == NULL) {
        UNLOCK();
        ESP_LOGW(TAG, "no memory to mark a new subscription unavailable");
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < count; i++) {
        s_entities[i].present = false;
        s_entities[i].has_state = false;
        clear_raw_attributes(&s_entities[i]);
        bool available = false;
        prepared[i].supported = normalize_entity(&s_entities[i], &prepared[i].entity,
                                                  &prepared[i].kind, &available);
    }
    UNLOCK();

    esp_err_t first_error = ESP_OK;
    for (size_t i = 0; i < count; i++) {
        if (prepared[i].supported) {
            esp_err_t err = publish_copy(&prepared[i].entity, prepared[i].kind, false);
            if (err != ESP_OK && first_error == ESP_OK) {
                first_error = err;
            }
        }
    }
    free(prepared);
    return first_error;
}

esp_err_t slate_ha_entities_process_event(const cJSON *event)
{
    return process_event(event, true);
}

#ifdef SLATE_HA_SELFTEST

esp_err_t slate_ha_entities_selftest(void)
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

    const char *ids[] = {"light.kitchen", "sensor.room_temperature"};
    bool binding_changed = false;
    CHECK(slate_ha_entities_bind(ids, 2, &binding_changed) == ESP_OK && binding_changed,
          "bind explicit entity ids");
    const char *reordered[] = {"sensor.room_temperature", "light.kitchen"};
    CHECK(slate_ha_entities_bind(reordered, 2, &binding_changed) == ESP_OK &&
              !binding_changed,
          "unchanged entity set avoids resubscription");
    const char *duplicates[] = {"light.kitchen", "light.kitchen"};
    CHECK(slate_ha_entities_bind(duplicates, 2, &binding_changed) == ESP_ERR_INVALID_ARG &&
              !binding_changed,
          "duplicate entity ids are refused");
    CHECK(slate_ha_entities_prepare_subscription() == ESP_OK,
          "prepare bound entities as unavailable");

    cJSON *initial = cJSON_Parse(
        "{\"a\":{\"light.kitchen\":{\"s\":\"on\",\"a\":{"
        "\"friendly_name\":\"Kitchen\",\"brightness\":128,"
        "\"supported_color_modes\":[\"brightness\"]}},"
        "\"sensor.room_temperature\":{\"s\":\"21.5\",\"a\":{"
        "\"friendly_name\":\"Room temperature\","
        "\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\"}}}}"
    );
    CHECK(initial != NULL && process_event(initial, false) == ESP_OK,
          "expand initial additions");
    cJSON_Delete(initial);

    ha_entity_t light;
    ha_entity_t sensor;
    slate_kind_t light_kind;
    slate_kind_t sensor_kind;
    bool light_available = false;
    bool sensor_available = false;
    LOCK();
    bool mapped_light = normalize_entity(find_entity(ids[0]), &light, &light_kind,
                                          &light_available);
    bool mapped_sensor = normalize_entity(find_entity(ids[1]), &sensor, &sensor_kind,
                                           &sensor_available);
    UNLOCK();
    CHECK(mapped_light && light_kind == SLATE_KIND_LIGHT && light_available &&
              light.normalized_light.on && light.normalized_light.brightness == 50 &&
              slate_capabilities_have(&light.normalized_capabilities,
                                      SLATE_ACTION_SET_BRIGHTNESS),
          "map light state and capability");
    CHECK(mapped_sensor && sensor_kind == SLATE_KIND_SENSOR && sensor_available &&
              sensor.normalized_sensor.numeric && sensor.normalized_sensor.value == 21.5 &&
              sensor.normalized_sensor.measurement == SLATE_MEASUREMENT_TEMPERATURE &&
              strcmp(sensor.normalized_sensor.unit, "°C") == 0,
          "map numeric sensor metadata");

    cJSON *change = cJSON_Parse(
        "{\"c\":{\"light.kitchen\":{\"+\":{\"s\":\"off\","
        "\"a\":{\"brightness\":0}},\"-\":{\"a\":[\"friendly_name\","
        "\"supported_color_modes\"]}}}}"
    );
    CHECK(change != NULL && process_event(change, false) == ESP_OK,
          "apply compressed additions and removals");
    cJSON_Delete(change);
    LOCK();
    mapped_light = normalize_entity(find_entity(ids[0]), &light, &light_kind,
                                    &light_available);
    UNLOCK();
    CHECK(mapped_light && !light.normalized_light.on &&
              light.normalized_light.brightness == 0 && light.normalized_name[0] == '\0',
          "partial light update keeps a complete snapshot");

    cJSON *long_number = cJSON_Parse(
        "{\"c\":{\"sensor.room_temperature\":{\"+\":{"
        "\"s\":\"1234567890123456789012345678901234567890\"}}}}"
    );
    CHECK(long_number != NULL && process_event(long_number, false) == ESP_OK,
          "accept long textual sensor state");
    cJSON_Delete(long_number);
    LOCK();
    mapped_sensor = normalize_entity(find_entity(ids[1]), &sensor, &sensor_kind,
                                     &sensor_available);
    UNLOCK();
    CHECK(mapped_sensor && sensor_available && sensor.normalized_sensor.numeric &&
              sensor.normalized_sensor.value > 1e39,
          "long numeric sensor state is parsed before text truncation");

    cJSON *restore_sensor = cJSON_Parse(
        "{\"c\":{\"sensor.room_temperature\":{\"+\":{\"s\":\"21.5\"}}}}"
    );
    CHECK(restore_sensor != NULL && process_event(restore_sensor, false) == ESP_OK,
          "restore numeric sensor fixture");
    cJSON_Delete(restore_sensor);

    cJSON *removed = cJSON_Parse("{\"r\":[\"sensor.room_temperature\"]}");
    CHECK(removed != NULL && process_event(removed, false) == ESP_OK,
          "expand entity removal");
    cJSON_Delete(removed);
    LOCK();
    mapped_sensor = normalize_entity(find_entity(ids[1]), &sensor, &sensor_kind,
                                     &sensor_available);
    UNLOCK();
    CHECK(mapped_sensor && !sensor_available && sensor.normalized_sensor.numeric &&
              sensor.normalized_sensor.value == 21.5,
          "removed entity keeps its last value unavailable");

    cJSON *bad_diff = cJSON_Parse("{\"c\":[]}");
    CHECK(bad_diff != NULL && process_event(bad_diff, false) == ESP_ERR_INVALID_ARG,
          "malformed compressed diff is refused");
    cJSON_Delete(bad_diff);

    CHECK(slate_ha_entities_bind(NULL, 0, &binding_changed) == ESP_OK && binding_changed,
          "empty set unsubscribes everything");

#undef CHECK
    ESP_LOGI(TAG, "selftest: %u failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif
