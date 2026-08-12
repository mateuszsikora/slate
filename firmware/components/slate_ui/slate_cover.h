/* Slate — semantic cover component internal to the UI runtime. */

#pragma once

#include <stdbool.h>

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
    bool vertical;
    bool motion_animation;
    lv_obj_t *tile;
    lv_obj_t *icon;
    lv_obj_t *name;
    lv_obj_t *position;
    lv_obj_t *motion;
    lv_obj_t *identity;
    lv_obj_t *open_button;
    lv_obj_t *stop_button;
    lv_obj_t *close_button;
} slate_cover_view_t;

/** Build §7.2's themed 1x1, 1x2 or 2x1 presentation into a tile shell. */
bool slate_cover_build(lv_obj_t *tile, const slate_config_tile_t *config,
                       const slate_theme_t *theme, slate_cover_view_t *view,
                       const char *provider, const char *resource);

/** Apply normalized position/motion plus the common action bus feedback. */
void slate_cover_update(slate_cover_view_t *view, const slate_resource_t *resource,
                        const slate_action_feedback_t *feedback,
                        const slate_theme_t *theme);
