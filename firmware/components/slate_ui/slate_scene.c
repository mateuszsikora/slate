/* Slate — semantic scene component (DESIGN.md ADR-2/ADR-3, §5.2–§5.3 and §7.4). */

#include "slate_scene.h"

#include <stdio.h>

#include "esp_log.h"

#include "slate_component.h"

static const char *TAG = "slate_scene";

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

static void set_visible(lv_obj_t *object, bool visible)
{
    if (visible) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

static void activate_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (!slate_component_actions_enabled()) {
        return;
    }
    slate_scene_view_t *view = lv_event_get_user_data(event);
    const slate_action_request_t request = {
        .provider = view->provider,
        .resource = view->resource,
        .action = SLATE_ACTION_ACTIVATE,
        .value_type = SLATE_ACTION_VALUE_NONE,
    };
    esp_err_t err = slate_action_dispatch(&request, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s:%s activate refused: %s", view->provider, view->resource,
                 esp_err_to_name(err));
    }
}

static lv_obj_t *build_button(lv_obj_t *tile, const slate_config_tile_t *config,
                              const slate_theme_t *theme)
{
    if (config->binding_count == 1) {
        return tile;
    }

    lv_obj_t *button = lv_obj_create(tile);
    if (button == NULL) {
        return NULL;
    }
    style_plain(button);
    lv_obj_set_height(button, lv_pct(100));
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_bg_color(button, lv_color_hex(theme->surface_alt), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, theme->radius > 4 ? theme->radius - 4 : theme->radius,
                            LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 10, LV_PART_MAIN);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    return button;
}

bool slate_scene_build(lv_obj_t *tile, const slate_config_tile_t *config,
                       const slate_theme_t *theme, slate_scene_view_t *view,
                       size_t index, const char *provider, const char *resource)
{
    if (tile == NULL || config == NULL || theme == NULL || view == NULL ||
        provider == NULL || resource == NULL || index >= config->binding_count ||
        config->binding_count == 0 || config->binding_count > 5) {
        return false;
    }

    if (index == 0 && config->binding_count > 1) {
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_layout(tile, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(tile, theme->gap, LV_PART_MAIN);
    }

    view->provider = provider;
    view->resource = resource;
    view->compact = config->binding_count == 1;
    view->label_override = view->compact && config->label != NULL;
    if (config->icon != NULL) {
        view->icon_override = slate_icon_find(config->icon);
        if (view->icon_override == NULL) {
            view->icon_override = SLATE_ICON_IMAGE_BROKEN_VARIANT;
        }
    }

    view->button = build_button(tile, config, theme);
    if (view->button == NULL) {
        return false;
    }
    const char *name = view->label_override ? config->label : resource;
    view->icon = make_label(view->button,
                            view->icon_override != NULL ? view->icon_override
                                                        : SLATE_ICON_PLAYLIST_PLAY,
                            theme->icons, theme->accent);
    view->name = make_label(view->button, name,
                            view->compact ? theme->body : theme->caption,
                            theme->text_hi);
    view->identity = make_label(view->button, "", theme->caption, theme->warn);
    if (view->icon == NULL || view->name == NULL || view->identity == NULL) {
        return false;
    }

    lv_obj_set_size(view->icon, 38, 34);
    lv_obj_set_style_text_align(view->icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(view->icon, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_long_mode(view->name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(view->name, lv_pct(100));
    lv_obj_set_style_text_align(view->name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(view->name, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_label_set_long_mode(view->identity, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(view->identity, lv_pct(100));
    lv_obj_set_style_text_align(view->identity, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(view->identity);
    lv_obj_add_flag(view->identity, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(view->button, activate_event, LV_EVENT_CLICKED, view);
    return true;
}

void slate_scene_update(slate_scene_view_t *view, const slate_resource_t *resource,
                        const slate_action_feedback_t *feedback,
                        const slate_theme_t *theme)
{
    bool missing = slate_component_is_placeholder(resource->presentation);
    bool no_current_state = resource->presentation == SLATE_PRESENT_UNAVAILABLE ||
                            resource->presentation == SLATE_PRESENT_STALE;
    bool pending = feedback != NULL && feedback->phase == SLATE_ACTION_PENDING;
    bool success = feedback != NULL && feedback->phase == SLATE_ACTION_SUCCESS;
    bool busy = pending || success;
    bool interactive = resource->presentation == SLATE_PRESENT_OK && !busy &&
                       slate_capabilities_have(&resource->capabilities,
                                               SLATE_ACTION_ACTIVATE);

    char identity[SLATE_COMPONENT_PLACEHOLDER_MAX];
    slate_component_placeholder_text(resource, identity, sizeof(identity));
    lv_label_set_text(view->identity, identity);
    set_visible(view->identity, missing);
    set_visible(view->icon, !missing);
    set_visible(view->name, !missing);
    lv_label_set_text(view->icon,
                      no_current_state ? "-"
                                       : view->icon_override != NULL
                                             ? view->icon_override
                                             : SLATE_ICON_PLAYLIST_PLAY);
    lv_obj_set_style_text_font(view->icon,
                               no_current_state ? theme->body : theme->icons,
                               LV_PART_MAIN);
    if (!view->label_override && !missing) {
        lv_label_set_text(view->name,
                          resource->name[0] != '\0' ? resource->name : view->resource);
    }

    if (interactive) {
        lv_obj_add_flag(view->button, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(view->button, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_bg_color(
        view->button,
        lv_color_hex(success ? theme->accent
                             : view->compact ? theme->surface : theme->surface_alt),
        LV_PART_MAIN);
    lv_obj_set_style_text_color(view->icon,
                                lv_color_hex(success ? theme->on_accent : theme->accent),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(view->name,
                                lv_color_hex(success ? theme->on_accent : theme->text_hi),
                                LV_PART_MAIN);
}
