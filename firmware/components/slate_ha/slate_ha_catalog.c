/* Home Assistant picker catalog: registry joins end here, before the API. */

#include "slate_ha_catalog.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "slate_api.h"
#include "slate_ha_entities.h"
#include "slate_state.h"

#define HA_REGISTRY_ID_MAX 63

#ifdef SLATE_HA_SELFTEST
#include "esp_log.h"
static const char *TAG = "slate_ha_catalog";
#endif

typedef struct {
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    char device[HA_REGISTRY_ID_MAX + 1];
    char area[HA_REGISTRY_ID_MAX + 1];
} entity_meta_t;

typedef struct {
    char id[HA_REGISTRY_ID_MAX + 1];
    char area[HA_REGISTRY_ID_MAX + 1];
} device_meta_t;

typedef struct {
    char id[HA_REGISTRY_ID_MAX + 1];
    char name[SLATE_RESOURCE_AREA_MAX + 1];
} area_meta_t;

typedef struct {
    bool active;
    bool received[SLATE_HA_CATALOG_STAGE_COUNT];
    bool succeeded[SLATE_HA_CATALOG_STAGE_COUNT];
    entity_meta_t *entities;
    size_t entity_count;
    device_meta_t *devices;
    size_t device_count;
    area_meta_t *areas;
    size_t area_count;
    slate_resource_t *resources;
    size_t resource_count;
} refresh_t;

static SemaphoreHandle_t s_lock;
static refresh_t s_refresh;
static slate_resource_t *s_resources;
static size_t s_resource_count;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void *psram_calloc(size_t count, size_t size)
{
    if (count == 0) {
        return NULL;
    }
    return heap_caps_calloc_prefer(count, size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void refresh_free(refresh_t *refresh)
{
    free(refresh->entities);
    free(refresh->devices);
    free(refresh->areas);
    free(refresh->resources);
    *refresh = (refresh_t) {0};
}

static bool copy_required(char *out, size_t out_len, const cJSON *object,
                          const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring[0] == '\0' ||
        strnlen(item->valuestring, out_len) >= out_len) {
        return false;
    }
    strlcpy(out, item->valuestring, out_len);
    return true;
}

static void copy_optional(char *out, size_t out_len, const cJSON *object,
                          const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsString(item) &&
        strnlen(item->valuestring, out_len) < out_len) {
        strlcpy(out, item->valuestring, out_len);
    }
}

static bool copy_presentation(char *out, size_t out_len, const cJSON *object,
                              const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring[0] == '\0') {
        return false;
    }
    strlcpy(out, item->valuestring, out_len);
    return true;
}

static esp_err_t parse_entities(const cJSON *result, refresh_t *refresh)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(result, "entities");
    if (!cJSON_IsObject(result) || !cJSON_IsArray(array)) {
        return ESP_ERR_INVALID_ARG;
    }

    int capacity = cJSON_GetArraySize(array);
    entity_meta_t *entries = psram_calloc((size_t) capacity, sizeof(*entries));
    if (capacity > 0 && entries == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsObject(item) ||
            !copy_required(entries[count].resource, sizeof(entries[count].resource),
                           item, "ei")) {
            continue;
        }
        copy_optional(entries[count].device, sizeof(entries[count].device), item, "di");
        copy_optional(entries[count].area, sizeof(entries[count].area), item, "ai");
        count++;
    }
    refresh->entities = entries;
    refresh->entity_count = count;
    return ESP_OK;
}

static esp_err_t parse_devices(const cJSON *result, refresh_t *refresh)
{
    if (!cJSON_IsArray(result)) {
        return ESP_ERR_INVALID_ARG;
    }

    int capacity = cJSON_GetArraySize(result);
    device_meta_t *entries = psram_calloc((size_t) capacity, sizeof(*entries));
    if (capacity > 0 && entries == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, result) {
        if (!cJSON_IsObject(item) ||
            !copy_required(entries[count].id, sizeof(entries[count].id), item, "id")) {
            continue;
        }
        copy_optional(entries[count].area, sizeof(entries[count].area), item, "area_id");
        count++;
    }
    refresh->devices = entries;
    refresh->device_count = count;
    return ESP_OK;
}

static esp_err_t parse_areas(const cJSON *result, refresh_t *refresh)
{
    if (!cJSON_IsArray(result)) {
        return ESP_ERR_INVALID_ARG;
    }

    int capacity = cJSON_GetArraySize(result);
    area_meta_t *entries = psram_calloc((size_t) capacity, sizeof(*entries));
    if (capacity > 0 && entries == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, result) {
        if (!cJSON_IsObject(item) ||
            !copy_required(entries[count].id, sizeof(entries[count].id), item, "area_id") ||
            !copy_presentation(entries[count].name, sizeof(entries[count].name),
                               item, "name")) {
            continue;
        }
        count++;
    }
    refresh->areas = entries;
    refresh->area_count = count;
    return ESP_OK;
}

static esp_err_t parse_states(const cJSON *result, refresh_t *refresh)
{
    if (!cJSON_IsArray(result)) {
        return ESP_ERR_INVALID_ARG;
    }

    int capacity = cJSON_GetArraySize(result);
    slate_resource_t *resources = psram_calloc((size_t) capacity, sizeof(*resources));
    if (capacity > 0 && resources == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, result) {
        esp_err_t err = slate_ha_entities_normalize_full_state(item, &resources[count]);
        if (err == ESP_OK) {
            count++;
        } else if (err != ESP_ERR_NOT_SUPPORTED && err != ESP_ERR_INVALID_ARG) {
            free(resources);
            return err;
        }
    }
    refresh->resources = resources;
    refresh->resource_count = count;
    return ESP_OK;
}

static const entity_meta_t *find_entity(const refresh_t *refresh, const char *resource)
{
    for (size_t i = 0; i < refresh->entity_count; i++) {
        if (strcmp(refresh->entities[i].resource, resource) == 0) {
            return &refresh->entities[i];
        }
    }
    return NULL;
}

static const device_meta_t *find_device(const refresh_t *refresh, const char *id)
{
    for (size_t i = 0; i < refresh->device_count; i++) {
        if (strcmp(refresh->devices[i].id, id) == 0) {
            return &refresh->devices[i];
        }
    }
    return NULL;
}

static const area_meta_t *find_area(const refresh_t *refresh, const char *id)
{
    for (size_t i = 0; i < refresh->area_count; i++) {
        if (strcmp(refresh->areas[i].id, id) == 0) {
            return &refresh->areas[i];
        }
    }
    return NULL;
}

static void join_areas(refresh_t *refresh)
{
    for (size_t i = 0; i < refresh->resource_count; i++) {
        const entity_meta_t *entity = find_entity(refresh, refresh->resources[i].resource);
        if (entity == NULL) {
            continue;
        }

        const char *area_id = entity->area;
        if (area_id[0] == '\0' && entity->device[0] != '\0') {
            const device_meta_t *device = find_device(refresh, entity->device);
            area_id = device != NULL ? device->area : area_id;
        }
        const area_meta_t *area = area_id[0] != '\0' ? find_area(refresh, area_id) : NULL;
        if (area != NULL) {
            strlcpy(refresh->resources[i].area, area->name,
                    sizeof(refresh->resources[i].area));
        }
    }
}

esp_err_t slate_ha_catalog_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t slate_ha_catalog_begin(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    refresh_free(&s_refresh);
    s_refresh.active = true;
    UNLOCK();
    return ESP_OK;
}

esp_err_t slate_ha_catalog_accept(slate_ha_catalog_stage_t stage, bool success,
                                  const cJSON *result, bool *complete,
                                  bool *degraded)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (stage >= SLATE_HA_CATALOG_STAGE_COUNT || complete == NULL || degraded == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *complete = false;
    *degraded = false;

    LOCK();
    if (!s_refresh.active || s_refresh.received[stage]) {
        UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t parsed = ESP_FAIL;
    if (success) {
        switch (stage) {
        case SLATE_HA_CATALOG_ENTITIES:
            parsed = parse_entities(result, &s_refresh);
            break;
        case SLATE_HA_CATALOG_DEVICES:
            parsed = parse_devices(result, &s_refresh);
            break;
        case SLATE_HA_CATALOG_AREAS:
            parsed = parse_areas(result, &s_refresh);
            break;
        case SLATE_HA_CATALOG_STATES:
            parsed = parse_states(result, &s_refresh);
            break;
        default:
            parsed = ESP_ERR_INVALID_ARG;
            break;
        }
    }
    s_refresh.received[stage] = true;
    s_refresh.succeeded[stage] = parsed == ESP_OK;

    bool all_received = true;
    for (size_t i = 0; i < SLATE_HA_CATALOG_STAGE_COUNT; i++) {
        all_received = all_received && s_refresh.received[i];
    }
    if (!all_received) {
        UNLOCK();
        return parsed == ESP_OK || !success ? ESP_OK : parsed;
    }

    bool states_ok = s_refresh.succeeded[SLATE_HA_CATALOG_STATES];
    bool registries_ok = s_refresh.succeeded[SLATE_HA_CATALOG_ENTITIES] &&
                         s_refresh.succeeded[SLATE_HA_CATALOG_DEVICES] &&
                         s_refresh.succeeded[SLATE_HA_CATALOG_AREAS];
    if (states_ok) {
        if (registries_ok) {
            join_areas(&s_refresh);
        }
        slate_resource_t *old = s_resources;
        s_resources = s_refresh.resources;
        s_resource_count = s_refresh.resource_count;
        s_refresh.resources = NULL;
        s_refresh.resource_count = 0;
        free(old);
    }

    *complete = true;
    *degraded = !registries_ok;
    refresh_free(&s_refresh);
    UNLOCK();
    /* Registry failure is the supported flat fallback. State failure cannot
     * produce a fresh picker and deliberately leaves the previous cache. */
    return states_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t slate_ha_catalog_append(void *ctx, cJSON *array)
{
    (void) ctx;
    if (s_lock == NULL || !cJSON_IsArray(array)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    LOCK();
    for (size_t i = 0; i < s_resource_count; i++) {
        err = slate_api_resource_append(array, &s_resources[i]);
        if (err != ESP_OK) {
            break;
        }
    }
    UNLOCK();
    return err;
}

void slate_ha_catalog_clear(void)
{
    if (s_lock == NULL) {
        return;
    }
    LOCK();
    refresh_free(&s_refresh);
    free(s_resources);
    s_resources = NULL;
    s_resource_count = 0;
    UNLOCK();
}

#ifdef SLATE_HA_SELFTEST

static bool accept_fixture(slate_ha_catalog_stage_t stage, bool success,
                           const char *json, bool *complete, bool *degraded)
{
    cJSON *result = json != NULL ? cJSON_Parse(json) : NULL;
    esp_err_t err = slate_ha_catalog_accept(stage, success, result, complete, degraded);
    cJSON_Delete(result);
    return err == ESP_OK;
}

static bool string_field_is(const cJSON *object, const char *name,
                            const char *expected)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && strcmp(item->valuestring, expected) == 0;
}

esp_err_t slate_ha_catalog_selftest(void)
{
    static const char ENTITIES[] =
        "{\"entities\":["
        "{\"ei\":\"light.kitchen\",\"di\":\"dev-kitchen\",\"ai\":\"area-kitchen\"},"
        "{\"ei\":\"sensor.office_temperature\",\"di\":\"dev-office\"},"
        "{\"ei\":\"switch.unsupported\",\"di\":\"dev-office\"}]}";
    static const char DEVICES[] =
        "[{\"id\":\"dev-kitchen\",\"area_id\":\"area-wrong\"},"
        "{\"id\":\"dev-office\",\"area_id\":\"area-office\"}]";
    static const char AREAS[] =
        "[{\"area_id\":\"area-kitchen\",\"name\":\"Kitchen\"},"
        "{\"area_id\":\"area-office\",\"name\":\"Office\"},"
        "{\"area_id\":\"area-wrong\",\"name\":\"Wrong\"}]";
    static const char STATES[] =
        "[{\"entity_id\":\"light.kitchen\",\"state\":\"on\",\"attributes\":{"
        "\"friendly_name\":\"Kitchen light\",\"brightness\":128,"
        "\"supported_color_modes\":[\"brightness\"]}},"
        "{\"entity_id\":\"sensor.office_temperature\",\"state\":\"21.5\","
        "\"attributes\":{\"friendly_name\":\"Office temperature\","
        "\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\"}},"
        "{\"entity_id\":\"switch.unsupported\",\"state\":\"on\","
        "\"attributes\":{\"friendly_name\":\"Unsupported\"}}]";

    unsigned failures = 0;
#define CHECK(condition, name) do {                                                \
        if (condition) {                                                          \
            ESP_LOGI(TAG, "selftest PASS: %s", name);                            \
        } else {                                                                  \
            ESP_LOGE(TAG, "selftest FAIL: %s", name);                            \
            failures++;                                                          \
        }                                                                         \
    } while (0)

    bool complete = false;
    bool degraded = false;
    CHECK(slate_ha_catalog_begin() == ESP_OK &&
              accept_fixture(SLATE_HA_CATALOG_ENTITIES, true, ENTITIES,
                             &complete, &degraded) && !complete &&
              accept_fixture(SLATE_HA_CATALOG_DEVICES, true, DEVICES,
                             &complete, &degraded) && !complete &&
              accept_fixture(SLATE_HA_CATALOG_AREAS, true, AREAS,
                             &complete, &degraded) && !complete &&
              accept_fixture(SLATE_HA_CATALOG_STATES, true, STATES,
                             &complete, &degraded) && complete && !degraded,
          "assemble one area-aware discovery refresh");

    cJSON *area_aware = cJSON_CreateArray();
    CHECK(area_aware != NULL && slate_ha_catalog_append(NULL, area_aware) == ESP_OK &&
              cJSON_GetArraySize(area_aware) == 2 &&
              string_field_is(cJSON_GetArrayItem(area_aware, 0), "area", "Kitchen") &&
              string_field_is(cJSON_GetArrayItem(area_aware, 1), "area", "Office"),
          "entity area wins and device area supplies the fallback");
    cJSON_Delete(area_aware);

    complete = false;
    degraded = false;
    CHECK(slate_ha_catalog_begin() == ESP_OK &&
              accept_fixture(SLATE_HA_CATALOG_ENTITIES, false, NULL,
                             &complete, &degraded) && !complete &&
              accept_fixture(SLATE_HA_CATALOG_DEVICES, true, DEVICES,
                             &complete, &degraded) && !complete &&
              accept_fixture(SLATE_HA_CATALOG_AREAS, true, AREAS,
                             &complete, &degraded) && !complete &&
              accept_fixture(SLATE_HA_CATALOG_STATES, true, STATES,
                             &complete, &degraded) && complete && degraded,
          "registry failure completes as a flat catalog");

    cJSON *flat = cJSON_CreateArray();
    CHECK(flat != NULL && slate_ha_catalog_append(NULL, flat) == ESP_OK &&
              cJSON_GetArraySize(flat) == 2 &&
              cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(flat, 0), "area") == NULL &&
              cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(flat, 1), "area") == NULL &&
              string_field_is(cJSON_GetArrayItem(flat, 0), "kind", "light"),
          "flat fallback keeps normalized resource shape without area");
    cJSON_Delete(flat);

    complete = false;
    degraded = false;
    bool states_failed = slate_ha_catalog_begin() == ESP_OK &&
                         accept_fixture(SLATE_HA_CATALOG_ENTITIES, true, ENTITIES,
                                        &complete, &degraded) && !complete &&
                         accept_fixture(SLATE_HA_CATALOG_DEVICES, true, DEVICES,
                                        &complete, &degraded) && !complete &&
                         accept_fixture(SLATE_HA_CATALOG_AREAS, true, AREAS,
                                        &complete, &degraded) && !complete;
    states_failed = states_failed &&
                    slate_ha_catalog_accept(SLATE_HA_CATALOG_STATES, false, NULL,
                                            &complete, &degraded) != ESP_OK && complete;
    cJSON *preserved = cJSON_CreateArray();
    CHECK(states_failed && preserved != NULL &&
              slate_ha_catalog_append(NULL, preserved) == ESP_OK &&
              cJSON_GetArraySize(preserved) == 2 &&
              cJSON_GetObjectItemCaseSensitive(
                  cJSON_GetArrayItem(preserved, 0), "area") == NULL,
          "failed get_states refresh preserves the previous catalog");
    cJSON_Delete(preserved);
    slate_ha_catalog_clear();

#undef CHECK
    ESP_LOGI(TAG, "selftest: %u failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif
