/*
 * Slate — rebuild idempotency spike (S-1).
 *
 * design.md §13 states the experiment: build an LVGL tree from JSON, destroy it,
 * repeat 500 times, logging lv_mem_monitor(). Pass if free heap returns to its
 * starting value within 1% with no downward trend. §6.4 says why it gates
 * everything: ADR-1 forbids hardcoded layouts, so every PUT /config destroys a
 * tree and builds another, forever. A runtime that leaks a few hundred bytes per
 * rebuild is a runtime that dies on the wall after a few days of editing.
 *
 * Three choices here are deliberate and are what separate a useful answer from a
 * trivially green one:
 *
 *   1. The tree *varies* between cycles. Rebuilding an identical tree 500 times
 *      lets the allocator hand back the same blocks in the same order, which
 *      hides a leak behind reuse. §6.4's real case is a *different* config, so
 *      each cycle generates a different tile count, mix and label set.
 *   2. The tiles are semantic components (ADR-2), not bare rectangles. A tree of
 *      500 empty lv_obj containers would pass and prove nothing: the things that
 *      actually leak in LVGL are local styles, label text buffers and running
 *      animations, so light/cover/sensor/scene all create them on purpose.
 *   3. Marquee labels (§7.5 text overflow) and pending-state pulses (§7.5) are
 *      included precisely because they leave animations attached to objects.
 *      Deleting an object with a live animation is the classic LVGL leak, and a
 *      spike that avoided it would be measuring the easy half of the problem.
 *
 * Out of scope, stated so the result is not read as broader than it is: this is
 * a dummy display. No RGB panel, no GT911, no CH422G — that is #6 and #7 in M1.
 * The result speaks about the widget tree's allocation lifecycle, not about the
 * display pipeline.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "cJSON.h"
#include "lvgl.h"

#include "s1_lvgl_pool.h"

static const char *TAG = "s1";

/* -------------------------------------------------------------------------
 * Experiment parameters
 * ------------------------------------------------------------------------- */

/* §13: "repeat 500 times". */
#define S1_CYCLES 500

/*
 * Negative control.
 *
 * A spike that reports "no drift over 500 cycles" is worthless unless the
 * harness can be shown to detect drift that is really there — otherwise a
 * measurement bug and a clean runtime produce the same green output. Building
 * with `idf.py -DS1_LEAK_BYTES=64 build` leaks that many bytes of LVGL heap per
 * tile per cycle and must turn the verdict red. s1.md reports both runs.
 */
#ifndef S1_LEAK_BYTES
#define S1_LEAK_BYTES 0
#endif

/*
 * Cycles run before the baseline is taken. LVGL fills lazily-initialised caches
 * (glyph cache, style transition scratch, the invalidated-area buffer) on first
 * use and keeps them. That is a one-time step, not a leak, but a baseline taken
 * at cycle 0 would charge the step to the trend and fail a healthy runtime.
 * s1.md reports the pre-warmup number too, so the step stays visible.
 */
#define S1_WARMUP_CYCLES 10

/* Grid geometry — design.md §3.2. */
#define S1_SCREEN_W    800
#define S1_SCREEN_H    480
#define S1_BAR_H        56
#define S1_GRID_COLS     4
#define S1_GRID_ROWS     3
#define S1_CELL_W      184
#define S1_CELL_H      124
#define S1_GAP          12
#define S1_MARGIN       14

/* Midnight theme tokens — design.md §8. */
#define S1_COL_BG          0x101114
#define S1_COL_SURFACE     0x1A1C21
#define S1_COL_SURFACE_ALT 0x22252B
#define S1_COL_TEXT_HI     0xF2F5F9
#define S1_COL_TEXT_LO     0x8A94A6
#define S1_COL_ACCENT      0x6C8CFF
#define S1_COL_WARN        0xF5A524
#define S1_RADIUS           18

/* -------------------------------------------------------------------------
 * LVGL heap backing store (design.md §6.2)
 * ------------------------------------------------------------------------- */

void *s1_lvgl_pool_alloc(size_t size)
{
    void *pool = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Printed rather than logged: lv_mem_init() runs before esp_log is useful
     * for anything the reader will see in context, and this line is the proof
     * that the 2 MB pool really landed in PSRAM. */
    printf("s1: LVGL pool %u bytes at %p (PSRAM)\n", (unsigned)size, pool);
    return pool;
}

/* -------------------------------------------------------------------------
 * Dummy display
 *
 * LVGL needs a display to parent objects onto and a draw buffer to render into;
 * it does not need the pixels to go anywhere. Rendering is left switched on
 * deliberately — a tree that is built but never drawn skips the draw-time
 * allocations (glyph lookups, layer buffers) that this spike wants to include.
 * ------------------------------------------------------------------------- */

/* ~1/10 screen in internal SRAM, per design.md §6.2. */
#define S1_DRAW_BUF_LINES 48
#define S1_DRAW_BUF_BYTES (S1_SCREEN_W * S1_DRAW_BUF_LINES * 2)

static void s1_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static uint32_t s1_tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void s1_display_init(void)
{
    void *draw_buf = heap_caps_malloc(S1_DRAW_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    assert(draw_buf != NULL);

    lv_display_t *disp = lv_display_create(S1_SCREEN_W, S1_SCREEN_H);
    lv_display_set_flush_cb(disp, s1_flush_cb);
    lv_display_set_buffers(disp, draw_buf, NULL, S1_DRAW_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(S1_COL_BG), LV_PART_MAIN);
}

/* -------------------------------------------------------------------------
 * Configuration generation
 *
 * Stands in for PUT /config: the builder below is handed a JSON *string*, the
 * same way the real runtime will be, so cJSON parse and free are inside the
 * measured cycle rather than short-circuited by passing structs around.
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *type;
    const char *entity;
    const char *label; /* NULL: fall back to friendly_name, per §3.3 */
    uint8_t     w;
    uint8_t     h;
} s1_recipe_t;

/*
 * The four launch components (§7) plus one unknown type. §3.1 requires an
 * unknown type to render a placeholder rather than crash, and the placeholder
 * path allocates too — leaving it out would exempt it from the measurement.
 * The overlong label is the §7.5 text-overflow case; it becomes a marquee.
 */
static const s1_recipe_t S1_RECIPES[] = {
    { "light",  "light.living_room",              "Living room",                             2, 1 },
    { "light",  "light.kitchen_spots",            NULL,                                      1, 1 },
    { "light",  "light.bedroom_ceiling",          "Bedroom ceiling downlights and cove",     2, 2 },
    { "cover",  "cover.living_room_blind",        NULL,                                      1, 2 },
    { "cover",  "cover.study_blind",              "Study",                                   2, 1 },
    { "sensor", "sensor.living_room_temperature", NULL,                                      1, 1 },
    { "sensor", "sensor.outdoor_pressure",        "Pressure",                                2, 1 },
    { "sensor", "sensor.hall_humidity",           "Hall",                                    1, 1 },
    { "scene",  NULL,                             "Scenes",                                  4, 1 },
    { "scene",  "scene.relax",                    "Relax",                                   1, 1 },
    { "thermostat", "climate.hall",               "Hall thermostat",                         1, 1 },
};
#define S1_RECIPE_COUNT (sizeof(S1_RECIPES) / sizeof(S1_RECIPES[0]))

/* Deterministic, so a failing cycle can be re-run in isolation. */
static uint32_t s1_rand(uint32_t *state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return *state >> 16;
}

/* Returns true if a w×h tile fits at (col,row) with every cell still free. */
static bool s1_cells_free(const bool occupied[S1_GRID_ROWS][S1_GRID_COLS],
                          int col, int row, int w, int h)
{
    if (col + w > S1_GRID_COLS || row + h > S1_GRID_ROWS) {
        return false;
    }
    for (int r = row; r < row + h; r++) {
        for (int c = col; c < col + w; c++) {
            if (occupied[r][c]) {
                return false;
            }
        }
    }
    return true;
}

/*
 * Builds a §3.3-shaped configuration for this cycle. Tile count, mix and
 * placement all vary with the cycle number — see the file header for why that
 * matters. Returns a heap string the caller frees.
 */
static char *s1_generate_config(int cycle, int *tiles_out)
{
    const size_t cap = 8192; /* well under the §3.1 limit of 64 KB */
    char *json = malloc(cap);
    assert(json != NULL);

    uint32_t rng = 0x5EED0000u ^ (uint32_t)cycle;
    bool occupied[S1_GRID_ROWS][S1_GRID_COLS] = { 0 };

    size_t len = 0;
    len += snprintf(json + len, cap - len,
                    "{\"schema\":1,\"theme\":\"midnight\",\"home_page\":\"home\","
                    "\"pages\":[{\"id\":\"home\",\"title\":\"Home\",\"tiles\":[");

    const int wanted = 3 + (int)(s1_rand(&rng) % 9); /* 3..11 tiles */
    int placed = 0;

    for (int attempt = 0; attempt < wanted * 4 && placed < wanted; attempt++) {
        const s1_recipe_t *r = &S1_RECIPES[s1_rand(&rng) % S1_RECIPE_COUNT];

        int col = -1, row = -1;
        for (int rr = 0; rr < S1_GRID_ROWS && row < 0; rr++) {
            for (int cc = 0; cc < S1_GRID_COLS; cc++) {
                if (s1_cells_free(occupied, cc, rr, r->w, r->h)) {
                    col = cc;
                    row = rr;
                    break;
                }
            }
        }
        if (row < 0) {
            continue; /* this recipe does not fit any more; try another */
        }
        for (int rr = row; rr < row + r->h; rr++) {
            for (int cc = col; cc < col + r->w; cc++) {
                occupied[rr][cc] = true;
            }
        }

        len += snprintf(json + len, cap - len,
                        "%s{\"id\":\"t%d\",\"type\":\"%s\",\"pos\":[%d,%d],\"size\":[%d,%d]",
                        placed ? "," : "", placed, r->type, col, row, r->w, r->h);
        if (r->entity) {
            len += snprintf(json + len, cap - len, ",\"entity\":\"%s\"", r->entity);
        } else if (strcmp(r->type, "scene") == 0) {
            /* §7.4: the 4×1 variant is a bar of 2–5 scenes. */
            len += snprintf(json + len, cap - len,
                            ",\"entities\":[\"scene.relax\",\"scene.goodnight\",\"scene.away\"]");
        }
        if (r->label) {
            len += snprintf(json + len, cap - len, ",\"label\":\"%s\"", r->label);
        }
        len += snprintf(json + len, cap - len, "}");
        placed++;
    }

    len += snprintf(json + len, cap - len, "]}]}");
    assert(len < cap);

    *tiles_out = placed;
    return json;
}

/*
 * Synthesised entity state. In the real runtime this comes from the state store
 * (§5.2); here it only has to make the components take their different visual
 * branches. Roughly one tile in eight is unavailable, which is the §7.5 dimmed
 * dash case and allocates differently from a normal tile.
 */
static const char *s1_entity_state(const char *entity_id, uint32_t seed)
{
    uint32_t h = seed;
    for (const char *p = entity_id; *p; p++) {
        h = h * 31u + (uint8_t)*p;
    }
    if ((h % 8u) == 0u) {
        return (h % 16u) == 0u ? "unknown" : "unavailable";
    }
    return (h % 2u) ? "on" : "off";
}

static bool s1_state_is_absent(const char *state)
{
    return strcmp(state, "unavailable") == 0 || strcmp(state, "unknown") == 0;
}

/* -------------------------------------------------------------------------
 * Components (design.md §7)
 *
 * Reduced to what a heap measurement can see: the widget mix, the local styles
 * and the animations. No icon font, no real service calls, no touch handling.
 * ------------------------------------------------------------------------- */

static void s1_pulse_anim_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, LV_PART_MAIN);
}

/* §7.5: every action shows a pending state. Left running when the tree is
 * destroyed — that is the point, see the file header. */
static void s1_start_pending_pulse(lv_obj_t *obj)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, s1_pulse_anim_cb);
    lv_anim_set_values(&a, LV_OPA_50, LV_OPA_COVER);
    lv_anim_set_duration(&a, 600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

/* Name label with §7.5 overflow behaviour: marquee rather than clipped. */
static lv_obj_t *s1_name_label(lv_obj_t *parent, const char *text, int32_t width)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_color(label, lv_color_hex(S1_COL_TEXT_LO), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    return label;
}

static void s1_build_light(lv_obj_t *tile, int32_t w, const char *name,
                           const char *state, int size_w, int size_h)
{
    lv_obj_t *icon = lv_label_create(tile);
    lv_label_set_text(icon, s1_state_is_absent(state) ? "-" : "*");
    lv_obj_set_style_text_color(icon,
                                lv_color_hex(strcmp(state, "on") == 0 ? S1_COL_WARN : S1_COL_TEXT_LO),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *label = s1_name_label(tile, name, w - 2 * S1_MARGIN);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* §7.1: the 2×1 and 2×2 variants carry a brightness slider. */
    if (size_w >= 2) {
        lv_obj_t *slider = lv_slider_create(tile);
        lv_obj_set_width(slider, w - 2 * S1_MARGIN);
        lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
        lv_slider_set_value(slider, s1_state_is_absent(state) ? 0 : 62, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider, lv_color_hex(S1_COL_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, lv_color_hex(S1_COL_SURFACE_ALT), LV_PART_MAIN);

        /* §7.1: colour temperature appears only on 2×2 and only when supported. */
        if (size_h >= 2) {
            lv_obj_t *ct = lv_slider_create(tile);
            lv_obj_set_width(ct, w - 2 * S1_MARGIN);
            lv_obj_align(ct, LV_ALIGN_CENTER, 0, 40);
            lv_slider_set_value(ct, 35, LV_ANIM_OFF);
        }
    }
}

static void s1_build_cover(lv_obj_t *tile, int32_t w, const char *name, const char *state)
{
    lv_obj_t *label = s1_name_label(tile, name, w - 2 * S1_MARGIN);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* §7.2: up / stop / down. */
    static const char *glyphs[] = { LV_SYMBOL_UP, LV_SYMBOL_STOP, LV_SYMBOL_DOWN };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_button_create(tile);
        lv_obj_set_size(btn, 48, 48); /* §7.5: touch targets ≥ 48 px */
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, (int32_t)i * 52, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(S1_COL_SURFACE_ALT), LV_PART_MAIN);
        lv_obj_set_style_radius(btn, S1_RADIUS / 2, LV_PART_MAIN);

        lv_obj_t *sym = lv_label_create(btn);
        lv_label_set_text(sym, glyphs[i]);
        lv_obj_center(sym);
    }

    /* §7.2: movement shows an animated indicator until the state settles. */
    if (!s1_state_is_absent(state) && strcmp(state, "on") == 0) {
        s1_start_pending_pulse(tile);
    }
}

static void s1_build_sensor(lv_obj_t *tile, int32_t w, const char *name, const char *state)
{
    lv_obj_t *value = lv_label_create(tile);
    if (s1_state_is_absent(state)) {
        /* §7.5: unavailable renders dimmed with a dash, not an empty tile. */
        lv_label_set_text(value, "—");
        lv_obj_set_style_text_color(value, lv_color_hex(S1_COL_TEXT_LO), LV_PART_MAIN);
    } else {
        /*
         * §7.5 out-of-range: the type scale steps down for longer strings, so
         * "1013.2" must not be laid out at the same size as "21.4". Picking the
         * font by string length is what the real component will do; here it also
         * keeps the measurement honest, because the two branches allocate
         * different glyph work.
         */
        lv_label_set_text(value, (strcmp(state, "on") == 0) ? "1013.2" : "21.4");
        lv_obj_set_style_text_color(value, lv_color_hex(S1_COL_TEXT_HI), LV_PART_MAIN);
    }
    lv_obj_set_style_text_font(value,
                               lv_strlen(lv_label_get_text(value)) > 4
                                   ? &lv_font_montserrat_20
                                   : &lv_font_montserrat_44,
                               LV_PART_MAIN);
    lv_obj_align(value, LV_ALIGN_LEFT_MID, 0, -8);

    lv_obj_t *label = s1_name_label(tile, name, w - 2 * S1_MARGIN);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void s1_build_scene(lv_obj_t *tile, const cJSON *entities, const char *name)
{
    /* §7.4: 1×1 is a single scene, 4×1 is a bar of 2–5. */
    if (!cJSON_IsArray(entities)) {
        lv_obj_t *label = lv_label_create(tile);
        lv_label_set_text(label, name);
        lv_obj_center(label);
        return;
    }

    int n = cJSON_GetArraySize(entities);
    int i = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, entities) {
        lv_obj_t *btn = lv_button_create(tile);
        lv_obj_set_size(btn, 180, 48);
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, (int32_t)i * 190, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(S1_COL_SURFACE_ALT), LV_PART_MAIN);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, cJSON_IsString(item) ? item->valuestring : "scene");
        lv_obj_center(label);
        i++;
    }
    (void)n;
}

static void s1_build_placeholder(lv_obj_t *tile, const char *entity_id, const char *type)
{
    /*
     * §3.1: an unknown component type renders a placeholder, never a crash.
     * §7.5: a missing entity shows a placeholder containing the entity_id so it
     * can be found in the editor.
     */
    lv_obj_t *label = lv_label_create(tile);
    lv_label_set_text_fmt(label, "?%s\n%s", type, entity_id ? entity_id : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_color(label, lv_color_hex(S1_COL_WARN), LV_PART_MAIN);
    lv_obj_center(label);
}

/* -------------------------------------------------------------------------
 * Tree build and destroy — the operation under test (design.md §6.4)
 * ------------------------------------------------------------------------- */

/*
 * Parses the configuration and builds the widget tree under a single root, which
 * is what the destroy step then deletes. Returns NULL if the JSON does not parse
 * — the real runtime answers 400 and leaves the existing UI untouched (§6.4);
 * here it aborts the run, because a spike that silently measured zero tiles
 * would report a perfect pass.
 */
static lv_obj_t *s1_build_tree(const char *config_json, uint32_t seed)
{
    cJSON *root = cJSON_Parse(config_json);
    if (!root) {
        ESP_LOGE(TAG, "config did not parse");
        return NULL;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *container = lv_obj_create(screen);
    lv_obj_set_size(container, S1_SCREEN_W, S1_SCREEN_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, lv_color_hex(S1_COL_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    /* §3.2: the 56 px system bar is fixed and not configurable. */
    lv_obj_t *bar = lv_obj_create(container);
    lv_obj_set_size(bar, S1_SCREEN_W, S1_BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(S1_COL_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *clock = lv_label_create(bar);
    lv_label_set_text(clock, "22:41");
    lv_obj_set_style_text_font(clock, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(clock, lv_color_hex(S1_COL_TEXT_HI), LV_PART_MAIN);
    lv_obj_align(clock, LV_ALIGN_LEFT_MID, 0, 0);

    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    const cJSON *page = cJSON_GetArrayItem(pages, 0);
    const cJSON *tiles = cJSON_GetObjectItemCaseSensitive(page, "tiles");

    const cJSON *tile_json = NULL;
    cJSON_ArrayForEach(tile_json, tiles) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(tile_json, "type");
        const cJSON *pos = cJSON_GetObjectItemCaseSensitive(tile_json, "pos");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(tile_json, "size");
        const cJSON *entity = cJSON_GetObjectItemCaseSensitive(tile_json, "entity");
        const cJSON *entities = cJSON_GetObjectItemCaseSensitive(tile_json, "entities");
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(tile_json, "label");

        const int col = cJSON_GetArrayItem(pos, 0)->valueint;
        const int row = cJSON_GetArrayItem(pos, 1)->valueint;
        const int sw = cJSON_GetArrayItem(size, 0)->valueint;
        const int sh = cJSON_GetArrayItem(size, 1)->valueint;

        const int32_t w = sw * S1_CELL_W + (sw - 1) * S1_GAP;
        const int32_t h = sh * S1_CELL_H + (sh - 1) * S1_GAP;
        const int32_t x = S1_MARGIN + col * (S1_CELL_W + S1_GAP);
        const int32_t y = S1_BAR_H + S1_MARGIN + row * (S1_CELL_H + S1_GAP);

        lv_obj_t *tile = lv_obj_create(container);
        lv_obj_set_size(tile, w, h);
        lv_obj_set_pos(tile, x, y);
        lv_obj_set_style_bg_color(tile, lv_color_hex(S1_COL_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_radius(tile, S1_RADIUS, LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(tile, S1_MARGIN, LV_PART_MAIN);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        const char *entity_id = cJSON_IsString(entity) ? entity->valuestring : NULL;
        const char *state = entity_id ? s1_entity_state(entity_id, seed) : "on";
        const char *name = cJSON_IsString(label) ? label->valuestring
                                                 : (entity_id ? entity_id : "unnamed");

        /* §7.5: an unavailable entity dims the whole tile. */
        if (s1_state_is_absent(state)) {
            lv_obj_set_style_opa(tile, LV_OPA_40, LV_PART_MAIN);
        }

#if S1_LEAK_BYTES
        /* Negative control only — see S1_LEAK_BYTES at the top of this file.
         * Allocated from the LVGL heap and never freed, so it is exactly the
         * class of bug the criterion is meant to catch. */
        (void)lv_malloc(S1_LEAK_BYTES);
#endif

        const char *type_str = cJSON_IsString(type) ? type->valuestring : "";
        if (strcmp(type_str, "light") == 0) {
            s1_build_light(tile, w, name, state, sw, sh);
        } else if (strcmp(type_str, "cover") == 0) {
            s1_build_cover(tile, w, name, state);
        } else if (strcmp(type_str, "sensor") == 0) {
            s1_build_sensor(tile, w, name, state);
        } else if (strcmp(type_str, "scene") == 0) {
            s1_build_scene(tile, entities, name);
        } else {
            s1_build_placeholder(tile, entity_id, type_str);
        }
    }

    cJSON_Delete(root);
    return container;
}

/*
 * Runs LVGL far enough that the tree is actually rendered and the animations
 * started above have ticked at least once. Without this the build step would be
 * measured before the draw-time allocations it triggers, and the animations
 * would be deleted before they ever allocated anything.
 */
static void s1_settle(int ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < deadline) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* -------------------------------------------------------------------------
 * Measurement
 * ------------------------------------------------------------------------- */

/*
 * One CSV line per cycle. Parsed off the serial log by tools/s1_report.py, so
 * the columns are fixed; the header is emitted once so the file is readable on
 * its own.
 *
 * Two things here are not in the §13 brief and are worth the extra columns:
 *
 *   built_* is sampled while the tree is still standing. Without it, a run in
 *   which the build silently did nothing would produce a flawlessly flat series
 *   and be indistinguishable from a pass. The built-versus-after gap is the
 *   evidence that each cycle allocated something to begin with.
 *
 *   int_free and psram_free are the ESP heap. §13's criterion names only the
 *   LVGL heap, but an LVGL-clean rebuild that leaks ESP heap underneath is
 *   still a leak that puts the panel into a reboot loop on the wall.
 */
typedef struct {
    uint32_t free_size;
    uint32_t free_biggest;
    uint32_t used_cnt;
    uint32_t max_used;
    uint8_t  used_pct;
    uint8_t  frag_pct;
} s1_sample_t;

static void s1_sample(s1_sample_t *out)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    out->free_size    = (uint32_t)mon.free_size;
    out->free_biggest = (uint32_t)mon.free_biggest_size;
    out->used_cnt     = (uint32_t)mon.used_cnt;
    out->max_used     = (uint32_t)mon.max_used;
    out->used_pct     = mon.used_pct;
    out->frag_pct     = mon.frag_pct;
}

static void s1_log_row(const char *tag, int cycle, int tiles,
                       const s1_sample_t *built, const s1_sample_t *after)
{
    printf("%s,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
           tag, cycle, tiles,
           (unsigned)built->free_size,
           (unsigned)built->used_cnt,
           (unsigned)after->free_size,
           (unsigned)after->free_biggest,
           (unsigned)after->used_cnt,
           (unsigned)after->max_used,
           (unsigned)after->used_pct,
           (unsigned)after->frag_pct,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

#define S1_CSV_HEADER \
    "tag,cycle,tiles,built_free,built_used_cnt,free,biggest,used_cnt,max_used," \
    "used_pct,frag_pct,int_free,psram_free"

/* One build/destroy cycle — design.md §6.4's PUT /config path. */
static int s1_cycle(int cycle, const char *tag)
{
    int tiles = 0;
    char *config_json = s1_generate_config(cycle, &tiles);

    lv_obj_t *tree = s1_build_tree(config_json, (uint32_t)cycle);
    free(config_json);
    assert(tree != NULL);

    s1_settle(20);

    s1_sample_t built;
    s1_sample(&built);

    lv_obj_delete(tree);

    /* Drain whatever the delete deferred: async deletions, invalidated areas,
     * animations being torn down. Sampling before this drains would flatter the
     * result by counting memory that LVGL is about to release anyway. */
    s1_settle(20);

    s1_sample_t after;
    s1_sample(&after);

    s1_log_row(tag, cycle, tiles, &built, &after);
    return tiles;
}

void app_main(void)
{
    ESP_LOGI(TAG, "S-1 rebuild idempotency — %d cycles after %d warmup",
             S1_CYCLES, S1_WARMUP_CYCLES);
    ESP_LOGI(TAG, "LVGL %d.%d.%d", lv_version_major(), lv_version_minor(), lv_version_patch());

    lv_init();
    lv_tick_set_cb(s1_tick_cb);
    s1_display_init();

    printf("%s\n", S1_CSV_HEADER);

    /* Before anything is built at all — the number the one-time caches are
     * charged to, so the warmup step stays visible rather than hidden. */
    s1_sample_t pre;
    s1_sample(&pre);
    s1_log_row("S1PRE", 0, 0, &pre, &pre);

    for (int i = 0; i < S1_WARMUP_CYCLES; i++) {
        s1_cycle(-(S1_WARMUP_CYCLES - i), "S1WARM");
    }

    for (int i = 1; i <= S1_CYCLES; i++) {
        s1_cycle(i, "S1");
    }

    ESP_LOGI(TAG, "S-1 complete");

    /* Idle rather than return: returning from app_main tears the task down and
     * the log tail can be lost mid-line on the serial monitor. */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
