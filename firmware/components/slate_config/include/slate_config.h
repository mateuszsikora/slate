/*
 * Slate — declarative dashboard configuration.
 *
 * DESIGN.md ADR-1/ADR-3, §3 and §4.1. This component owns the JSON-to-model
 * boundary and no transport: HTTP asks it to parse, and the UI runtime in #20
 * will consume the same owned model.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLATE_CONFIG_SCHEMA_MAX 1
#define SLATE_BAR_SLOT_COUNT    12

typedef enum {
    SLATE_COMPONENT_UNKNOWN = 0,
    SLATE_COMPONENT_LIGHT,
    SLATE_COMPONENT_COVER,
    SLATE_COMPONENT_SENSOR,
    SLATE_COMPONENT_SCENE,
} slate_component_type_t;

typedef struct {
    char *provider;
    char *resource;
} slate_config_binding_t;

typedef struct {
    char *id;
    char *type;
    char *label;
    char *icon;
    slate_component_type_t component;
    uint8_t column;
    uint8_t row;
    uint8_t width;
    uint8_t height;
    slate_config_binding_t *bindings;
    size_t binding_count;
} slate_config_tile_t;

typedef struct {
    char *id;
    char *title;
    slate_config_tile_t *tiles;
    size_t tile_count;
} slate_config_page_t;

typedef enum {
    SLATE_BAR_ITEM_UNKNOWN = 0,
    SLATE_BAR_ITEM_CLOCK,
    SLATE_BAR_ITEM_TITLE,
    SLATE_BAR_ITEM_BADGE,
    SLATE_BAR_ITEM_PAGE_INDICATOR,
} slate_bar_item_type_t;

typedef struct {
    char *type;
    char *provider;
    char *label;
    slate_bar_item_type_t item_type;
    uint8_t slot;
    uint8_t span;
} slate_config_bar_item_t;

/* Settings are copied when they have the documented JSON type. Later issues
 * own their product defaults and range validation; presence bits keep an
 * omitted setting distinct from an explicit zero or false without inventing a
 * validation code outside §4.1's schema-1 vocabulary. */
typedef struct {
    char *timezone;
    char *night_start;
    char *night_end;
    int brightness_day;
    int brightness_night;
    int screen_off_after;
    bool wake_on_touch;
    bool has_brightness_day;
    bool has_brightness_night;
    bool has_screen_off_after;
    bool has_wake_on_touch;
} slate_config_settings_t;

typedef struct {
    int schema;
    char *theme;
    char *home_page;
    slate_config_settings_t settings;
    bool has_bar;
    slate_config_bar_item_t *bar_items;
    size_t bar_item_count;
    slate_config_page_t *pages;
    size_t page_count;
} slate_config_t;

typedef enum {
    SLATE_CONFIG_PARSE_OK = 0,
    SLATE_CONFIG_PARSE_EMPTY_BODY,
    SLATE_CONFIG_PARSE_INVALID_JSON,
    SLATE_CONFIG_PARSE_TOO_LARGE,
    SLATE_CONFIG_PARSE_OUT_OF_MEMORY,
    SLATE_CONFIG_PARSE_INVALID_CONFIG,
} slate_config_parse_status_t;

typedef struct {
    const char *code;
    char *path;
    char *tile_id; /* NULL means config_errors. */
} slate_config_error_t;

typedef struct {
    slate_config_parse_status_t status;
    slate_config_error_t *errors;
    size_t error_count;
    size_t error_capacity;
} slate_config_report_t;

/**
 * Parse and validate one complete configuration document.
 *
 * The input need not be NUL-terminated. On success, `*out` owns a model that
 * survives this call and must be released with slate_config_free(). On every
 * failure `*out` is NULL and report says whether this was a request-level
 * failure or an invalid configuration. The temporary cJSON tree is always
 * deleted before this function returns.
 */
slate_config_parse_status_t slate_config_parse(const char *json, size_t len,
                                                slate_config_t **out,
                                                slate_config_report_t *report);

/** Build §4.1's detailed invalid_config response. Caller owns the cJSON tree. */
cJSON *slate_config_report_json(const slate_config_report_t *report);

void slate_config_free(slate_config_t *config);
void slate_config_report_free(slate_config_report_t *report);

#ifdef __cplusplus
}
#endif
