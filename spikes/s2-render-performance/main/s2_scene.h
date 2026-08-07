/*
 * The scene under measurement: a page of design.md §7 components on the §3.2
 * grid, with the §8 type scale and icon font.
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

/*
 * Tiles per page.
 *
 * design.md §3.2 fixes the content grid at 4 × 3 — twelve cells — and the
 * smallest tile at 1×1, so 12 is the most a configuration can express and it is
 * the number the verdict is about. The issue asks for 20, which is 1.7× what
 * the format permits; it is measured as an overload case on a relaxed 5 × 4
 * grid, to record whether a wider grid is even on the table later. Reporting 20
 * as "the maximum" would name a number no configuration can reach.
 */
#ifndef S2_TILES
#define S2_TILES 12
#endif

#if S2_TILES > 12
#define S2_GRID_COLS 5
#define S2_GRID_ROWS 4
#else
#define S2_GRID_COLS 4
#define S2_GRID_ROWS 3
#endif

#define S2_PAGE_COUNT 4

/* Builds page `index` (0..S2_PAGE_COUNT-1) onto the active screen. */
lv_obj_t *s2_scene_build(uint32_t index);
void      s2_scene_destroy(lv_obj_t *root);

/* One entity update, as §5.2 would deliver it: a new state for one entity,
 * pushed into the widget that shows it. Called at 10 Hz. */
void s2_scene_update(uint32_t seq);

uint32_t s2_scene_tile_count(void);
