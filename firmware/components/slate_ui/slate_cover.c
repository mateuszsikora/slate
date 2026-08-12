/* Slate — semantic cover component (DESIGN.md ADR-2/ADR-3, §5.2–§5.3 and §7.2). */

#include "slate_cover.h"

#include <stdio.h>

#include "esp_log.h"

#include "slate_component.h"

static const char *TAG = "slate_cover";

#define COVER_BUTTON_HEIGHT  48
#define COVER_MOTION_OPA_LOW LV_OPA_30
#define COVER_MOTION_PULSE_MS 500

_Static_assert(COVER_BUTTON_HEIGHT >= 48, "cover controls need a 48 px touch target");

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

static void style_plain(lv_obj_t *object)
{
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
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

static void set_clickable(lv_obj_t *object, bool clickable)
{
    if (object == NULL) {
        return;
    }
    if (clickable) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_state(object, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_state(object, LV_STATE_DISABLED);
    }
}

static void motion_opa(void *object, int32_t opacity)
{
    lv_obj_set_style_text_opa(object, (lv_opa_t) opacity, LV_PART_MAIN);
}

static void set_motion_animation(slate_cover_view_t *view, bool moving)
{
    if (moving == view->motion_animation) {
        return;
    }
    if (!moving) {
        lv_anim_delete(view->motion, motion_opa);
        lv_obj_set_style_text_opa(view->motion, LV_OPA_COVER, LV_PART_MAIN);
        view->motion_animation = false;
        return;
    }

    /* `var` is the LVGL object itself. lv_obj_delete() therefore removes the
     * animation during every ADR-1 tree rebuild (S-1's lifetime requirement). */
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, view->motion);
    lv_anim_set_exec_cb(&animation, motion_opa);
    lv_anim_set_values(&animation, COVER_MOTION_OPA_LOW, LV_OPA_COVER);
    lv_anim_set_duration(&animation, COVER_MOTION_PULSE_MS);
    lv_anim_set_reverse_duration(&animation, COVER_MOTION_PULSE_MS);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    view->motion_animation = lv_anim_start(&animation) != NULL;
}

static void dispatch_action(slate_cover_view_t *view, slate_action_t action)
{
    const slate_action_request_t request = {
        .provider = view->provider,
        .resource = view->resource,
        .action = action,
        .value_type = SLATE_ACTION_VALUE_NONE,
    };
    esp_err_t err = slate_action_dispatch(&request, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s:%s %s refused: %s", view->provider, view->resource,
                 slate_action_str(action), esp_err_to_name(err));
    }
}

static void toggle_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        dispatch_action(lv_event_get_user_data(event), SLATE_ACTION_TOGGLE);
    }
}

static void open_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        dispatch_action(lv_event_get_user_data(event), SLATE_ACTION_OPEN);
    }
}

static void stop_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        dispatch_action(lv_event_get_user_data(event), SLATE_ACTION_STOP);
    }
}

static void close_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        dispatch_action(lv_event_get_user_data(event), SLATE_ACTION_CLOSE);
    }
}

static lv_obj_t *make_button(lv_obj_t *tile, const char *icon,
                             const slate_theme_t *theme, lv_event_cb_t callback,
                             slate_cover_view_t *view)
{
    lv_obj_t *button = lv_obj_create(tile);
    if (button == NULL) {
        return NULL;
    }
    style_plain(button);
    lv_obj_set_height(button, COVER_BUTTON_HEIGHT);
    lv_obj_set_style_bg_color(button, lv_color_hex(theme->surface_alt), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, theme->radius > 4 ? theme->radius - 4 : theme->radius,
                            LV_PART_MAIN);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, view);

    lv_obj_t *label = make_label(button, icon, theme->icons, theme->text_hi);
    if (label == NULL) {
        return NULL;
    }
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_center(label);
    return button;
}

static bool build_common(lv_obj_t *tile, const slate_config_tile_t *config,
                         const slate_theme_t *theme, slate_cover_view_t *view)
{
    const char *name = config->label != NULL ? config->label : view->resource;
    view->icon = make_label(tile, SLATE_ICON_WINDOW_SHUTTER, theme->icons_large,
                            theme->text_lo);
    view->name = make_label(tile, name, theme->body, theme->text_hi);
    view->position = make_label(tile, "-", theme->body, theme->text_hi);
    view->motion = make_label(tile, SLATE_ICON_ARROW_UP, theme->icons, theme->accent);
    view->identity = make_label(tile, "", theme->caption, theme->warn);
    if (view->icon == NULL || view->name == NULL || view->position == NULL ||
        view->motion == NULL || view->identity == NULL) {
        return false;
    }

    lv_label_set_long_mode(view->name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->position, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_long_mode(view->identity, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(view->identity, lv_pct(100));
    lv_obj_set_style_text_align(view->identity, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(view->identity);
    lv_obj_add_flag(view->identity, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->motion, LV_OBJ_FLAG_HIDDEN);
    return true;
}

static bool build_compact(lv_obj_t *tile, const slate_theme_t *theme,
                          slate_cover_view_t *view)
{
    lv_obj_set_pos(view->icon, 0, 0);
    lv_obj_set_size(view->icon, 48, 48);
    lv_obj_set_style_text_align(view->icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(view->motion, 62, 8);
    lv_obj_set_size(view->motion, 32, 32);
    lv_obj_set_style_text_align(view->motion, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(view->position, 96, 8);
    lv_obj_set_width(view->position, 60);
    lv_obj_set_width(view->name, lv_pct(100));
    lv_obj_align(view->name, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(tile, toggle_event, LV_EVENT_CLICKED, view);
    (void) theme;
    return true;
}

static bool build_extended(lv_obj_t *tile, const slate_theme_t *theme,
                           slate_cover_view_t *view)
{
    view->open_button = make_button(tile, SLATE_ICON_ARROW_UP, theme, open_event, view);
    view->stop_button = make_button(tile, SLATE_ICON_STOP, theme, stop_event, view);
    view->close_button = make_button(tile, SLATE_ICON_ARROW_DOWN, theme, close_event, view);
    if (view->open_button == NULL || view->stop_button == NULL ||
        view->close_button == NULL) {
        return false;
    }

    if (view->vertical) {
        lv_obj_set_pos(view->icon, 0, 0);
        lv_obj_set_size(view->icon, 42, 42);
        lv_obj_set_style_text_align(view->icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(view->name, 50, 0);
        lv_obj_set_width(view->name, 78);
        lv_obj_set_pos(view->position, 50, 28);
        lv_obj_set_width(view->position, 78);
        lv_obj_set_pos(view->motion, 128, 22);
        lv_obj_set_size(view->motion, 28, 28);
        lv_obj_set_width(view->open_button, lv_pct(100));
        lv_obj_set_pos(view->open_button, 0, 72);
        lv_obj_set_width(view->stop_button, lv_pct(100));
        lv_obj_set_pos(view->stop_button, 0, 128);
        lv_obj_set_width(view->close_button, lv_pct(100));
        lv_obj_set_pos(view->close_button, 0, 184);
    } else {
        int32_t content_width = lv_obj_calc_dynamic_width(tile, LV_STYLE_WIDTH) -
                                lv_obj_get_style_space_left(tile, LV_PART_MAIN) -
                                lv_obj_get_style_space_right(tile, LV_PART_MAIN);
        int32_t controls_x = 52;
        int32_t button_gap = 6;
        int32_t button_width = (content_width - controls_x - button_gap * 2) / 3;
        lv_obj_set_pos(view->icon, 0, 0);
        lv_obj_set_size(view->icon, 44, 44);
        lv_obj_set_style_text_align(view->icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(view->name, 52, 0);
        lv_obj_set_width(view->name, content_width - 202);
        lv_obj_set_pos(view->position, content_width - 140, 0);
        lv_obj_set_width(view->position, 100);
        lv_obj_set_pos(view->motion, content_width - 32, 0);
        lv_obj_set_size(view->motion, 32, 32);
        lv_obj_set_pos(view->open_button, controls_x, 48);
        lv_obj_set_width(view->open_button, button_width);
        lv_obj_set_pos(view->stop_button, controls_x + button_width + button_gap, 48);
        lv_obj_set_width(view->stop_button, button_width);
        lv_obj_set_pos(view->close_button,
                       controls_x + (button_width + button_gap) * 2, 48);
        lv_obj_set_width(view->close_button, button_width);
    }
    return true;
}

bool slate_cover_build(lv_obj_t *tile, const slate_config_tile_t *config,
                       const slate_theme_t *theme, slate_cover_view_t *view,
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
    view->compact = config->width == 1 && config->height == 1;
    view->vertical = config->width == 1 && config->height > 1;
    if (config->icon != NULL) {
        view->icon_override = slate_icon_find(config->icon);
        if (view->icon_override == NULL) {
            view->icon_override = SLATE_ICON_IMAGE_BROKEN_VARIANT;
        }
    }

    return build_common(tile, config, theme, view) &&
           (view->compact ? build_compact(tile, theme, view)
                          : build_extended(tile, theme, view));
}

static bool has_value(const slate_resource_t *resource)
{
    return resource->updated_us != 0 &&
           (resource->presentation == SLATE_PRESENT_OK ||
            resource->presentation == SLATE_PRESENT_STALE ||
            resource->presentation == SLATE_PRESENT_UNAVAILABLE);
}

static const char *position_icon(const slate_cover_view_t *view,
                                 const slate_cover_state_t *state)
{
    if (view->icon_override != NULL) {
        return view->icon_override;
    }
    if (state->position == SLATE_STATE_ABSENT) {
        return SLATE_ICON_WINDOW_SHUTTER_ALERT;
    }
    if (state->position <= 0) {
        return SLATE_ICON_WINDOW_SHUTTER;
    }
    if (state->position >= 100) {
        return SLATE_ICON_WINDOW_SHUTTER_OPEN;
    }
    return SLATE_ICON_BLINDS;
}

void slate_cover_update(slate_cover_view_t *view, const slate_resource_t *resource,
                        const slate_action_feedback_t *feedback,
                        const slate_theme_t *theme)
{
    bool missing = slate_component_is_placeholder(resource->presentation);
    bool present = has_value(resource);
    bool healthy = resource->presentation == SLATE_PRESENT_OK;
    bool pending = feedback != NULL && feedback->phase == SLATE_ACTION_PENDING;
    const slate_cover_state_t *state = pending ? &feedback->optimistic.cover
                                               : &resource->state.cover;
    bool moving = healthy && state->motion != SLATE_COVER_IDLE;

    char identity[SLATE_COMPONENT_PLACEHOLDER_MAX];
    slate_component_placeholder_text(resource, identity, sizeof(identity));
    lv_label_set_text(view->identity, identity);
    set_visible(view->identity, missing);
    set_visible(view->icon, !missing);
    set_visible(view->name, !missing);
    set_visible(view->position, !missing);
    set_visible(view->motion, !missing && moving);
    set_visible(view->open_button, !missing);
    set_visible(view->stop_button, !missing);
    set_visible(view->close_button, !missing);

    if (!view->label_override && !missing) {
        lv_label_set_text(view->name,
                          resource->name[0] != '\0' ? resource->name : view->resource);
    }

    if (present) {
        lv_label_set_text(view->icon, position_icon(view, state));
        lv_obj_set_style_text_font(view->icon, theme->icons_large, LV_PART_MAIN);
    } else {
        lv_label_set_text(view->icon, "-");
        lv_obj_set_style_text_font(view->icon, theme->body, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(view->icon,
                                lv_color_hex(moving ? theme->accent : theme->text_lo),
                                LV_PART_MAIN);

    char position[8] = "-";
    if (healthy && state->position != SLATE_STATE_ABSENT) {
        snprintf(position, sizeof(position), "%d%%", state->position);
    }
    lv_label_set_text(view->position, position);
    lv_label_set_text(view->motion,
                      state->motion == SLATE_COVER_CLOSING ? SLATE_ICON_ARROW_DOWN
                                                           : SLATE_ICON_ARROW_UP);
    set_motion_animation(view, !missing && moving);

    bool interactive = healthy && !pending;
    bool can_toggle = present && slate_capabilities_have(&resource->capabilities,
                                                         SLATE_ACTION_TOGGLE);
    bool can_open = present && slate_capabilities_have(&resource->capabilities,
                                                       SLATE_ACTION_OPEN);
    bool can_stop = present && moving &&
                    slate_capabilities_have(&resource->capabilities,
                                            SLATE_ACTION_STOP);
    bool can_close = present && slate_capabilities_have(&resource->capabilities,
                                                        SLATE_ACTION_CLOSE);
    if (view->compact) {
        set_clickable(view->tile, interactive && can_toggle);
    } else {
        set_clickable(view->open_button, interactive && can_open);
        set_clickable(view->stop_button, interactive && can_stop);
        set_clickable(view->close_button, interactive && can_close);
    }
}
