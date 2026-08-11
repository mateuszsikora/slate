/* Slate — semantic sensor presentation internal to the UI runtime. */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

#include "slate_config.h"
#include "slate_state.h"
#include "slate_theme.h"

typedef struct {
    lv_obj_t *icon;
    lv_obj_t *value;
    lv_obj_t *unit;
    const char *icon_override;
} slate_sensor_view_t;

/** Build §7.3's themed 1x1 or 2x1 presentation into an existing tile shell. */
bool slate_sensor_build(lv_obj_t *tile, const slate_config_tile_t *config,
                        const slate_theme_t *theme, slate_sensor_view_t *view,
                        lv_obj_t **name_label);

/** Apply one provider-neutral sensor resource; no provider payload is expressible here. */
void slate_sensor_update(const slate_sensor_view_t *view, const slate_resource_t *resource,
                         const slate_theme_t *theme);
