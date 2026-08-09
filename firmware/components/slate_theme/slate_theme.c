/* Slate — Midnight theme registry (DESIGN.md §8, ADR-6). */

#include "slate_theme.h"

#include <string.h>

LV_FONT_DECLARE(slate_font_hero_44);
LV_FONT_DECLARE(slate_font_body_20);
LV_FONT_DECLARE(slate_font_caption_15);
LV_FONT_DECLARE(slate_font_icons_28);
LV_FONT_DECLARE(slate_font_icons_48);

static const slate_theme_t MIDNIGHT = {
    .id = "midnight",
    .bg = 0x101114,
    .surface = 0x1A1C21,
    .surface_alt = 0x22252B,
    .text_hi = 0xF2F5F9,
    .text_lo = 0x8A94A6,
    .accent = 0x6C8CFF,
    .warn = 0xF5A524,
    .radius = 18,
    .gap = 12,
    .pad = 14,
    .hero = &slate_font_hero_44,
    .body = &slate_font_body_20,
    .caption = &slate_font_caption_15,
    .icons = &slate_font_icons_28,
    .icons_large = &slate_font_icons_48,
};

static const slate_theme_t *const THEMES[] = {&MIDNIGHT};

const slate_theme_t *slate_theme_default(void)
{
    return THEMES[0];
}

const slate_theme_t *slate_theme_find(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < slate_theme_count(); i++) {
        if (strcmp(THEMES[i]->id, id) == 0) {
            return THEMES[i];
        }
    }
    return NULL;
}

size_t slate_theme_count(void)
{
    return sizeof(THEMES) / sizeof(THEMES[0]);
}

const slate_theme_t *slate_theme_at(size_t index)
{
    return index < slate_theme_count() ? THEMES[index] : NULL;
}
