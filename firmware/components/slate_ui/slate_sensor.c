/* Slate — semantic sensor component (DESIGN.md ADR-2/ADR-3, §5.2 and §7.3). */

#include "slate_sensor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "slate_component.h"

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
     * humidity stays a whole percent, and pressure preserves hundredths so an
     * upstream value such as 1013.25 is not silently changed. Power keeps more
     * resolution at small magnitudes without letting a large reading crowd out
     * its unit. Providers only supply the neutral value. */
    switch (measurement) {
    case SLATE_MEASUREMENT_TEMPERATURE:
        return 1;
    case SLATE_MEASUREMENT_HUMIDITY:
        return 0;
    case SLATE_MEASUREMENT_PRESSURE:
        return 2;
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
    if (length <= 5) {
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
                        const slate_theme_t *theme, slate_sensor_view_t *view)
{
    if (tile == NULL || config == NULL || theme == NULL || view == NULL ||
        config->binding_count == 0) {
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
    view->name = make_label(content, name, theme->caption, theme->text_lo);
    if (view->name == NULL) {
        return false;
    }
    lv_label_set_long_mode(view->name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(view->name, lv_pct(100));
    lv_obj_align(view->name, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    view->identity = make_label(tile, "", theme->caption, theme->warn);
    if (view->identity == NULL) {
        return false;
    }
    lv_label_set_long_mode(view->identity, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(view->identity, lv_pct(100));
    lv_obj_set_style_text_align(view->identity, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_flag(view->identity, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(view->identity);
    return true;
}

void slate_sensor_update(const slate_sensor_view_t *view, const slate_resource_t *resource,
                         const slate_theme_t *theme)
{
    const slate_sensor_state_t *sensor = &resource->state.sensor;
    bool placeholder = slate_component_is_placeholder(resource->presentation);
    bool state_present = has_state(resource);
    char value[SLATE_SENSOR_TEXT_MAX + 24];

    char identity[SLATE_COMPONENT_PLACEHOLDER_MAX];
    slate_component_placeholder_text(resource, identity, sizeof(identity));
    lv_label_set_text(view->identity, identity);
    if (placeholder) {
        lv_obj_remove_flag(view->identity, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(view->identity, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *content[] = {view->icon, view->value, view->unit, view->name};
    for (size_t i = 0; i < sizeof(content) / sizeof(content[0]); i++) {
        if (content[i] == NULL) {
            continue;
        }
        if (placeholder) {
            lv_obj_add_flag(content[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(content[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

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
