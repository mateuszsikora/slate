/* Slate — semantic scene component internal to the UI runtime. */

#pragma once

#include <stdbool.h>
#include <stddef.h>

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
    lv_obj_t *button;
    lv_obj_t *icon;
    lv_obj_t *name;
    lv_obj_t *identity;
} slate_scene_view_t;

/** Build one entry of §7.4's 1x1 tile or 4x1 scene bar. */
bool slate_scene_build(lv_obj_t *tile, const slate_config_tile_t *config,
                       const slate_theme_t *theme, slate_scene_view_t *view,
                       size_t index, const char *provider, const char *resource);

/** Apply one stateless resource and the action bus's pending/result feedback. */
void slate_scene_update(slate_scene_view_t *view, const slate_resource_t *resource,
                        const slate_action_feedback_t *feedback,
                        const slate_theme_t *theme);
