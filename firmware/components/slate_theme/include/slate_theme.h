/*
 * Slate — firmware-owned visual tokens and compiled type faces.
 *
 * Components consume this vocabulary rather than naming colours or font
 * files. Themes are capabilities of the running firmware (ADR-6), so the same
 * registry also supplies GET /api/v1/info.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "slate_icons.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;

    uint32_t bg;
    uint32_t surface;
    uint32_t surface_alt;
    uint32_t text_hi;
    uint32_t text_lo;
    uint32_t accent;
    uint32_t warn;

    int32_t radius;
    int32_t gap;
    int32_t pad;

    const lv_font_t *hero;
    const lv_font_t *body;
    const lv_font_t *caption;
    const lv_font_t *icons;
    const lv_font_t *icons_large;
} slate_theme_t;

/** @brief The theme used when a configuration does not select one. */
const slate_theme_t *slate_theme_default(void);

/** @brief Look up a compiled theme by its stable configuration id. */
const slate_theme_t *slate_theme_find(const char *id);

/** @brief Number of theme capabilities compiled into this firmware. */
size_t slate_theme_count(void);

/** @brief Theme capability at @p index, or NULL when out of range. */
const slate_theme_t *slate_theme_at(size_t index);

#ifdef __cplusplus
}
#endif
