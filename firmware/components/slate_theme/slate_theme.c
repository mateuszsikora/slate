/* Slate — firmware-owned theme registry (DESIGN.md §8, ADR-6). */

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
    .on_accent = 0x101114,
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

static const slate_theme_t MINIMAL_LIGHT = {
    .id = "minimal-light",
    .bg = 0xD8D3C9,
    .surface = 0xF7F5F0,
    .surface_alt = 0xC9C3B8,
    .text_hi = 0x1F2933,
    .text_lo = 0x4A5661,
    .accent = 0x2F6671,
    .on_accent = 0xFFFFFF,
    .warn = 0x9A442F,
    .radius = 18,
    .gap = 12,
    .pad = 14,
    .hero = &slate_font_hero_44,
    .body = &slate_font_body_20,
    .caption = &slate_font_caption_15,
    .icons = &slate_font_icons_28,
    .icons_large = &slate_font_icons_48,
};

static const slate_diagnostic_palette_t DIAGNOSTICS = {
    .touch_idle = 0x3A3F4A,
    .touch_hit = 0x2FBF71,
    .crosshair = 0x00E5FF,
    .edge = 0xFFFFFF,
    .rgb = {
        0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
        0xFF00FF, 0xFF0000, 0x0000FF, 0x000000,
    },
    .greyscale = {
        0xFFFFFF, 0xDADADA, 0xB6B6B6, 0x919191,
        0x6D6D6D, 0x494949, 0x242424, 0x000000,
    },
};

static const slate_theme_t *const THEMES[] = {&MIDNIGHT, &MINIMAL_LIGHT};

typedef struct {
    const char *name;
    const char *glyph;
} icon_entry_t;

#define ICON_ENTRY(name, glyph) {name, glyph},
static const icon_entry_t ICONS[] = {SLATE_ICON_FOREACH(ICON_ENTRY)};
#undef ICON_ENTRY

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

const char *slate_icon_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(ICONS) / sizeof(ICONS[0]); i++) {
        if (strcmp(ICONS[i].name, name) == 0) {
            return ICONS[i].glyph;
        }
    }
    return NULL;
}

const slate_diagnostic_palette_t *slate_diagnostic_palette(void)
{
    return &DIAGNOSTICS;
}
