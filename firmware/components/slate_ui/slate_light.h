/* Slate — semantic light component internal to the UI runtime. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "slate_action.h"
#include "slate_config.h"
#include "slate_state.h"
#include "slate_theme.h"

typedef struct {
    const char *provider;
    const char *resource;
    const char *icon_override;
    bool label_override;
    bool compact;
    bool updating;
    bool pending_animation;
    bool temperature_range_valid;
    bool temperature_range_advertised;
    int32_t temperature_min;
    int32_t temperature_max;
    lv_obj_t *tile;
    lv_obj_t *icon;
    lv_obj_t *name;
    lv_obj_t *state_dot;
    lv_obj_t *identity;
    lv_obj_t *brightness_label;
    lv_obj_t *brightness_value;
    lv_obj_t *brightness_slider;
    lv_obj_t *temperature_label;
    lv_obj_t *temperature_value;
    lv_obj_t *temperature_slider;
} slate_light_view_t;

/** Build §7.1's themed 1x1, 2x1 or 2x2 presentation into a tile shell. */
bool slate_light_build(lv_obj_t *tile, const slate_config_tile_t *config,
                       const slate_theme_t *theme, slate_light_view_t *view,
                       const char *provider, const char *resource);

/** Apply normalized state plus the common action bus's optimistic/error overlay. */
void slate_light_update(slate_light_view_t *view, const slate_resource_t *resource,
                        const slate_action_feedback_t *feedback,
                        const slate_theme_t *theme);
