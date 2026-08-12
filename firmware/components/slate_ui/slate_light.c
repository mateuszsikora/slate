/* Slate — semantic light component (DESIGN.md ADR-2/ADR-3, §5.2–§5.3 and §7.1). */

#include "slate_light.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "slate_component.h"

static const char *TAG = "slate_light";

#define LIGHT_SLIDER_HEIGHT       12
#define LIGHT_SLIDER_TOUCH_EXPAND 18
#define LIGHT_TEMPERATURE_SPAN_K  1500
#define LIGHT_TEMPERATURE_MIN_K   2000
#define LIGHT_TEMPERATURE_MAX_K   6500
#define LIGHT_PENDING_OPA_LOW     LV_OPA_40
#define LIGHT_PENDING_PULSE_MS    650

_Static_assert(LIGHT_SLIDER_HEIGHT + 2 * LIGHT_SLIDER_TOUCH_EXPAND >= 48,
               "light sliders need a 48 px touch target");

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

static void set_visible(lv_obj_t *object, bool visible)
{
    if (object == NULL) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_enabled(lv_obj_t *object, bool enabled)
{
    if (object == NULL) {
        return;
    }
    if (enabled) {
        lv_obj_remove_state(object, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(object, LV_STATE_DISABLED);
    }
}

static void pending_border_opa(void *object, int32_t opacity)
{
    lv_obj_set_style_border_opa(object, (lv_opa_t) opacity, LV_PART_MAIN);
}

static void set_pending_animation(slate_light_view_t *view, bool pending)
{
    if (pending == view->pending_animation) {
        return;
    }

    if (!pending) {
        lv_anim_delete(view->tile, pending_border_opa);
        lv_obj_set_style_border_opa(view->tile, LV_OPA_COVER, LV_PART_MAIN);
        view->pending_animation = false;
        return;
    }

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, view->tile);
    lv_anim_set_exec_cb(&animation, pending_border_opa);
    lv_anim_set_values(&animation, LIGHT_PENDING_OPA_LOW, LV_OPA_COVER);
    lv_anim_set_duration(&animation, LIGHT_PENDING_PULSE_MS);
    lv_anim_set_reverse_duration(&animation, LIGHT_PENDING_PULSE_MS);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    view->pending_animation = lv_anim_start(&animation) != NULL;
}

static void dispatch_action(slate_light_view_t *view, slate_action_t action,
                            slate_action_value_type_t value_type, int32_t value)
{
    if (!slate_component_actions_enabled()) {
        return;
    }
    const slate_action_request_t request = {
        .provider = view->provider,
        .resource = view->resource,
        .action = action,
        .value_type = value_type,
        .value.number = value,
    };
    esp_err_t err = slate_action_dispatch(&request, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s:%s %s refused: %s", view->provider, view->resource,
                 slate_action_str(action), esp_err_to_name(err));
    }
}

static void toggle_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    slate_light_view_t *view = lv_event_get_user_data(event);
    dispatch_action(view, SLATE_ACTION_TOGGLE, SLATE_ACTION_VALUE_NONE, 0);
}

static void brightness_event(lv_event_t *event)
{
    slate_light_view_t *view = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (view->updating) {
        return;
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        char value[8];
        snprintf(value, sizeof(value), "%" PRId32 "%%",
                 lv_slider_get_value(view->brightness_slider));
        lv_label_set_text(view->brightness_value, value);
    } else if (code == LV_EVENT_RELEASED) {
        dispatch_action(view, SLATE_ACTION_SET_BRIGHTNESS, SLATE_ACTION_VALUE_NUMBER,
                        lv_slider_get_value(view->brightness_slider));
    }
}

static void temperature_event(lv_event_t *event)
{
    slate_light_view_t *view = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (view->updating) {
        return;
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        char value[16];
        snprintf(value, sizeof(value), "%" PRId32 " K",
                 lv_slider_get_value(view->temperature_slider));
        lv_label_set_text(view->temperature_value, value);
    } else if (code == LV_EVENT_RELEASED) {
        dispatch_action(view, SLATE_ACTION_SET_COLOR_TEMPERATURE,
                        SLATE_ACTION_VALUE_NUMBER,
                        lv_slider_get_value(view->temperature_slider));
    }
}

static lv_obj_t *make_slider(lv_obj_t *parent, const slate_theme_t *theme,
                             lv_event_cb_t callback, slate_light_view_t *view)
{
    lv_obj_t *slider = lv_slider_create(parent);
    if (slider == NULL) {
        return NULL;
    }
    lv_obj_set_height(slider, LIGHT_SLIDER_HEIGHT);
    lv_obj_set_ext_click_area(slider, LIGHT_SLIDER_TOUCH_EXPAND);
    lv_obj_set_style_bg_color(slider, lv_color_hex(theme->surface_alt), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(theme->accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(theme->text_hi), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED, view);
    lv_obj_add_event_cb(slider, callback, LV_EVENT_RELEASED, view);
    return slider;
}

static bool build_compact(lv_obj_t *tile, const slate_theme_t *theme,
                          slate_light_view_t *view, const char *name)
{
    view->icon = make_label(tile, SLATE_ICON_LIGHTBULB_OUTLINE, theme->icons, theme->text_lo);
    view->name = make_label(tile, name, theme->body, theme->text_hi);
    view->state_dot = lv_obj_create(tile);
    if (view->icon == NULL || view->name == NULL || view->state_dot == NULL) {
        return false;
    }

    lv_obj_set_pos(view->icon, 0, 0);
    lv_obj_set_size(view->icon, 36, 36);
    lv_obj_set_style_text_align(view->icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(view->name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(view->name, lv_pct(100));
    lv_obj_align(view->name, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_remove_style_all(view->state_dot);
    lv_obj_remove_flag(view->state_dot,
                       LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                           LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_size(view->state_dot, 12, 12);
    lv_obj_set_style_radius(view->state_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(view->state_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(view->state_dot, LV_ALIGN_TOP_RIGHT, -2, 8);
    lv_obj_add_event_cb(tile, toggle_event, LV_EVENT_CLICKED, view);
    return true;
}

static bool build_wide(lv_obj_t *tile, const slate_theme_t *theme,
                       slate_light_view_t *view, const char *name)
{
    view->icon = make_label(tile, SLATE_ICON_LIGHTBULB_OUTLINE, theme->icons, theme->text_lo);
    view->name = make_label(tile, name, theme->body, theme->text_hi);
    view->brightness_value = make_label(tile, "-", theme->body, theme->text_hi);
    view->brightness_slider = make_slider(tile, theme, brightness_event, view);
    if (view->icon == NULL || view->name == NULL || view->brightness_value == NULL ||
        view->brightness_slider == NULL) {
        return false;
    }

    lv_obj_set_pos(view->icon, 0, 28);
    lv_obj_set_size(view->icon, 36, 36);
    lv_obj_set_style_text_align(view->icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(view->name, 52, 0);
    /* The shell's coordinates are resolved lazily by LVGL. Read its configured
     * width so wide controls also size correctly before the first layout pass. */
    int32_t content_width = lv_obj_calc_dynamic_width(tile, LV_STYLE_WIDTH) -
                            lv_obj_get_style_space_left(tile, LV_PART_MAIN) -
                            lv_obj_get_style_space_right(tile, LV_PART_MAIN);
    int32_t controls_width = content_width - 64;
    lv_obj_set_width(view->name, controls_width - 64);
    lv_label_set_long_mode(view->name, LV_LABEL_LONG_DOT);
    lv_obj_align(view->brightness_value, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_pos(view->brightness_slider, 52, 54);
    lv_obj_set_width(view->brightness_slider, controls_width);
    return true;
}

static bool build_large(lv_obj_t *tile, const slate_theme_t *theme,
                        slate_light_view_t *view, const char *name)
{
    view->icon = make_label(tile, SLATE_ICON_LIGHTBULB_OUTLINE, theme->icons_large,
                            theme->text_lo);
    view->name = make_label(tile, name, theme->body, theme->text_hi);
    view->state_dot = lv_obj_create(tile);
    view->brightness_value = make_label(tile, "-", theme->caption, theme->text_hi);
    view->brightness_slider = make_slider(tile, theme, brightness_event, view);
    view->temperature_label = make_label(tile, "COLOUR TEMPERATURE", theme->caption,
                                         theme->text_lo);
    view->temperature_value = make_label(tile, "-", theme->caption, theme->text_hi);
    view->temperature_slider = make_slider(tile, theme, temperature_event, view);
    if (view->icon == NULL || view->name == NULL || view->state_dot == NULL ||
        view->brightness_value == NULL || view->brightness_slider == NULL ||
        view->temperature_label == NULL || view->temperature_value == NULL ||
        view->temperature_slider == NULL) {
        return false;
    }

    lv_obj_set_pos(view->icon, 0, 0);
    lv_obj_set_size(view->icon, 54, 54);
    lv_obj_set_style_text_align(view->icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(view->name, 66, 8);
    lv_obj_set_width(view->name, 220);
    lv_label_set_long_mode(view->name, LV_LABEL_LONG_DOT);

    lv_obj_remove_style_all(view->state_dot);
    lv_obj_remove_flag(view->state_dot,
                       LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                           LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_size(view->state_dot, 12, 12);
    lv_obj_set_style_radius(view->state_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(view->state_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(view->state_dot, LV_ALIGN_TOP_RIGHT, -2, 18);

    view->brightness_label = make_label(tile, "BRIGHTNESS", theme->caption,
                                        theme->text_lo);
    if (view->brightness_label == NULL) {
        return false;
    }
    lv_obj_set_pos(view->brightness_label, 0, 72);
    lv_obj_align(view->brightness_value, LV_ALIGN_TOP_RIGHT, 0, 72);
    lv_obj_set_pos(view->brightness_slider, 0, 108);
    lv_obj_set_width(view->brightness_slider, lv_pct(100));

    lv_obj_set_pos(view->temperature_label, 0, 144);
    lv_obj_align(view->temperature_value, LV_ALIGN_TOP_RIGHT, 0, 144);
    lv_obj_set_pos(view->temperature_slider, 0, 180);
    lv_obj_set_width(view->temperature_slider, lv_pct(100));
    return true;
}

bool slate_light_build(lv_obj_t *tile, const slate_config_tile_t *config,
                       const slate_theme_t *theme, slate_light_view_t *view,
                       const char *provider, const char *resource)
{
    if (tile == NULL || config == NULL || theme == NULL || view == NULL ||
        provider == NULL || resource == NULL || config->binding_count == 0) {
        return false;
    }

    view->provider = provider;
    view->resource = resource;
    view->tile = tile;
    view->label_override = config->label != NULL;
    /* A 1x2 tile is valid in schema 1 even though §7.1 has no dedicated
     * presentation for it. Use the compact, width-safe fallback rather than
     * placing the 2x1 horizontal controls outside its content area. */
    view->compact = config->width == 1;
    if (config->icon != NULL) {
        view->icon_override = slate_icon_find(config->icon);
        if (view->icon_override == NULL) {
            view->icon_override = SLATE_ICON_IMAGE_BROKEN_VARIANT;
        }
    }

    const char *name = config->label != NULL ? config->label : resource;
    bool built = view->compact
                     ? build_compact(tile, theme, view, name)
                     : config->width >= 2 && config->height >= 2
                           ? build_large(tile, theme, view, name)
                           : build_wide(tile, theme, view, name);
    if (!built) {
        return false;
    }
    view->icon_font = lv_obj_get_style_text_font(view->icon, LV_PART_MAIN);

    view->identity = make_label(tile, "", theme->caption, theme->warn);
    if (view->identity == NULL) {
        return false;
    }
    lv_label_set_long_mode(view->identity, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(view->identity, lv_pct(100));
    lv_obj_set_style_text_align(view->identity, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(view->identity);
    lv_obj_add_flag(view->identity, LV_OBJ_FLAG_HIDDEN);
    return true;
}

static bool has_value(const slate_resource_t *resource)
{
    return resource->updated_us != 0 &&
           (resource->presentation == SLATE_PRESENT_OK ||
            resource->presentation == SLATE_PRESENT_STALE ||
            resource->presentation == SLATE_PRESENT_UNAVAILABLE);
}

static void temperature_range(slate_light_view_t *view,
                              const slate_capabilities_t *caps, int16_t current,
                              int32_t *minimum, int32_t *maximum)
{
    bool advertised = caps->color_temperature_min != 0 ||
                      caps->color_temperature_max != 0;
    bool current_outside = current != SLATE_STATE_ABSENT &&
                           (current < view->temperature_min ||
                            current > view->temperature_max);

    if (advertised) {
        view->temperature_min = caps->color_temperature_min;
        view->temperature_max = caps->color_temperature_max;
    } else if (!view->temperature_range_valid ||
               view->temperature_range_advertised || current_outside) {
        if (current == SLATE_STATE_ABSENT) {
            view->temperature_min = LIGHT_TEMPERATURE_MIN_K;
            view->temperature_max = LIGHT_TEMPERATURE_MAX_K;
        } else {
            int32_t low = (int32_t) current - LIGHT_TEMPERATURE_SPAN_K;
            int32_t high = (int32_t) current + LIGHT_TEMPERATURE_SPAN_K;
            view->temperature_min = low > 0 ? low : 1;
            view->temperature_max = high <= INT16_MAX ? high : INT16_MAX;
        }
    }
    view->temperature_range_valid = true;
    view->temperature_range_advertised = advertised;
    *minimum = view->temperature_min;
    *maximum = view->temperature_max;
}

void slate_light_update(slate_light_view_t *view, const slate_resource_t *resource,
                        const slate_action_feedback_t *feedback,
                        const slate_theme_t *theme)
{
    bool missing = slate_component_is_placeholder(resource->presentation);
    bool no_current_state = resource->presentation == SLATE_PRESENT_UNAVAILABLE ||
                            resource->presentation == SLATE_PRESENT_STALE;
    bool present = has_value(resource);
    bool healthy = resource->presentation == SLATE_PRESENT_OK;
    bool pending = feedback != NULL && feedback->phase == SLATE_ACTION_PENDING;
    const slate_light_state_t *state = pending ? &feedback->optimistic.light
                                               : &resource->state.light;
    set_pending_animation(view, pending);

    char identity[SLATE_COMPONENT_PLACEHOLDER_MAX];
    slate_component_placeholder_text(resource, identity, sizeof(identity));
    lv_label_set_text(view->identity, identity);
    set_visible(view->identity, missing);
    set_visible(view->icon, !missing);
    set_visible(view->name, !missing);
    set_visible(view->state_dot, !missing && !no_current_state);

    if (!view->label_override && !missing) {
        lv_label_set_text(view->name,
                          resource->name[0] != '\0' ? resource->name : view->resource);
    }

    /* Capabilities describe the resource, not the controls this tile geometry
     * owns. A compact light deliberately has no sliders; a later snapshot may
     * still advertise dimming or colour temperature, and must not turn those
     * absent LVGL objects into call targets. */
    bool brightness_control = view->brightness_slider != NULL &&
                              view->brightness_value != NULL;
    bool temperature_control = view->temperature_slider != NULL &&
                               view->temperature_value != NULL;
    bool brightness_cap = brightness_control && present &&
                          slate_capabilities_have(&resource->capabilities,
                                                  SLATE_ACTION_SET_BRIGHTNESS);
    bool temperature_cap = temperature_control && present &&
                           slate_capabilities_have(
                               &resource->capabilities,
                               SLATE_ACTION_SET_COLOR_TEMPERATURE);
    bool show_temperature = temperature_cap;
    if (!temperature_cap) {
        view->temperature_range_valid = false;
        view->temperature_range_advertised = false;
    }

    set_visible(view->brightness_label, !missing && brightness_cap);
    set_visible(view->brightness_value, !missing && brightness_cap);
    set_visible(view->brightness_slider, !missing && brightness_cap);
    set_visible(view->temperature_label, !missing && show_temperature);
    set_visible(view->temperature_value, !missing && show_temperature);
    set_visible(view->temperature_slider, !missing && show_temperature);

    bool interactive = healthy && !pending;
    bool can_toggle = present && slate_capabilities_have(&resource->capabilities,
                                                         SLATE_ACTION_TOGGLE);
    if (view->compact) {
        if (interactive && can_toggle) {
            lv_obj_add_flag(view->tile, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_remove_flag(view->tile, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    set_enabled(view->brightness_slider, interactive && brightness_cap);
    set_enabled(view->temperature_slider, interactive && show_temperature);

    bool on = present && state->on;
    const char *icon = "-";
    if (!no_current_state) {
        icon = view->icon_override != NULL
                   ? view->icon_override
                   : on ? SLATE_ICON_LIGHTBULB_ON : SLATE_ICON_LIGHTBULB_OUTLINE;
    }
    lv_label_set_text(view->icon, icon);
    lv_obj_set_style_text_font(view->icon,
                               no_current_state ? theme->body : view->icon_font,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(view->icon,
                                lv_color_hex(on ? theme->accent : theme->text_lo),
                                LV_PART_MAIN);
    if (view->state_dot != NULL) {
        lv_obj_set_style_bg_color(view->state_dot,
                                  lv_color_hex(on ? theme->accent : theme->text_lo),
                                  LV_PART_MAIN);
    }

    view->updating = true;
    if (brightness_cap) {
        int32_t value = state->brightness == SLATE_STATE_ABSENT
                            ? resource->capabilities.brightness_min
                            : state->brightness;
        lv_slider_set_range(view->brightness_slider,
                            resource->capabilities.brightness_min,
                            resource->capabilities.brightness_max);
        lv_slider_set_value(view->brightness_slider, value, LV_ANIM_OFF);
        char text[8] = "-";
        if (healthy && state->brightness != SLATE_STATE_ABSENT) {
            snprintf(text, sizeof(text), "%d%%", state->brightness);
        }
        lv_label_set_text(view->brightness_value, text);
    }
    if (show_temperature) {
        int32_t minimum = 0;
        int32_t maximum = 0;
        int16_t current = state->color_temperature;
        temperature_range(view, &resource->capabilities, current, &minimum, &maximum);
        lv_slider_set_range(view->temperature_slider, minimum, maximum);
        lv_slider_set_value(view->temperature_slider,
                            current == SLATE_STATE_ABSENT ? minimum : current,
                            LV_ANIM_OFF);
        char text[16] = "-";
        if (healthy && state->color_temperature != SLATE_STATE_ABSENT) {
            snprintf(text, sizeof(text), "%d K", state->color_temperature);
        }
        lv_label_set_text(view->temperature_value, text);
    }
    view->updating = false;
}
