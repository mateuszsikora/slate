/* Slate — semantic sensor component (DESIGN.md ADR-2/ADR-3, §5.2 and §7.3). */

#include "slate_sensor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void style_plain(lv_obj_t *object)
{
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) {
        return NULL;
    }
    lv_label_set_text(label, text != NULL ? text : "");
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    return label;
}

static const char *measurement_icon(slate_measurement_t measurement)
{
    switch (measurement) {
    case SLATE_MEASUREMENT_TEMPERATURE:
        return SLATE_ICON_THERMOMETER;
    case SLATE_MEASUREMENT_HUMIDITY:
        return SLATE_ICON_WATER_PERCENT;
    case SLATE_MEASUREMENT_PRESSURE:
        return SLATE_ICON_GAUGE;
    case SLATE_MEASUREMENT_POWER:
        return SLATE_ICON_LIGHTNING_BOLT;
    case SLATE_MEASUREMENT_NONE:
        return SLATE_ICON_HELP_CIRCLE_OUTLINE;
    }
    return SLATE_ICON_HELP_CIRCLE_OUTLINE;
}

static unsigned decimal_places(slate_measurement_t measurement, double value)
{
    /* The component owns display precision: tenths matter for temperature,
     * while fractional humidity and pressure are noise at this tile size.
     * Power keeps more resolution at small magnitudes without letting a large
     * reading crowd out its unit. Providers only supply the neutral value. */
    switch (measurement) {
    case SLATE_MEASUREMENT_TEMPERATURE:
        return 1;
    case SLATE_MEASUREMENT_HUMIDITY:
    case SLATE_MEASUREMENT_PRESSURE:
        return 0;
    case SLATE_MEASUREMENT_POWER:
    case SLATE_MEASUREMENT_NONE:
        value = fabs(value);
        return value < 10.0 ? 2 : value < 100.0 ? 1 : 0;
    }
    return 1;
}

static void format_value(const slate_sensor_state_t *sensor, char *out, size_t size)
{
    if (!sensor->numeric) {
        strlcpy(out, sensor->text[0] != '\0' ? sensor->text : "-", size);
        return;
    }

    unsigned decimals = decimal_places(sensor->measurement, sensor->value);
    double value = sensor->value;
    double zero_threshold = decimals == 0 ? 0.5 : decimals == 1 ? 0.05 : 0.005;
    if (fabs(value) < zero_threshold) {
        value = 0.0; /* Do not put a surprising "-0" on the wall. */
    }
    snprintf(out, size, "%.*f", (int) decimals, value);
}

static const lv_font_t *value_font(const slate_theme_t *theme, const char *value)
{
    size_t length = strlen(value);
    if (length <= 7) {
        return theme->hero;
    }
    return length <= 15 ? theme->body : theme->caption;
}

static bool has_state(const slate_resource_t *resource)
{
    return resource->updated_us != 0 &&
           (resource->presentation == SLATE_PRESENT_OK ||
            resource->presentation == SLATE_PRESENT_STALE ||
            resource->presentation == SLATE_PRESENT_UNAVAILABLE);
}

bool slate_sensor_build(lv_obj_t *tile, const slate_config_tile_t *config,
                        const slate_theme_t *theme, slate_sensor_view_t *view,
                        lv_obj_t **name_label)
{
    if (tile == NULL || config == NULL || theme == NULL || view == NULL ||
        name_label == NULL || config->binding_count == 0) {
        return false;
    }

    /* Sensors are read-only (§7.3), including through the direct provider. */
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(tile, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    if (config->width >= 2) {
        if (config->icon != NULL) {
            view->icon_override = slate_icon_find(config->icon);
            if (view->icon_override == NULL) {
                view->icon_override = SLATE_ICON_IMAGE_BROKEN_VARIANT;
            }
        }
        view->icon = make_label(tile,
                                view->icon_override != NULL
                                    ? view->icon_override
                                    : SLATE_ICON_HELP_CIRCLE_OUTLINE,
                                theme->icons, theme->accent);
        if (view->icon == NULL) {
            return false;
        }
        lv_obj_set_width(view->icon, 42);
        lv_obj_set_style_text_align(view->icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }

    lv_obj_t *content = lv_obj_create(tile);
    if (content == NULL) {
        return false;
    }
    style_plain(content);
    lv_obj_set_height(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);

    lv_obj_t *reading = lv_obj_create(content);
    if (reading == NULL) {
        return false;
    }
    style_plain(reading);
    lv_obj_set_size(reading, lv_pct(100), 56);
    lv_obj_align(reading, LV_ALIGN_TOP_LEFT, 0, -4);
    lv_obj_set_layout(reading, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(reading, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reading, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);

    view->value = make_label(reading, "-", theme->hero, theme->text_hi);
    if (view->value == NULL) {
        return false;
    }
    lv_label_set_long_mode(view->value, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(view->value, 1);

    view->unit = make_label(reading, "", theme->caption, theme->text_lo);
    if (view->unit == NULL) {
        return false;
    }

    const slate_config_binding_t *binding = &config->bindings[0];
    const char *name = config->label != NULL ? config->label : binding->resource;
    *name_label = make_label(content, name, theme->caption, theme->text_lo);
    if (*name_label == NULL) {
        return false;
    }
    lv_label_set_long_mode(*name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(*name_label, lv_pct(100));
    lv_obj_align(*name_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return true;
}

void slate_sensor_update(const slate_sensor_view_t *view, const slate_resource_t *resource,
                         const slate_theme_t *theme)
{
    const slate_sensor_state_t *sensor = &resource->state.sensor;
    bool state_present = has_state(resource);
    char value[SLATE_SENSOR_TEXT_MAX + 24];

    if (resource->presentation == SLATE_PRESENT_OK) {
        format_value(sensor, value, sizeof(value));
    } else {
        strlcpy(value, "-", sizeof(value));
    }
    lv_label_set_text(view->value, value);
    lv_obj_set_style_text_font(view->value, value_font(theme, value), LV_PART_MAIN);
    lv_label_set_text(view->unit, state_present ? sensor->unit : "");
    if (view->icon != NULL) {
        lv_label_set_text(
            view->icon,
            view->icon_override != NULL
                ? view->icon_override
                : measurement_icon(state_present ? sensor->measurement
                                                  : SLATE_MEASUREMENT_NONE));
    }
}
