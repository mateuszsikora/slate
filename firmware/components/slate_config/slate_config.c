/* Slate — configuration parser and schema-1 validator. */

#include "slate_config.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "slate_state.h"
#include "slate_store.h"
#include "slate_theme.h"

#define GRID_COLUMNS 4
#define GRID_ROWS    3

typedef struct {
    const char *provider;
    const char *resource;
    slate_component_type_t component;
} seen_binding_t;

typedef struct {
    slate_config_report_t *report;
    seen_binding_t *bindings;
    size_t binding_count;
    size_t binding_capacity;
} validation_t;

typedef enum {
    BINDING_ACCEPTED = 0,
    BINDING_CONFLICT,
    BINDING_CAPACITY_EXCEEDED,
    BINDING_OUT_OF_MEMORY,
} binding_result_t;

static void *config_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0) {
        return NULL;
    }
    return heap_caps_calloc_prefer(count, size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void *config_malloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    return heap_caps_malloc_prefer(size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static char *config_strdup(const char *value)
{
    if (value == NULL) {
        return NULL;
    }
    size_t len = strlen(value) + 1;
    char *copy = config_malloc(len);
    if (copy != NULL) {
        memcpy(copy, value, len);
    }
    return copy;
}

void slate_config_report_free(slate_config_report_t *report)
{
    if (report == NULL) {
        return;
    }
    for (size_t i = 0; i < report->error_count; i++) {
        free(report->errors[i].path);
        free(report->errors[i].tile_id);
    }
    free(report->errors);
    memset(report, 0, sizeof(*report));
}

static void report_out_of_memory(slate_config_report_t *report)
{
    slate_config_report_free(report);
    report->status = SLATE_CONFIG_PARSE_OUT_OF_MEMORY;
}

static bool add_error(slate_config_report_t *report, const char *code,
                      const char *path, const char *tile_id)
{
    if (report->status == SLATE_CONFIG_PARSE_OUT_OF_MEMORY) {
        return false;
    }

    if (report->error_count == report->error_capacity) {
        size_t capacity = report->error_capacity == 0 ? 8 : report->error_capacity * 2;
        slate_config_error_t *errors =
            heap_caps_realloc(report->errors, capacity * sizeof(*errors), MALLOC_CAP_8BIT);
        if (errors == NULL) {
            report_out_of_memory(report);
            return false;
        }
        memset(errors + report->error_capacity, 0,
               (capacity - report->error_capacity) * sizeof(*errors));
        report->errors = errors;
        report->error_capacity = capacity;
    }

    char *path_copy = config_strdup(path);
    char *tile_copy = tile_id != NULL ? config_strdup(tile_id) : NULL;
    if (path_copy == NULL || (tile_id != NULL && tile_copy == NULL)) {
        free(path_copy);
        free(tile_copy);
        report_out_of_memory(report);
        return false;
    }

    report->errors[report->error_count++] = (slate_config_error_t) {
        .code = code,
        .path = path_copy,
        .tile_id = tile_copy,
    };
    return true;
}

static bool json_integer(const cJSON *item, int *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble < INT_MIN || item->valuedouble > INT_MAX) {
        return false;
    }
    *out = (int) item->valuedouble;
    return true;
}

static slate_component_type_t component_type(const cJSON *tile)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(tile, "type");
    if (!cJSON_IsString(type)) {
        return SLATE_COMPONENT_UNKNOWN;
    }
    if (strcmp(type->valuestring, "light") == 0) {
        return SLATE_COMPONENT_LIGHT;
    }
    if (strcmp(type->valuestring, "cover") == 0) {
        return SLATE_COMPONENT_COVER;
    }
    if (strcmp(type->valuestring, "sensor") == 0) {
        return SLATE_COMPONENT_SENSOR;
    }
    if (strcmp(type->valuestring, "scene") == 0) {
        return SLATE_COMPONENT_SCENE;
    }
    return SLATE_COMPONENT_UNKNOWN;
}

static bool grid_pair(const cJSON *tile, const char *name, int *first, int *second)
{
    const cJSON *pair = cJSON_GetObjectItemCaseSensitive(tile, name);
    return cJSON_IsArray(pair) && cJSON_GetArraySize(pair) == 2 &&
           json_integer(cJSON_GetArrayItem(pair, 0), first) &&
           json_integer(cJSON_GetArrayItem(pair, 1), second);
}

static bool permitted_size(int width, int height)
{
    return (width == 1 && height == 1) || (width == 2 && height == 1) ||
           (width == 1 && height == 2) || (width == 2 && height == 2) ||
           (width == 4 && height == 1);
}

static const char *tile_id(const cJSON *tile)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(tile, "id");
    return cJSON_IsString(id) && id->valuestring[0] != '\0' ? id->valuestring : NULL;
}

static bool tile_id_is_unique(const cJSON *pages, const char *wanted)
{
    if (wanted == NULL) {
        return false;
    }
    size_t matches = 0;
    const cJSON *page = NULL;
    cJSON_ArrayForEach(page, pages) {
        const cJSON *tiles = cJSON_GetObjectItemCaseSensitive(page, "tiles");
        const cJSON *tile = NULL;
        cJSON_ArrayForEach(tile, tiles) {
            const char *id = tile_id(tile);
            if (id != NULL && strcmp(id, wanted) == 0) {
                matches++;
            }
        }
    }
    return matches == 1;
}

static bool id_seen_before(const cJSON *pages, int page_index, int tile_index,
                           const char *wanted)
{
    for (int p = 0; p <= page_index; p++) {
        const cJSON *page = cJSON_GetArrayItem(pages, p);
        const cJSON *tiles = cJSON_GetObjectItemCaseSensitive(page, "tiles");
        int limit = p == page_index ? tile_index : cJSON_GetArraySize(tiles);
        for (int t = 0; t < limit; t++) {
            const char *id = tile_id(cJSON_GetArrayItem(tiles, t));
            if (id != NULL && strcmp(id, wanted) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool page_id_seen_before(const cJSON *pages, int page_index, const char *wanted)
{
    for (int i = 0; i < page_index; i++) {
        const cJSON *page = cJSON_GetArrayItem(pages, i);
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(page, "id");
        if (cJSON_IsString(id) && strcmp(id->valuestring, wanted) == 0) {
            return true;
        }
    }
    return false;
}

static binding_result_t remember_binding(validation_t *validation,
                                         const char *provider,
                                         const char *resource,
                                         slate_component_type_t component)
{
    for (size_t i = 0; i < validation->binding_count; i++) {
        const seen_binding_t *seen = &validation->bindings[i];
        if (strcmp(seen->provider, provider) == 0 &&
            strcmp(seen->resource, resource) == 0) {
            return seen->component == component ? BINDING_ACCEPTED
                                                : BINDING_CONFLICT;
        }
    }

    /* The validated model must remain activatable by slate_state. Reject the
     * first resource that cannot fit instead of accepting a document which a
     * later apply operation is guaranteed to refuse. */
    if (validation->binding_count >= SLATE_STATE_MAX_RESOURCES) {
        return BINDING_CAPACITY_EXCEEDED;
    }

    if (validation->binding_count == validation->binding_capacity) {
        size_t capacity = validation->binding_capacity == 0
                              ? 8
                              : validation->binding_capacity * 2;
        seen_binding_t *bindings = heap_caps_realloc(
            validation->bindings, capacity * sizeof(*bindings), MALLOC_CAP_8BIT);
        if (bindings == NULL) {
            report_out_of_memory(validation->report);
            return BINDING_OUT_OF_MEMORY;
        }
        validation->bindings = bindings;
        validation->binding_capacity = capacity;
    }

    validation->bindings[validation->binding_count++] = (seen_binding_t) {
        .provider = provider,
        .resource = resource,
        .component = component,
    };
    return BINDING_ACCEPTED;
}

static void validate_binding(validation_t *validation, const cJSON *binding,
                             slate_component_type_t component, const char *path,
                             const char *unambiguous_tile_id)
{
    if (!cJSON_IsObject(binding)) {
        add_error(validation->report, "binding_required", path, unambiguous_tile_id);
        return;
    }

    const cJSON *provider = cJSON_GetObjectItemCaseSensitive(binding, "provider");
    const cJSON *resource = cJSON_GetObjectItemCaseSensitive(binding, "resource");
    bool provider_ok = cJSON_IsString(provider) && provider->valuestring[0] != '\0' &&
                       strlen(provider->valuestring) <= SLATE_PROVIDER_ID_MAX;
    bool resource_ok = cJSON_IsString(resource) && resource->valuestring[0] != '\0' &&
                       strlen(resource->valuestring) <= SLATE_RESOURCE_ID_MAX;

    char field_path[256];
    if (!provider_ok) {
        snprintf(field_path, sizeof(field_path), "%s/provider", path);
        add_error(validation->report, "provider_required", field_path,
                  unambiguous_tile_id);
    }
    if (!resource_ok) {
        snprintf(field_path, sizeof(field_path), "%s/resource", path);
        add_error(validation->report, "resource_required", field_path,
                  unambiguous_tile_id);
    }

    if (provider_ok && resource_ok) {
        binding_result_t result = remember_binding(
            validation, provider->valuestring, resource->valuestring, component);
        if (result == BINDING_CONFLICT || result == BINDING_CAPACITY_EXCEEDED) {
            /* Schema 1 has one public code for an unusable binding. This covers
             * both a cross-component contradiction and the 257th distinct
             * state resource, while preserving the exact offending path. */
            add_error(validation->report, "binding_required", path,
                      unambiguous_tile_id);
        }
    }
}

static bool rectangles_overlap(const cJSON *left, const cJSON *right)
{
    int lx, ly, lw, lh;
    int rx, ry, rw, rh;
    if (!grid_pair(left, "pos", &lx, &ly) || !grid_pair(left, "size", &lw, &lh) ||
        !permitted_size(lw, lh) || !grid_pair(right, "pos", &rx, &ry) ||
        !grid_pair(right, "size", &rw, &rh) || !permitted_size(rw, rh)) {
        return false;
    }
    if (lx < 0 || ly < 0 || lx > GRID_COLUMNS - lw || ly > GRID_ROWS - lh ||
        rx < 0 || ry < 0 || rx > GRID_COLUMNS - rw || ry > GRID_ROWS - rh) {
        return false;
    }
    return lx < rx + rw && rx < lx + lw && ly < ry + rh && ry < ly + lh;
}

static void validate_tile(validation_t *validation, const cJSON *pages,
                          const cJSON *tiles, int page_index, int tile_index)
{
    const cJSON *tile = cJSON_GetArrayItem(tiles, tile_index);
    char path[128];
    snprintf(path, sizeof(path), "/pages/%d/tiles/%d", page_index, tile_index);

    const char *id = tile_id(tile);
    bool unique_id = tile_id_is_unique(pages, id);
    const char *bucket = unique_id ? id : NULL;
    if (id == NULL) {
        char id_path[160];
        snprintf(id_path, sizeof(id_path), "%s/id", path);
        add_error(validation->report, "tile_id_required", id_path, NULL);
    } else if (id_seen_before(pages, page_index, tile_index, id)) {
        char id_path[160];
        snprintf(id_path, sizeof(id_path), "%s/id", path);
        add_error(validation->report, "duplicate_tile_id", id_path, NULL);
    }

    int column, row, width, height;
    bool position_ok = grid_pair(tile, "pos", &column, &row);
    bool size_ok = grid_pair(tile, "size", &width, &height) &&
                   permitted_size(width, height);
    if (!position_ok) {
        char pos_path[160];
        snprintf(pos_path, sizeof(pos_path), "%s/pos", path);
        add_error(validation->report, "invalid_position", pos_path, bucket);
    }
    if (!size_ok) {
        char size_path[160];
        snprintf(size_path, sizeof(size_path), "%s/size", path);
        add_error(validation->report, "invalid_size", size_path, bucket);
    }
    if (position_ok && size_ok &&
        (column < 0 || row < 0 || column > GRID_COLUMNS - width ||
         row > GRID_ROWS - height)) {
        char pos_path[160];
        snprintf(pos_path, sizeof(pos_path), "%s/pos", path);
        add_error(validation->report, "tile_out_of_bounds", pos_path, bucket);
    }

    if (position_ok && size_ok) {
        for (int previous = 0; previous < tile_index; previous++) {
            if (rectangles_overlap(tile, cJSON_GetArrayItem(tiles, previous))) {
                char pos_path[160];
                snprintf(pos_path, sizeof(pos_path), "%s/pos", path);
                add_error(validation->report, "tile_overlap", pos_path, bucket);
                break;
            }
        }
    }

    slate_component_type_t component = component_type(tile);
    if (component == SLATE_COMPONENT_UNKNOWN) {
        return; /* §3.1: a future component is a placeholder, not an error. */
    }

    if (component == SLATE_COMPONENT_SCENE) {
        const cJSON *bindings = cJSON_GetObjectItemCaseSensitive(tile, "bindings");
        char bindings_path[160];
        snprintf(bindings_path, sizeof(bindings_path), "%s/bindings", path);
        if (!cJSON_IsArray(bindings) || cJSON_GetArraySize(bindings) == 0) {
            add_error(validation->report, "binding_required", bindings_path, bucket);
            return;
        }
        int binding_count = cJSON_GetArraySize(bindings);
        bool compact = size_ok && width == 1 && height == 1;
        bool bar = size_ok && width == 4 && height == 1;
        if (size_ok && !compact && !bar) {
            char size_path[160];
            snprintf(size_path, sizeof(size_path), "%s/size", path);
            add_error(validation->report, "invalid_size", size_path, bucket);
        }
        if ((compact && binding_count != 1) ||
            (bar && (binding_count < 2 || binding_count > 5))) {
            add_error(validation->report, "binding_required", bindings_path, bucket);
        }
        for (int i = 0; i < cJSON_GetArraySize(bindings); i++) {
            char binding_path[192];
            snprintf(binding_path, sizeof(binding_path), "%s/%d", bindings_path, i);
            validate_binding(validation, cJSON_GetArrayItem(bindings, i), component,
                             binding_path, bucket);
        }
        return;
    }

    char binding_path[160];
    snprintf(binding_path, sizeof(binding_path), "%s/binding", path);
    validate_binding(validation, cJSON_GetObjectItemCaseSensitive(tile, "binding"),
                     component, binding_path, bucket);
}

static void validate_document(const cJSON *root, validation_t *validation)
{
    slate_config_report_t *report = validation->report;
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    int schema_number = 0;
    if (schema == NULL) {
        add_error(report, "schema_required", "/schema", NULL);
    } else if (!json_integer(schema, &schema_number) || schema_number < 1) {
        add_error(report, "schema_invalid", "/schema", NULL);
    } else if (schema_number > SLATE_CONFIG_SCHEMA_MAX) {
        add_error(report, "schema_too_new", "/schema", NULL);
        /* Do not diagnose a document whose vocabulary this firmware does not
         * know. A schema-2 theme or page shape is not malformed schema 1; the
         * one actionable answer is §3.4's firmware-update requirement. */
        return;
    }

    const cJSON *theme = cJSON_GetObjectItemCaseSensitive(root, "theme");
    if (!cJSON_IsString(theme) || theme->valuestring[0] == '\0') {
        add_error(report, "theme_required", "/theme", NULL);
    } else if (slate_theme_find(theme->valuestring) == NULL) {
        add_error(report, "theme_not_found", "/theme", NULL);
    }

    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    if (!cJSON_IsArray(pages)) {
        add_error(report, "pages_required", "/pages", NULL);
        add_error(report, "home_page_not_found", "/home_page", NULL);
        return;
    }

    const cJSON *home_page = cJSON_GetObjectItemCaseSensitive(root, "home_page");
    bool home_found = false;
    for (int p = 0; p < cJSON_GetArraySize(pages); p++) {
        const cJSON *page = cJSON_GetArrayItem(pages, p);
        char page_path[96];
        snprintf(page_path, sizeof(page_path), "/pages/%d", p);
        if (!cJSON_IsObject(page)) {
            add_error(report, "pages_required", page_path, NULL);
            continue;
        }

        const cJSON *page_id = cJSON_GetObjectItemCaseSensitive(page, "id");
        if (!cJSON_IsString(page_id) || page_id->valuestring[0] == '\0') {
            char id_path[128];
            snprintf(id_path, sizeof(id_path), "%s/id", page_path);
            add_error(report, "pages_required", id_path, NULL);
        } else {
            if (page_id_seen_before(pages, p, page_id->valuestring)) {
                char id_path[128];
                snprintf(id_path, sizeof(id_path), "%s/id", page_path);
                add_error(report, "duplicate_page_id", id_path, NULL);
            }
            if (cJSON_IsString(home_page) &&
                strcmp(home_page->valuestring, page_id->valuestring) == 0) {
                home_found = true;
            }
        }

        const cJSON *tiles = cJSON_GetObjectItemCaseSensitive(page, "tiles");
        if (tiles == NULL) {
            continue; /* An empty page may omit its empty tile collection. */
        }
        if (!cJSON_IsArray(tiles)) {
            char tiles_path[128];
            snprintf(tiles_path, sizeof(tiles_path), "%s/tiles", page_path);
            add_error(report, "pages_required", tiles_path, NULL);
            continue;
        }
        for (int t = 0; t < cJSON_GetArraySize(tiles); t++) {
            if (!cJSON_IsObject(cJSON_GetArrayItem(tiles, t))) {
                char tile_path[128];
                snprintf(tile_path, sizeof(tile_path), "%s/tiles/%d", page_path, t);
                add_error(report, "tile_id_required", tile_path, NULL);
                continue;
            }
            validate_tile(validation, pages, tiles, p, t);
        }
    }

    if (!home_found) {
        add_error(report, "home_page_not_found", "/home_page", NULL);
    }
}

static void binding_free(slate_config_binding_t *binding)
{
    free(binding->provider);
    free(binding->resource);
}

void slate_config_free(slate_config_t *config)
{
    if (config == NULL) {
        return;
    }
    free(config->theme);
    free(config->home_page);
    free(config->settings.timezone);
    free(config->settings.night_start);
    free(config->settings.night_end);
    for (size_t p = 0; p < config->page_count; p++) {
        slate_config_page_t *page = &config->pages[p];
        free(page->id);
        free(page->title);
        for (size_t t = 0; t < page->tile_count; t++) {
            slate_config_tile_t *tile = &page->tiles[t];
            free(tile->id);
            free(tile->type);
            free(tile->label);
            free(tile->icon);
            for (size_t b = 0; b < tile->binding_count; b++) {
                binding_free(&tile->bindings[b]);
            }
            free(tile->bindings);
        }
        free(page->tiles);
    }
    free(config->pages);
    free(config);
}

static bool copy_optional_string(const cJSON *object, const char *name, char **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item)) {
        return true;
    }
    *out = config_strdup(item->valuestring);
    return *out != NULL;
}

static bool build_binding(const cJSON *json, slate_config_binding_t *binding)
{
    const cJSON *provider = cJSON_GetObjectItemCaseSensitive(json, "provider");
    const cJSON *resource = cJSON_GetObjectItemCaseSensitive(json, "resource");
    binding->provider = config_strdup(provider->valuestring);
    binding->resource = config_strdup(resource->valuestring);
    return binding->provider != NULL && binding->resource != NULL;
}

static bool build_settings(const cJSON *root, slate_config_settings_t *settings)
{
    const cJSON *json = cJSON_GetObjectItemCaseSensitive(root, "settings");
    if (!cJSON_IsObject(json)) {
        return true;
    }
    if (!copy_optional_string(json, "timezone", &settings->timezone) ||
        !copy_optional_string(json, "night_start", &settings->night_start) ||
        !copy_optional_string(json, "night_end", &settings->night_end)) {
        return false;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, "brightness_day");
    settings->has_brightness_day = json_integer(item, &settings->brightness_day);
    item = cJSON_GetObjectItemCaseSensitive(json, "brightness_night");
    settings->has_brightness_night = json_integer(item, &settings->brightness_night);
    item = cJSON_GetObjectItemCaseSensitive(json, "screen_off_after");
    settings->has_screen_off_after = json_integer(item, &settings->screen_off_after);
    item = cJSON_GetObjectItemCaseSensitive(json, "wake_on_touch");
    if (cJSON_IsBool(item)) {
        settings->has_wake_on_touch = true;
        settings->wake_on_touch = cJSON_IsTrue(item);
    }
    return true;
}

static bool build_tile(const cJSON *json, slate_config_tile_t *tile)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    tile->id = config_strdup(id->valuestring);
    tile->type = config_strdup(cJSON_IsString(type) ? type->valuestring : "unknown");
    tile->component = component_type(json);
    if (tile->id == NULL || tile->type == NULL ||
        !copy_optional_string(json, "label", &tile->label) ||
        !copy_optional_string(json, "icon", &tile->icon)) {
        return false;
    }

    int value[4] = {0};
    grid_pair(json, "pos", &value[0], &value[1]);
    grid_pair(json, "size", &value[2], &value[3]);
    tile->column = (uint8_t) value[0];
    tile->row = (uint8_t) value[1];
    tile->width = (uint8_t) value[2];
    tile->height = (uint8_t) value[3];

    if (tile->component == SLATE_COMPONENT_UNKNOWN) {
        return true;
    }

    const cJSON *bindings;
    if (tile->component == SLATE_COMPONENT_SCENE) {
        bindings = cJSON_GetObjectItemCaseSensitive(json, "bindings");
        tile->binding_count = (size_t) cJSON_GetArraySize(bindings);
    } else {
        bindings = NULL;
        tile->binding_count = 1;
    }
    tile->bindings = config_calloc(tile->binding_count, sizeof(*tile->bindings));
    if (tile->bindings == NULL) {
        return false;
    }
    for (size_t i = 0; i < tile->binding_count; i++) {
        const cJSON *binding = tile->component == SLATE_COMPONENT_SCENE
                                   ? cJSON_GetArrayItem(bindings, (int) i)
                                   : cJSON_GetObjectItemCaseSensitive(json, "binding");
        if (!build_binding(binding, &tile->bindings[i])) {
            return false;
        }
    }
    return true;
}

static slate_config_t *build_config(const cJSON *root)
{
    slate_config_t *config = config_calloc(1, sizeof(*config));
    if (config == NULL) {
        return NULL;
    }

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *theme = cJSON_GetObjectItemCaseSensitive(root, "theme");
    const cJSON *home_page = cJSON_GetObjectItemCaseSensitive(root, "home_page");
    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    config->schema = (int) schema->valuedouble;
    config->theme = config_strdup(theme->valuestring);
    config->home_page = config_strdup(home_page->valuestring);
    config->page_count = (size_t) cJSON_GetArraySize(pages);
    if (config->theme == NULL || config->home_page == NULL || !build_settings(root, &config->settings)) {
        slate_config_free(config);
        return NULL;
    }

    if (config->page_count > 0) {
        config->pages = config_calloc(config->page_count, sizeof(*config->pages));
        if (config->pages == NULL) {
            slate_config_free(config);
            return NULL;
        }
    }
    for (size_t p = 0; p < config->page_count; p++) {
        const cJSON *page_json = cJSON_GetArrayItem(pages, (int) p);
        slate_config_page_t *page = &config->pages[p];
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(page_json, "id");
        const cJSON *tiles = cJSON_GetObjectItemCaseSensitive(page_json, "tiles");
        page->id = config_strdup(id->valuestring);
        if (page->id == NULL || !copy_optional_string(page_json, "title", &page->title)) {
            slate_config_free(config);
            return NULL;
        }
        page->tile_count = cJSON_IsArray(tiles) ? (size_t) cJSON_GetArraySize(tiles) : 0;
        if (page->tile_count > 0) {
            page->tiles = config_calloc(page->tile_count, sizeof(*page->tiles));
            if (page->tiles == NULL) {
                slate_config_free(config);
                return NULL;
            }
        }
        for (size_t t = 0; t < page->tile_count; t++) {
            if (!build_tile(cJSON_GetArrayItem(tiles, (int) t), &page->tiles[t])) {
                slate_config_free(config);
                return NULL;
            }
        }
    }
    return config;
}

static bool input_is_utf8(const char *input, size_t len)
{
    const unsigned char *bytes = (const unsigned char *) input;
    size_t offset = 0;
    while (offset < len) {
        uint32_t codepoint;
        size_t continuation_count;
        uint32_t minimum;
        unsigned char first = bytes[offset++];
        if (first <= 0x7f) {
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1f;
            continuation_count = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0f;
            continuation_count = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 0x07;
            continuation_count = 3;
            minimum = 0x10000;
        } else {
            return false;
        }

        if (continuation_count > len - offset) {
            return false;
        }
        for (size_t i = 0; i < continuation_count; i++) {
            unsigned char continuation = bytes[offset++];
            if ((continuation & 0xc0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (continuation & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

static int hex_digit(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

static bool json_has_escaped_nul(const char *json, size_t len)
{
    bool in_string = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char byte = (unsigned char) json[i];
        if (!in_string) {
            in_string = byte == '"';
            continue;
        }
        if (byte == '"') {
            in_string = false;
            continue;
        }
        if (byte != '\\' || i + 1 >= len) {
            continue;
        }

        unsigned char escape = (unsigned char) json[++i];
        if (escape != 'u' || i + 4 >= len) {
            continue;
        }
        int value = 0;
        bool valid_escape = true;
        for (size_t digit = 1; digit <= 4; digit++) {
            int nibble = hex_digit((unsigned char) json[i + digit]);
            if (nibble < 0) {
                valid_escape = false;
                break;
            }
            value = (value << 4) | nibble;
        }
        if (valid_escape && value == 0) {
            return true;
        }
        if (valid_escape) {
            i += 4;
        }
    }
    return false;
}

slate_config_parse_status_t slate_config_parse(const char *json, size_t len,
                                                slate_config_t **out,
                                                slate_config_report_t *report)
{
    if (out == NULL || report == NULL) {
        return SLATE_CONFIG_PARSE_INVALID_JSON;
    }
    *out = NULL;
    memset(report, 0, sizeof(*report));

    if (json == NULL || len == 0) {
        report->status = SLATE_CONFIG_PARSE_EMPTY_BODY;
        return report->status;
    }
    if (len > SLATE_CONFIG_MAX_BYTES) {
        report->status = SLATE_CONFIG_PARSE_TOO_LARGE;
        return report->status;
    }
    if (memchr(json, '\0', len) != NULL || !input_is_utf8(json, len) ||
        json_has_escaped_nul(json, len)) {
        report->status = SLATE_CONFIG_PARSE_INVALID_JSON;
        return report->status;
    }

    char *copy = config_malloc(len + 1);
    if (copy == NULL) {
        report->status = SLATE_CONFIG_PARSE_OUT_OF_MEMORY;
        return report->status;
    }
    memcpy(copy, json, len);
    copy[len] = '\0';

    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(copy, len + 1, &end, true);
    free(copy);
    if (root == NULL || end == NULL) {
        cJSON_Delete(root);
        report->status = SLATE_CONFIG_PARSE_INVALID_JSON;
        return report->status;
    }

    validation_t validation = {.report = report};
    if (!cJSON_IsObject(root)) {
        add_error(report, "schema_required", "/schema", NULL);
        add_error(report, "theme_required", "/theme", NULL);
        add_error(report, "pages_required", "/pages", NULL);
        add_error(report, "home_page_not_found", "/home_page", NULL);
    } else {
        validate_document(root, &validation);
    }
    free(validation.bindings);

    if (report->status == SLATE_CONFIG_PARSE_OUT_OF_MEMORY) {
        cJSON_Delete(root);
        return report->status;
    }
    if (report->error_count > 0) {
        cJSON_Delete(root);
        report->status = SLATE_CONFIG_PARSE_INVALID_CONFIG;
        return report->status;
    }

    *out = build_config(root);
    cJSON_Delete(root);
    if (*out == NULL) {
        report_out_of_memory(report);
        return report->status;
    }
    report->status = SLATE_CONFIG_PARSE_OK;
    return report->status;
}

static cJSON *error_json(const slate_config_error_t *error)
{
    cJSON *object = cJSON_CreateObject();
    bool ok = object != NULL &&
              cJSON_AddStringToObject(object, "code", error->code) != NULL &&
              cJSON_AddStringToObject(object, "path", error->path) != NULL;
    if (!ok) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

cJSON *slate_config_report_json(const slate_config_report_t *report)
{
    if (report == NULL || report->status != SLATE_CONFIG_PARSE_INVALID_CONFIG) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *config_errors = root != NULL ? cJSON_CreateArray() : NULL;
    cJSON *tile_errors = config_errors != NULL ? cJSON_CreateObject() : NULL;
    bool ok = root != NULL && config_errors != NULL && tile_errors != NULL &&
              cJSON_AddStringToObject(root, "error", "invalid_config") != NULL &&
              cJSON_AddItemToObject(root, "config_errors", config_errors);
    if (ok) {
        config_errors = NULL;
        ok = cJSON_AddItemToObject(root, "tile_errors", tile_errors);
        if (ok) {
            tile_errors = NULL;
        }
    }

    for (size_t i = 0; ok && i < report->error_count; i++) {
        const slate_config_error_t *error = &report->errors[i];
        cJSON *target = NULL;
        if (error->tile_id == NULL) {
            target = cJSON_GetObjectItemCaseSensitive(root, "config_errors");
        } else {
            cJSON *tiles = cJSON_GetObjectItemCaseSensitive(root, "tile_errors");
            target = cJSON_GetObjectItemCaseSensitive(tiles, error->tile_id);
            if (target == NULL) {
                target = cJSON_CreateArray();
                if (target == NULL ||
                    !cJSON_AddItemToObject(tiles, error->tile_id, target)) {
                    cJSON_Delete(target);
                    target = NULL;
                }
            }
        }

        cJSON *entry = target != NULL ? error_json(error) : NULL;
        ok = entry != NULL && cJSON_AddItemToArray(target, entry);
        if (!ok) {
            cJSON_Delete(entry);
        }
    }

    if (!ok) {
        cJSON_Delete(root);
        cJSON_Delete(config_errors);
        cJSON_Delete(tile_errors);
        return NULL;
    }
    return root;
}
