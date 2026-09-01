/* Slate — provider-neutral declarative UI runtime. */

#include "slate_ui.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include "slate_action.h"
#include "slate_brightness.h"
#include "slate_component.h"
#include "slate_cover.h"
#include "slate_display.h"
#include "slate_light.h"
#include "slate_scene.h"
#include "slate_sensor.h"
#include "slate_state.h"
#include "slate_store.h"
#include "slate_theme.h"
#include "slate_time.h"
#include "slate_wifi.h"

static const char *TAG = "slate_ui";

/* §3.2 is a public layout contract. Keep the arithmetic named rather than
 * deriving it from the theme: changing a theme must not move configured cells. */
#define UI_WIDTH       800
#define UI_HEIGHT      480
#define UI_BAR_HEIGHT   56
#define UI_COLUMNS       4
#define UI_ROWS          3
#define UI_CELL_WIDTH   184
#define UI_CELL_HEIGHT  124
#define UI_GAP           12
#define UI_MARGIN        14

#define UI_REBUILD_POST_TIMEOUT_MS 1000
#define UI_LABEL_LIMIT             64
#define UI_PROVIDER_SUMMARY_MAX    128
#define UI_EDITOR_URL_MAX          32
#define UI_EDITOR_QR_SIZE          220

typedef struct {
    char provider[SLATE_PROVIDER_ID_MAX + 1];
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    slate_kind_t kind;
    bool label_override;
    lv_obj_t *tile;
    lv_obj_t *name;
    lv_obj_t *detail;
    slate_cover_view_t cover;
    slate_light_view_t light;
    slate_scene_view_t scene;
    slate_sensor_view_t sensor;
} binding_view_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *clock;
    lv_obj_t *provider;
    lv_timer_t *bar_timer;
    binding_view_t *views;
    size_t view_count;
    const slate_theme_t *theme;
} ui_tree_t;

typedef struct {
    const slate_config_t *config;
    SemaphoreHandle_t done;
    esp_err_t result;
} rebuild_request_t;

typedef struct {
    const char *title;
    const char *message;
    bool clear_bindings;
    SemaphoreHandle_t done;
    esp_err_t result;
} message_request_t;

static ui_tree_t *s_tree;
static bool s_ready;
static atomic_bool s_update_posted = ATOMIC_VAR_INIT(false);
static atomic_bool s_edit_mode = ATOMIC_VAR_INIT(false);
static atomic_bool s_message_active = ATOMIC_VAR_INIT(false);

static void tree_destroy(ui_tree_t *tree);

static void *ui_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0) {
        return NULL;
    }
    return heap_caps_calloc_prefer(count, size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static slate_kind_t component_kind(slate_component_type_t component)
{
    switch (component) {
    case SLATE_COMPONENT_LIGHT:
        return SLATE_KIND_LIGHT;
    case SLATE_COMPONENT_COVER:
        return SLATE_KIND_COVER;
    case SLATE_COMPONENT_SENSOR:
        return SLATE_KIND_SENSOR;
    case SLATE_COMPONENT_SCENE:
        return SLATE_KIND_SCENE;
    case SLATE_COMPONENT_UNKNOWN:
        break;
    }
    return SLATE_KIND_LIGHT;
}

static const slate_config_page_t *home_page(const slate_config_t *config)
{
    for (size_t i = 0; i < config->page_count; i++) {
        if (strcmp(config->pages[i].id, config->home_page) == 0) {
            return &config->pages[i];
        }
    }
    return NULL; /* Validation makes this unreachable for an accepted model. */
}

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

static unsigned provider_status_rank(slate_provider_status_t status)
{
    switch (status) {
    case SLATE_PROVIDER_ERROR:
        return 5;
    case SLATE_PROVIDER_OFFLINE:
        return 4;
    case SLATE_PROVIDER_CONNECTING:
        return 3;
    case SLATE_PROVIDER_UNCONFIGURED:
        return 2;
    case SLATE_PROVIDER_DEGRADED:
        return 1;
    case SLATE_PROVIDER_ONLINE:
        return 0;
    }
    return 0;
}

static bool provider_status_unavailable(slate_provider_status_t status)
{
    return status != SLATE_PROVIDER_ONLINE && status != SLATE_PROVIDER_DEGRADED;
}

static void append_upper(char *out, size_t size, const char *text)
{
    size_t used = strlen(out);
    while (*text != '\0' && used + 1 < size) {
        char ch = *text++;
        out[used++] = ch >= 'a' && ch <= 'z' ? (char) (ch - 'a' + 'A') : ch;
    }
    out[used] = '\0';
}

static void provider_summary(char *out, size_t size, uint32_t *color,
                             const slate_theme_t *theme)
{
    unsigned worst_rank = 0;
    bool unavailable = false;
    out[0] = '\0';

    slate_state_provider_info_t providers[SLATE_STATE_MAX_PROVIDERS];
    size_t active_count = 0;
    size_t provider_count = slate_state_provider_count();
    for (size_t i = 0; i < provider_count && active_count < SLATE_STATE_MAX_PROVIDERS; i++) {
        slate_state_provider_info_t info;
        if (slate_state_provider_at(i, &info) != ESP_OK || info.resource_count == 0) {
            continue;
        }
        providers[active_count++] = info;
        unavailable = unavailable || provider_status_unavailable(info.status);
    }

    for (size_t i = 0; i < active_count; i++) {
        slate_state_provider_info_t info = providers[i];
        unsigned rank = provider_status_rank(info.status);
        bool include = unavailable ? provider_status_unavailable(info.status)
                                   : info.status == SLATE_PROVIDER_DEGRADED;
        if (!include) {
            continue;
        }
        if (out[0] != '\0') {
            strlcat(out, " + ", size);
        }
        append_upper(out, size, info.id);
        strlcat(out, " ", size);
        append_upper(out, size, slate_provider_status_str(info.status));
        if (rank > worst_rank) {
            worst_rank = rank;
        }
    }

    if (worst_rank == 0) {
        strlcpy(out, "PROVIDERS ONLINE", size);
        *color = theme->accent;
        return;
    }

    *color = worst_rank >= provider_status_rank(SLATE_PROVIDER_CONNECTING)
                 ? theme->warn
                 : theme->text_lo;
}

static void update_bar(ui_tree_t *tree)
{
    if (tree == NULL) {
        return;
    }

    char clock[8] = "--:--";
    if (slate_time_synced()) {
        time_t now = time(NULL);
        struct tm local;
        if (slate_time_localtime(now, &local)) {
            strftime(clock, sizeof(clock), "%H:%M", &local);
        }
    }
    lv_label_set_text(tree->clock, clock);

    char text[UI_PROVIDER_SUMMARY_MAX];
    uint32_t color;
    if (atomic_load_explicit(&s_edit_mode, memory_order_acquire)) {
        strlcpy(text, "EDIT MODE", sizeof(text));
        color = tree->theme->warn;
    } else {
        color = tree->theme->accent;
        provider_summary(text, sizeof(text), &color, tree->theme);
    }
    lv_label_set_text(tree->provider, text);
    lv_obj_set_style_text_color(tree->provider, lv_color_hex(color), LV_PART_MAIN);
}

static void style_presentation(binding_view_t *view, slate_presentation_t presentation,
                               slate_action_phase_t phase, const slate_theme_t *theme)
{
    bool dim = presentation == SLATE_PRESENT_STALE ||
               presentation == SLATE_PRESENT_UNAVAILABLE;
    bool warn = presentation == SLATE_PRESENT_MISSING_PROVIDER ||
                presentation == SLATE_PRESENT_INCOMPATIBLE || phase == SLATE_ACTION_ERROR;

    lv_obj_set_style_opa(view->tile, dim ? LV_OPA_50 : LV_OPA_COVER, LV_PART_MAIN);
    /* An outline, not a border. LVGL counts border width into the content area
     * (lv_obj_get_style_space_left()), so raising a 2 px border on touch moved
     * every child of the tile 2 px down and right — the name visibly jumped
     * when a light was tapped, which read as the panel mis-registering the
     * touch. An outline is drawn outside the tile and owns no layout. */
    lv_obj_set_style_outline_width(view->tile,
                                   warn || phase == SLATE_ACTION_PENDING ? 2 : 0,
                                   LV_PART_MAIN);
    lv_obj_set_style_outline_color(
        view->tile,
        lv_color_hex(warn ? theme->warn : theme->accent),
        LV_PART_MAIN);
}

static void update_sensor_view(binding_view_t *view, const slate_resource_t *resource,
                               const slate_theme_t *theme)
{
    slate_sensor_update(&view->sensor, resource, theme);

    if (!slate_component_is_placeholder(resource->presentation) &&
        !view->label_override) {
        lv_label_set_text(view->name,
                          resource->name[0] != '\0' ? resource->name : view->resource);
    }
}

static void update_view(binding_view_t *view, const slate_resource_t *resource,
                        const slate_theme_t *theme)
{
    slate_action_feedback_t feedback = {0};
    if (slate_action_feedback(view->provider, view->resource, &feedback) != ESP_OK) {
        feedback.phase = SLATE_ACTION_IDLE;
    }

    if (view->kind == SLATE_KIND_SENSOR && view->sensor.value != NULL) {
        update_sensor_view(view, resource, theme);
        style_presentation(view, resource->presentation, feedback.phase, theme);
        return;
    }

    if (view->kind == SLATE_KIND_LIGHT && view->light.icon != NULL) {
        slate_light_update(&view->light, resource, &feedback, theme);
        style_presentation(view, resource->presentation, feedback.phase, theme);
        return;
    }

    if (view->kind == SLATE_KIND_COVER && view->cover.icon != NULL) {
        slate_cover_update(&view->cover, resource, &feedback, theme);
        style_presentation(view, resource->presentation, feedback.phase, theme);
        return;
    }

    if (view->kind == SLATE_KIND_SCENE && view->scene.icon != NULL) {
        slate_scene_update(&view->scene, resource, &feedback, theme);
        style_presentation(view, resource->presentation, feedback.phase, theme);
        return;
    }

    if (!view->label_override && resource->name[0] != '\0') {
        lv_label_set_text(view->name, resource->name);
    }

    char detail[SLATE_PROVIDER_ID_MAX + SLATE_RESOURCE_ID_MAX + 32];
    const char *presentation = slate_presentation_str(resource->presentation);
    const char *phase = feedback.phase == SLATE_ACTION_PENDING
                            ? "pending"
                            : feedback.phase == SLATE_ACTION_ERROR ? "error" : presentation;
    snprintf(detail, sizeof(detail), "%s:%s  ·  %s", view->provider, view->resource, phase);
    lv_label_set_text(view->detail, detail);
    lv_obj_set_style_text_color(
        view->detail,
        lv_color_hex(feedback.phase == SLATE_ACTION_ERROR ? theme->warn : theme->text_lo),
        LV_PART_MAIN);
    style_presentation(view, resource->presentation, feedback.phase, theme);
}

static void update_all(void)
{
    ui_tree_t *tree = s_tree;
    if (tree == NULL) {
        return;
    }

    for (size_t i = 0; i < tree->view_count; i++) {
        binding_view_t *view = &tree->views[i];
        slate_resource_t resource;
        if (slate_state_get(view->provider, view->resource, &resource) == ESP_OK) {
            update_view(view, &resource, tree->theme);
        }
    }
    update_bar(tree);
}

static void discard_changed(const slate_resource_t *resource, void *ctx)
{
    (void) resource;
    (void) ctx;
}

static void update_work(void *ctx)
{
    (void) ctx;
    atomic_store_explicit(&s_update_posted, false, memory_order_release);
    slate_state_drain(discard_changed, NULL);
    update_all();
}

static void schedule_update(void *ctx)
{
    (void) ctx;
    if (!s_ready) {
        return;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&s_update_posted, &expected, true,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        return;
    }
    if (slate_display_post(update_work, NULL, 0) != ESP_OK) {
        /* The one-second bar timer below is also a drain fallback, so a full
         * display queue delays a redraw rather than losing it indefinitely. */
        atomic_store_explicit(&s_update_posted, false, memory_order_release);
    }
}

static void bar_timer_cb(lv_timer_t *timer)
{
    (void) timer;
    slate_state_drain(discard_changed, NULL);
    update_all();
}

static ui_tree_t *tree_base(const slate_theme_t *theme, const char *title)
{
    ui_tree_t *tree = ui_calloc(1, sizeof(*tree));
    if (tree == NULL) {
        return NULL;
    }
    tree->theme = theme;
    tree->screen = lv_obj_create(NULL);
    if (tree->screen == NULL) {
        free(tree);
        return NULL;
    }
    style_plain(tree->screen);
    lv_obj_set_style_bg_color(tree->screen, lv_color_hex(theme->bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tree->screen, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *bar = lv_obj_create(tree->screen);
    if (bar == NULL) {
        tree_destroy(tree);
        return NULL;
    }
    style_plain(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, UI_WIDTH, UI_BAR_HEIGHT);
    lv_obj_set_style_bg_color(bar, lv_color_hex(theme->surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, UI_MARGIN, LV_PART_MAIN);

    tree->clock = make_label(bar, "--:--", theme->body, theme->text_hi);
    if (tree->clock == NULL) {
        tree_destroy(tree);
        return NULL;
    }
    lv_obj_align(tree->clock, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *page_title = make_label(bar, title != NULL ? title : "Slate",
                                      theme->body, theme->text_hi);
    if (page_title == NULL) {
        tree_destroy(tree);
        return NULL;
    }
    lv_obj_set_width(page_title, 320);
    slate_component_label_one_line(page_title);
    lv_obj_align(page_title, LV_ALIGN_CENTER, 0, 0);

    tree->provider = make_label(bar, "PROVIDERS DEGRADED", theme->caption, theme->text_lo);
    if (tree->provider == NULL) {
        tree_destroy(tree);
        return NULL;
    }
    lv_obj_set_width(tree->provider, 220);
    slate_component_label_one_line(tree->provider);
    lv_obj_set_style_text_align(tree->provider, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(tree->provider, LV_ALIGN_RIGHT_MID, 0, 0);

    tree->bar_timer = lv_timer_create(bar_timer_cb, 1000, NULL);
    if (tree->bar_timer == NULL) {
        lv_obj_delete(tree->screen);
        free(tree);
        return NULL;
    }
    update_bar(tree);
    return tree;
}

static void tree_destroy(ui_tree_t *tree)
{
    if (tree == NULL) {
        return;
    }
    if (tree->bar_timer != NULL) {
        lv_timer_delete(tree->bar_timer);
        tree->bar_timer = NULL;
    }
    if (tree->screen != NULL) {
        lv_obj_delete(tree->screen);
    }
    free(tree->views);
    free(tree);
}

static lv_obj_t *build_tile_shell(ui_tree_t *tree, const slate_config_tile_t *tile)
{
    const slate_theme_t *theme = tree->theme;
    int32_t width = tile->width * UI_CELL_WIDTH + (tile->width - 1) * UI_GAP;
    int32_t height = tile->height * UI_CELL_HEIGHT + (tile->height - 1) * UI_GAP;
    int32_t x = UI_MARGIN + tile->column * (UI_CELL_WIDTH + UI_GAP);
    int32_t y = UI_BAR_HEIGHT + UI_MARGIN + tile->row * (UI_CELL_HEIGHT + UI_GAP);

    lv_obj_t *object = lv_obj_create(tree->screen);
    if (object == NULL) {
        return NULL;
    }
    style_plain(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(theme->surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(object, theme->radius, LV_PART_MAIN);
    lv_obj_set_style_pad_all(object, theme->pad, LV_PART_MAIN);
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}

static bool build_unknown_tile(ui_tree_t *tree, const slate_config_tile_t *tile,
                               lv_obj_t *object)
{
    char text[UI_LABEL_LIMIT * 2];
    snprintf(text, sizeof(text), "Unknown component\n%s", tile->type);
    lv_obj_t *label = make_label(object, text, tree->theme->caption, tree->theme->warn);
    if (label == NULL) {
        return false;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_center(label);
    return true;
}

static bool build_sensor_tile(ui_tree_t *tree, const slate_config_tile_t *tile,
                              lv_obj_t *object, size_t *view_index)
{
    const slate_theme_t *theme = tree->theme;
    const slate_config_binding_t *binding = &tile->bindings[0];
    binding_view_t *view = &tree->views[(*view_index)++];

    if (strlcpy(view->provider, binding->provider, sizeof(view->provider)) >=
            sizeof(view->provider) ||
        strlcpy(view->resource, binding->resource, sizeof(view->resource)) >=
            sizeof(view->resource)) {
        return false;
    }
    view->kind = SLATE_KIND_SENSOR;
    view->label_override = tile->label != NULL;
    view->tile = object;
    if (!slate_sensor_build(object, tile, theme, &view->sensor)) {
        return false;
    }
    view->name = view->sensor.name;
    return true;
}

static bool build_light_tile(ui_tree_t *tree, const slate_config_tile_t *tile,
                             lv_obj_t *object, size_t *view_index)
{
    const slate_config_binding_t *binding = &tile->bindings[0];
    binding_view_t *view = &tree->views[(*view_index)++];

    if (strlcpy(view->provider, binding->provider, sizeof(view->provider)) >=
            sizeof(view->provider) ||
        strlcpy(view->resource, binding->resource, sizeof(view->resource)) >=
            sizeof(view->resource)) {
        return false;
    }
    view->kind = SLATE_KIND_LIGHT;
    view->label_override = tile->label != NULL;
    view->tile = object;
    if (!slate_light_build(object, tile, tree->theme, &view->light,
                           view->provider, view->resource)) {
        return false;
    }
    view->name = view->light.name;
    return true;
}

static bool build_cover_tile(ui_tree_t *tree, const slate_config_tile_t *tile,
                             lv_obj_t *object, size_t *view_index)
{
    const slate_config_binding_t *binding = &tile->bindings[0];
    binding_view_t *view = &tree->views[(*view_index)++];

    if (strlcpy(view->provider, binding->provider, sizeof(view->provider)) >=
            sizeof(view->provider) ||
        strlcpy(view->resource, binding->resource, sizeof(view->resource)) >=
            sizeof(view->resource)) {
        return false;
    }
    view->kind = SLATE_KIND_COVER;
    view->label_override = tile->label != NULL;
    view->tile = object;
    if (!slate_cover_build(object, tile, tree->theme, &view->cover,
                           view->provider, view->resource)) {
        return false;
    }
    view->name = view->cover.name;
    return true;
}

static bool build_scene_tile(ui_tree_t *tree, const slate_config_tile_t *tile,
                             lv_obj_t *object, size_t *view_index)
{
    for (size_t i = 0; i < tile->binding_count; i++) {
        const slate_config_binding_t *binding = &tile->bindings[i];
        binding_view_t *view = &tree->views[(*view_index)++];
        if (strlcpy(view->provider, binding->provider, sizeof(view->provider)) >=
                sizeof(view->provider) ||
            strlcpy(view->resource, binding->resource, sizeof(view->resource)) >=
                sizeof(view->resource)) {
            return false;
        }
        view->kind = SLATE_KIND_SCENE;
        view->label_override = tile->label != NULL && tile->binding_count == 1;
        if (!slate_scene_build(object, tile, tree->theme, &view->scene, i,
                               view->provider, view->resource)) {
            return false;
        }
        view->tile = view->scene.button;
        view->name = view->scene.name;
    }
    return true;
}

static bool build_known_tile(ui_tree_t *tree, const slate_config_tile_t *tile,
                             lv_obj_t *object, size_t *view_index)
{
    const slate_theme_t *theme = tree->theme;
    const slate_config_binding_t *first = &tile->bindings[0];
    const char *name = tile->label != NULL ? tile->label : tile->type;
    lv_obj_t *name_label = make_label(object, name, theme->body, theme->text_hi);
    if (name_label == NULL) {
        return false;
    }
    lv_obj_set_width(name_label, lv_pct(100));
    slate_component_label_one_line(name_label);
    lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 0, 0);

    char identity[SLATE_PROVIDER_ID_MAX + SLATE_RESOURCE_ID_MAX + 16];
    snprintf(identity, sizeof(identity), "%s:%s  ·  missing", first->provider, first->resource);
    lv_obj_t *detail = make_label(object, identity, theme->caption, theme->text_lo);
    if (detail == NULL) {
        return false;
    }
    lv_obj_set_width(detail, lv_pct(100));
    slate_component_label_one_line(detail);
    lv_obj_align(detail, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Every binding enters the state store. Generic multi-binding components
     * share this shell until they gain their own semantic presentation. */
    for (size_t i = 0; i < tile->binding_count; i++) {
        binding_view_t *view = &tree->views[(*view_index)++];
        if (strlcpy(view->provider, tile->bindings[i].provider, sizeof(view->provider)) >=
                sizeof(view->provider) ||
            strlcpy(view->resource, tile->bindings[i].resource, sizeof(view->resource)) >=
                sizeof(view->resource)) {
            return false;
        }
        view->kind = component_kind(tile->component);
        view->label_override = tile->label != NULL;
        view->tile = object;
        view->name = name_label;
        view->detail = detail;
    }
    return true;
}

static ui_tree_t *build_config_tree(const slate_config_t *config)
{
    const slate_config_page_t *page = home_page(config);
    const slate_theme_t *theme = slate_theme_find(config->theme);
    if (page == NULL || theme == NULL) {
        return NULL;
    }

    size_t view_count = 0;
    for (size_t i = 0; i < page->tile_count; i++) {
        if (page->tiles[i].component != SLATE_COMPONENT_UNKNOWN) {
            view_count += page->tiles[i].binding_count;
        }
    }

    ui_tree_t *tree = tree_base(theme, page->title != NULL ? page->title : page->id);
    if (tree == NULL) {
        return NULL;
    }
    if (view_count > 0) {
        tree->views = ui_calloc(view_count, sizeof(*tree->views));
        if (tree->views == NULL) {
            tree_destroy(tree);
            return NULL;
        }
    }
    tree->view_count = view_count;

    size_t view_index = 0;
    for (size_t i = 0; i < page->tile_count; i++) {
        const slate_config_tile_t *tile = &page->tiles[i];
        lv_obj_t *object = build_tile_shell(tree, tile);
        if (object == NULL) {
            tree_destroy(tree);
            return NULL;
        }
#if defined(SLATE_UI_SELFTEST) && defined(SLATE_UI_LEAK_BYTES) && SLATE_UI_LEAK_BYTES > 0
        /* Instrument negative control only. It is deliberately unreachable in
         * ordinary builds and must turn the 500-cycle verifier red. */
        (void) lv_malloc(SLATE_UI_LEAK_BYTES);
#endif
        if (tile->component == SLATE_COMPONENT_UNKNOWN) {
            if (!build_unknown_tile(tree, tile, object)) {
                tree_destroy(tree);
                return NULL;
            }
        } else if (tile->component == SLATE_COMPONENT_LIGHT) {
            if (!build_light_tile(tree, tile, object, &view_index)) {
                tree_destroy(tree);
                return NULL;
            }
        } else if (tile->component == SLATE_COMPONENT_SENSOR) {
            if (!build_sensor_tile(tree, tile, object, &view_index)) {
                tree_destroy(tree);
                return NULL;
            }
        } else if (tile->component == SLATE_COMPONENT_COVER) {
            if (!build_cover_tile(tree, tile, object, &view_index)) {
                tree_destroy(tree);
                return NULL;
            }
        } else if (tile->component == SLATE_COMPONENT_SCENE) {
            if (!build_scene_tile(tree, tile, object, &view_index)) {
                tree_destroy(tree);
                return NULL;
            }
        } else if (!build_known_tile(tree, tile, object, &view_index)) {
            tree_destroy(tree);
            return NULL;
        }
    }
    return tree;
}

static slate_binding_t *config_bindings(const slate_config_t *config, size_t *out_count)
{
    size_t count = 0;
    for (size_t p = 0; p < config->page_count; p++) {
        const slate_config_page_t *page = &config->pages[p];
        for (size_t i = 0; i < page->tile_count; i++) {
            if (page->tiles[i].component != SLATE_COMPONENT_UNKNOWN) {
                count += page->tiles[i].binding_count;
            }
        }
    }
    *out_count = count;
    if (count == 0) {
        return NULL;
    }

    slate_binding_t *bindings = ui_calloc(count, sizeof(*bindings));
    if (bindings == NULL) {
        return NULL;
    }
    size_t used = 0;
    for (size_t p = 0; p < config->page_count; p++) {
        const slate_config_page_t *page = &config->pages[p];
        for (size_t i = 0; i < page->tile_count; i++) {
            const slate_config_tile_t *tile = &page->tiles[i];
            if (tile->component == SLATE_COMPONENT_UNKNOWN) {
                continue;
            }
            for (size_t b = 0; b < tile->binding_count; b++) {
                bindings[used++] = (slate_binding_t) {
                    .provider = tile->bindings[b].provider,
                    .resource = tile->bindings[b].resource,
                    .kind = component_kind(tile->component),
                };
            }
        }
    }
    return bindings;
}

static void activate_tree(ui_tree_t *fresh)
{
    ui_tree_t *old = s_tree;
    lv_obj_t *previous_screen = lv_screen_active();
    lv_screen_load(fresh->screen);
    s_tree = fresh;
    update_all();

    if (old != NULL) {
        tree_destroy(old);
    } else if (previous_screen != NULL && previous_screen != fresh->screen) {
        /* The first activation retires slate_display's physical bring-up tree.
         * Its delete callback clears the diagnostic timer and object handles. */
        lv_obj_delete(previous_screen);
    }
}

static esp_err_t rebuild_on_task(const slate_config_t *config)
{
    size_t binding_count = 0;
    slate_binding_t *bindings = config_bindings(config, &binding_count);
    if (binding_count > 0 && bindings == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ui_tree_t *fresh = build_config_tree(config);
    if (fresh == NULL) {
        free(bindings);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = slate_state_bind(bindings, binding_count);
    free(bindings);
    if (err != ESP_OK) {
        tree_destroy(fresh);
        return err;
    }

    const char *timezone = config->settings.timezone != NULL
                               ? config->settings.timezone
                               : SLATE_TIME_DEFAULT_ZONE;
    esp_err_t timezone_err = slate_time_set_timezone(timezone);
    if (timezone_err != ESP_OK) {
        ESP_LOGW(TAG, "timezone %s unavailable: %s — keeping the previous timezone",
                 timezone, esp_err_to_name(timezone_err));
    }
    esp_err_t brightness_err = slate_brightness_configure(&config->settings);
    if (brightness_err != ESP_OK) {
        /* A headless or otherwise degraded display must not make a valid
         * dashboard configuration impossible to activate through the API. */
        ESP_LOGW(TAG, "brightness settings unavailable: %s",
                 esp_err_to_name(brightness_err));
    }
    activate_tree(fresh);
    atomic_store_explicit(&s_message_active, false, memory_order_release);
    slate_state_drain(discard_changed, NULL);
    update_all();
    ESP_LOGI(TAG, "activated page \"%s\": %u tile(s), %u binding(s)",
             home_page(config)->id, (unsigned) home_page(config)->tile_count,
             (unsigned) binding_count);
    return ESP_OK;
}

static void rebuild_work(void *ctx)
{
    rebuild_request_t *request = ctx;
    request->result = rebuild_on_task(request->config);
    xSemaphoreGive(request->done);
}

esp_err_t slate_ui_rebuild(const slate_config_t *config)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    rebuild_request_t request = {
        .config = config,
        .done = done,
        .result = ESP_ERR_INVALID_STATE,
    };
    esp_err_t err = slate_display_post(rebuild_work, &request, UI_REBUILD_POST_TIMEOUT_MS);
    if (err == ESP_OK) {
        /* Once queued, the borrowed config and stack request must remain alive.
         * The display task is permanent, so waiting is safer than timing out
         * into a use-after-return while work is still queued. */
        xSemaphoreTake(done, portMAX_DELAY);
        err = request.result;
    }
    vSemaphoreDelete(done);
    return err;
}

static bool editor_url(char *out, size_t out_len, char *address, size_t address_len)
{
    slate_wifi_status_t status;
    slate_wifi_status(&status);
    if (!status.connected || status.ip[0] == '\0') {
        return false;
    }

    int written = snprintf(out, out_len, "http://%s/", status.ip);
    if (written < 0 || (size_t) written >= out_len) {
        return false;
    }
    strlcpy(address, status.ip, address_len);
    return true;
}

static ui_tree_t *build_message_tree(const char *title, const char *message)
{
    const slate_theme_t *theme = slate_theme_default();
    ui_tree_t *tree = tree_base(theme, title);
    if (tree == NULL) {
        return NULL;
    }

    lv_obj_t *card = lv_obj_create(tree->screen);
    if (card == NULL) {
        tree_destroy(tree);
        return NULL;
    }
    style_plain(card);
    char url[UI_EDITOR_URL_MAX] = {0};
    char address[16] = {0};
    bool can_open = editor_url(url, sizeof(url), address, sizeof(address));

    lv_obj_set_size(card, can_open ? 720 : 650, can_open ? 330 : 250);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(theme->surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, theme->radius, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 30, LV_PART_MAIN);

    int32_t text_x = can_open ? UI_EDITOR_QR_SIZE + 36 : 0;
    int32_t text_width = can_open ? 404 : 590;
    lv_obj_t *heading = make_label(card, title, theme->body, theme->warn);
    if (heading == NULL) {
        tree_destroy(tree);
        return NULL;
    }
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, text_x, 0);
    lv_obj_t *body = make_label(card, message, theme->caption, theme->text_lo);
    if (body == NULL) {
        tree_destroy(tree);
        return NULL;
    }
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, text_width);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, text_x, can_open ? 104 : 55);

    if (can_open) {
        lv_obj_t *qr = lv_qrcode_create(card);
        if (qr == NULL) {
            explicit_bzero(url, sizeof(url));
            tree_destroy(tree);
            return NULL;
        }
        lv_qrcode_set_size(qr, UI_EDITOR_QR_SIZE);
        /* Machine-readable contrast is not a theme token. In particular, the
         * default dark theme's text/surface pair would invert the symbol, which
         * some camera decoders do not support. */
        lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
        lv_qrcode_set_quiet_zone(qr, true);
        if (lv_qrcode_update(qr, url, strlen(url)) != LV_RESULT_OK) {
            explicit_bzero(url, sizeof(url));
            tree_destroy(tree);
            return NULL;
        }
        lv_obj_align(qr, LV_ALIGN_LEFT_MID, 0, 0);

        char open[64];
        snprintf(open, sizeof(open), "Open http://%s", address);
        lv_obj_t *address_label = make_label(card, open, theme->body, theme->text_hi);
        lv_obj_t *hint = make_label(card, "Scan to open the editor", theme->caption,
                                    theme->accent);
        if (address_label == NULL || hint == NULL) {
            explicit_bzero(url, sizeof(url));
            tree_destroy(tree);
            return NULL;
        }
        lv_label_set_long_mode(address_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(address_label, text_width);
        lv_obj_align(address_label, LV_ALIGN_TOP_LEFT, text_x, 44);
        lv_obj_align(hint, LV_ALIGN_TOP_LEFT, text_x, 76);
    }
    explicit_bzero(url, sizeof(url));
    return tree;
}

static void message_work(void *ctx)
{
    message_request_t *request = ctx;
    ui_tree_t *fresh = build_message_tree(request->title, request->message);
    if (fresh == NULL) {
        request->result = ESP_ERR_NO_MEM;
    } else {
        request->result = request->clear_bindings ? slate_state_bind(NULL, 0) : ESP_OK;
        if (request->result == ESP_OK) {
            activate_tree(fresh);
            atomic_store_explicit(&s_message_active, true, memory_order_release);
        } else {
            tree_destroy(fresh);
        }
    }
    xSemaphoreGive(request->done);
}

static esp_err_t show_message(const char *title, const char *message, bool clear_bindings)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    message_request_t request = {
        .title = title,
        .message = message,
        .clear_bindings = clear_bindings,
        .done = done,
        .result = ESP_ERR_INVALID_STATE,
    };
    esp_err_t err = slate_display_post(message_work, &request, UI_REBUILD_POST_TIMEOUT_MS);
    if (err == ESP_OK) {
        xSemaphoreTake(done, portMAX_DELAY);
        err = request.result;
    }
    vSemaphoreDelete(done);
    return err;
}

static bool report_has_code(const slate_config_report_t *report, const char *code)
{
    for (size_t i = 0; i < report->error_count; i++) {
        if (strcmp(report->errors[i].code, code) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t restore_stored(slate_ui_config_info_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }

    char *json = NULL;
    size_t len = 0;
    esp_err_t read_err = slate_store_config_read(&json, &len);
    if (read_err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "no stored dashboard configuration");
        return show_message("NO DASHBOARD",
                            "Open the editor, create a dashboard, and publish it to this panel.",
                            true);
    }
    if (read_err != ESP_OK) {
        ESP_LOGE(TAG, "stored configuration unreadable: %s", esp_err_to_name(read_err));
        return show_message("CONFIGURATION ERROR",
                            "The stored dashboard could not be read. The device API remains available.",
                            true);
    }

    slate_config_t *config = NULL;
    slate_config_report_t report;
    slate_config_parse_status_t status = slate_config_parse(json, len, &config, &report);
    free(json);
    if (status != SLATE_CONFIG_PARSE_OK) {
        bool future = status == SLATE_CONFIG_PARSE_INVALID_CONFIG &&
                      report_has_code(&report, "schema_too_new");
        ESP_LOGE(TAG, "stored configuration rejected: %d%s", (int) status,
                 future ? " (newer schema)" : "");
        slate_config_report_free(&report);
        return show_message(future ? "FIRMWARE UPDATE REQUIRED" : "CONFIGURATION ERROR",
                            future ? "Update Slate firmware, then reopen the editor."
                                   : "Open the editor to repair or replace the stored dashboard.",
                            true);
    }
    slate_config_report_free(&report);

    unsigned schema = (unsigned) config->schema;
    size_t tiles = home_page(config)->tile_count;
    esp_err_t err = slate_ui_rebuild(config);
    slate_config_free(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stored dashboard activation failed: %s", esp_err_to_name(err));
        return show_message("DASHBOARD ERROR",
                            "The dashboard could not be built. The device API remains available.",
                            true);
    }
    if (out != NULL) {
        out->configured = true;
        out->schema = schema;
        out->tiles = tiles;
    }
    return ESP_OK;
}

esp_err_t slate_ui_init(void)
{
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!slate_display_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ready = true;
    slate_state_set_wake(schedule_update, NULL);
    slate_action_set_wake(schedule_update, NULL);
    return restore_stored(NULL);
}

esp_err_t slate_ui_restore_stored(slate_ui_config_info_t *out)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return restore_stored(out);
}

bool slate_ui_ready(void)
{
    return s_ready;
}

void slate_ui_mode_set(bool edit)
{
    atomic_store_explicit(&s_edit_mode, edit, memory_order_release);
    slate_component_actions_mode_set(edit);
    if (s_ready) {
        schedule_update(NULL);
    }
}

void slate_ui_mode_restore_complete(void)
{
    slate_component_actions_restore_complete();
}

void slate_ui_network_connected(void)
{
    if (!s_ready || !atomic_load_explicit(&s_message_active, memory_order_acquire)) {
        return;
    }
    esp_err_t err = restore_stored(NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "refreshing editor address: %s", esp_err_to_name(err));
    }
}

#ifdef SLATE_UI_SELFTEST

#define UI_TEST_WARMUP 10
#define UI_TEST_CYCLES 500

typedef struct {
    size_t subscriptions;
    unsigned calls;
    unsigned action_calls;
    uint32_t action_id;
    slate_action_t action;
    slate_action_value_type_t value_type;
    int32_t value;
    char action_resource[SLATE_RESOURCE_ID_MAX + 1];
} fixture_provider_t;

typedef struct {
    uint32_t free_size;
    uint32_t used_count;
    uint32_t internal_free;
    uint32_t psram_free;
    uint8_t frag_pct;
} heap_sample_t;

typedef struct {
    SemaphoreHandle_t done;
    esp_err_t result;
} selftest_request_t;

typedef struct {
    unsigned count;
    double sum_x;
    double sum_y;
    double sum_xx;
    double sum_xy;
    uint32_t first;
    uint32_t last;
    uint32_t min;
    uint32_t max;
    uint32_t used_first;
    uint32_t used_last;
    uint32_t used_min;
    uint32_t used_max;
    uint32_t tree_min;
    uint32_t tree_max;
    uint8_t frag_min;
    uint8_t frag_max;
    uint32_t int_first;
    uint32_t int_last;
    uint32_t psram_first;
    uint32_t psram_last;
} heap_series_t;

static fixture_provider_t s_fixture;

static esp_err_t fixture_subscribe(void *ctx, const char *const *resources, size_t count)
{
    (void) resources;
    fixture_provider_t *fixture = ctx;
    fixture->subscriptions = count;
    fixture->calls++;
    return ESP_OK;
}

static esp_err_t fixture_dispatch(void *ctx, uint32_t id,
                                  const slate_action_request_t *request)
{
    fixture_provider_t *fixture = ctx;
    fixture->action_calls++;
    fixture->action_id = id;
    fixture->action = request->action;
    fixture->value_type = request->value_type;
    fixture->value = request->value_type == SLATE_ACTION_VALUE_NUMBER
                         ? request->value.number : 0;
    strlcpy(fixture->action_resource, request->resource,
            sizeof(fixture->action_resource));
    return ESP_OK;
}

static void sample_heap(heap_sample_t *out)
{
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    *out = (heap_sample_t) {
        .free_size = (uint32_t) monitor.free_size,
        .used_count = (uint32_t) monitor.used_cnt,
        .internal_free = (uint32_t) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .psram_free = (uint32_t) heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        .frag_pct = monitor.frag_pct,
    };
}

static void series_add(heap_series_t *series, unsigned cycle,
                       const heap_sample_t *built, const heap_sample_t *after)
{
    uint32_t cost = after->free_size > built->free_size
                        ? after->free_size - built->free_size : 0;
    if (series->count == 0) {
        series->first = after->free_size;
        series->min = after->free_size;
        series->max = after->free_size;
        series->used_first = after->used_count;
        series->used_min = after->used_count;
        series->used_max = after->used_count;
        series->tree_min = cost;
        series->tree_max = cost;
        series->frag_min = after->frag_pct;
        series->frag_max = after->frag_pct;
        series->int_first = after->internal_free;
        series->psram_first = after->psram_free;
    }
    series->count++;
    series->last = after->free_size;
    series->used_last = after->used_count;
    if (after->free_size < series->min) {
        series->min = after->free_size;
    }
    if (after->free_size > series->max) {
        series->max = after->free_size;
    }
    if (after->used_count < series->used_min) {
        series->used_min = after->used_count;
    }
    if (after->used_count > series->used_max) {
        series->used_max = after->used_count;
    }
    if (cost < series->tree_min) {
        series->tree_min = cost;
    }
    if (cost > series->tree_max) {
        series->tree_max = cost;
    }
    if (after->frag_pct < series->frag_min) {
        series->frag_min = after->frag_pct;
    }
    if (after->frag_pct > series->frag_max) {
        series->frag_max = after->frag_pct;
    }
    series->int_last = after->internal_free;
    series->psram_last = after->psram_free;

    double x = (double) cycle;
    double y = (double) after->free_size;
    series->sum_x += x;
    series->sum_y += y;
    series->sum_xx += x * x;
    series->sum_xy += x * y;
}

static double series_slope(const heap_series_t *series)
{
    double count = (double) series->count;
    double denominator = count * series->sum_xx - series->sum_x * series->sum_x;
    return denominator == 0.0
               ? 0.0
               : (count * series->sum_xy - series->sum_x * series->sum_y) / denominator;
}

static char *test_config_json(int cycle)
{
    /* Three valid tilings exercise every allowed size. The page title and one
     * label vary with the cycle, so the allocator cannot merely replay one
     * identical allocation pattern 500 times. All documents mix the real
     * direct provider with the deterministic fixture. */
    static const char *layouts[] = {
        "{\"id\":\"l\",\"type\":\"light\",\"pos\":[0,0],\"size\":[2,2],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"living-room\"}},"
        "{\"id\":\"s\",\"type\":\"sensor\",\"pos\":[2,0],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"temperature\"}},"
        "{\"id\":\"c\",\"type\":\"cover\",\"pos\":[2,1],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"blind\"}},"
        "{\"id\":\"u\",\"type\":\"thermostat\",\"pos\":[3,1],\"size\":[1,1]},"
        "{\"id\":\"sc\",\"type\":\"scene\",\"pos\":[0,2],\"size\":[4,1],"
        "\"bindings\":[{\"provider\":\"direct\",\"resource\":\"relax\"},"
        "{\"provider\":\"ui-fixture\",\"resource\":\"away\"}]}",

        "{\"id\":\"c\",\"type\":\"cover\",\"pos\":[0,0],\"size\":[1,2],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"blind\"}},"
        "{\"id\":\"l\",\"type\":\"light\",\"pos\":[1,0],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"living-room\"}},"
        "{\"id\":\"s1\",\"type\":\"sensor\",\"pos\":[3,0],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"temperature\"}},"
        "{\"id\":\"s2\",\"type\":\"sensor\",\"pos\":[1,1],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"pressure\"}},"
        "{\"id\":\"u\",\"type\":\"future\",\"pos\":[3,1],\"size\":[1,1]},"
        "{\"id\":\"sc\",\"type\":\"scene\",\"pos\":[0,2],\"size\":[4,1],"
        "\"bindings\":[{\"provider\":\"direct\",\"resource\":\"relax\"},"
        "{\"provider\":\"ui-fixture\",\"resource\":\"away\"}]}",

        "{\"id\":\"a\",\"type\":\"light\",\"pos\":[0,0],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"living-room\"}},"
        "{\"id\":\"b\",\"type\":\"sensor\",\"pos\":[1,0],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"temperature\"}},"
        "{\"id\":\"c\",\"type\":\"sensor\",\"pos\":[2,0],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"pressure\"}},"
        "{\"id\":\"d\",\"type\":\"unknown\",\"pos\":[3,0],\"size\":[1,1]},"
        "{\"id\":\"e\",\"type\":\"light\",\"pos\":[0,1],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"kitchen\"}},"
        "{\"id\":\"f\",\"type\":\"sensor\",\"pos\":[1,1],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"power\"}},"
        "{\"id\":\"g\",\"type\":\"cover\",\"pos\":[2,1],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"blind\"}},"
        "{\"id\":\"h\",\"type\":\"sensor\",\"pos\":[3,1],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"humidity\"}},"
        "{\"id\":\"i\",\"type\":\"scene\",\"pos\":[0,2],\"size\":[1,1],"
        "\"bindings\":[{\"provider\":\"direct\",\"resource\":\"relax\"}]},"
        "{\"id\":\"j\",\"type\":\"light\",\"pos\":[1,2],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"desk\"}},"
        "{\"id\":\"k\",\"type\":\"sensor\",\"pos\":[2,2],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"humidity\"}},"
        "{\"id\":\"m\",\"type\":\"future\",\"pos\":[3,2],\"size\":[1,1]}",
    };

    const char *layout = layouts[(unsigned) cycle % (sizeof(layouts) / sizeof(layouts[0]))];
    size_t capacity = strlen(layout) + 512;
    char *json = malloc(capacity);
    if (json == NULL) {
        return NULL;
    }
    const char *theme = cycle % 2 == 0 ? "midnight" : "minimal-light";
    int written = snprintf(json, capacity,
                           "{\"schema\":1,\"theme\":\"%s\","
                           "\"home_page\":\"home\",\"pages\":[{\"id\":\"home\","
                           "\"title\":\"Runtime cycle %d\",\"tiles\":[%s]},"
                           "{\"id\":\"secondary\",\"title\":\"Secondary\",\"tiles\":["
                           "{\"id\":\"offscreen\",\"type\":\"sensor\","
                           "\"pos\":[0,0],\"size\":[1,1],\"binding\":{"
                           "\"provider\":\"ui-fixture\",\"resource\":\"offscreen\"}}]}]}",
                           theme, cycle, layout);
    if (written < 0 || (size_t) written >= capacity) {
        free(json);
        return NULL;
    }
    return json;
}

static slate_config_t *test_config(int cycle)
{
    char *json = test_config_json(cycle);
    if (json == NULL) {
        return NULL;
    }
    slate_config_t *config = NULL;
    slate_config_report_t report;
    slate_config_parse_status_t status = slate_config_parse(json, strlen(json), &config, &report);
    free(json);
    slate_config_report_free(&report);
    return status == SLATE_CONFIG_PARSE_OK ? config : NULL;
}

static slate_config_t *sensor_test_config(void)
{
    static const char JSON[] =
        "{\"schema\":1,\"theme\":\"minimal-light\",\"home_page\":\"sensors\",\"pages\":[{"
        "\"id\":\"sensors\",\"title\":\"Sensor component #23\",\"tiles\":["
        "{\"id\":\"temperature\",\"type\":\"sensor\",\"pos\":[0,0],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"temperature\"}},"
        "{\"id\":\"humidity\",\"type\":\"sensor\",\"pos\":[1,0],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"humidity\"}},"
        "{\"id\":\"pressure\",\"type\":\"sensor\",\"pos\":[2,0],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"pressure\"}},"
        "{\"id\":\"power\",\"type\":\"sensor\",\"label\":\"Solar output\","
        "\"pos\":[0,1],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"power\"}},"
        "{\"id\":\"status\",\"type\":\"sensor\",\"icon\":\"fire\","
        "\"pos\":[2,1],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"status\"}},"
        "{\"id\":\"wrong\",\"type\":\"sensor\",\"pos\":[0,2],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"wrong-sensor\"}},"
        "{\"id\":\"future\",\"type\":\"sensor\",\"pos\":[1,2],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"future\",\"resource\":\"outside\"}}]}]}";

    slate_config_t *config = NULL;
    slate_config_report_t report;
    slate_config_parse_status_t status =
        slate_config_parse(JSON, sizeof(JSON) - 1, &config, &report);
    slate_config_report_free(&report);
    return status == SLATE_CONFIG_PARSE_OK ? config : NULL;
}

static slate_config_t *sensor_icon_test_config(void)
{
    static const char JSON[] =
        "{\"schema\":1,\"theme\":\"minimal-light\",\"home_page\":\"icons\",\"pages\":[{"
        "\"id\":\"icons\",\"tiles\":["
        "{\"id\":\"temperature\",\"type\":\"sensor\",\"pos\":[0,0],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"temperature\"}},"
        "{\"id\":\"humidity\",\"type\":\"sensor\",\"pos\":[2,0],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"humidity\"}},"
        "{\"id\":\"pressure\",\"type\":\"sensor\",\"pos\":[0,1],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"pressure\"}},"
        "{\"id\":\"power\",\"type\":\"sensor\",\"pos\":[2,1],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"power\"}},"
        "{\"id\":\"status\",\"type\":\"sensor\",\"pos\":[0,2],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"status\"}}]}]}";

    slate_config_t *config = NULL;
    slate_config_report_t report;
    slate_config_parse_status_t status =
        slate_config_parse(JSON, sizeof(JSON) - 1, &config, &report);
    slate_config_report_free(&report);
    return status == SLATE_CONFIG_PARSE_OK ? config : NULL;
}

static slate_config_t *light_test_config(void)
{
    static const char JSON[] =
        "{\"schema\":1,\"theme\":\"minimal-light\",\"home_page\":\"lights\",\"pages\":[{"
        "\"id\":\"lights\",\"title\":\"Light component #22\",\"tiles\":["
        /* The compact tile carries a name far too long for a 1x1 cell on
         * purpose: it is the fixture for the truncation check below. */
        "{\"id\":\"compact\",\"type\":\"light\","
        "\"label\":\"Upstairs landing reading lamp by the window\","
        "\"pos\":[0,0],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"living-room\"}},"
        "{\"id\":\"wide\",\"type\":\"light\",\"pos\":[1,0],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"kitchen\"}},"
        "{\"id\":\"readonly\",\"type\":\"light\",\"pos\":[3,0],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"readonly\"}},"
        "{\"id\":\"large\",\"type\":\"light\",\"label\":\"Mood light\","
        "\"icon\":\"floor-lamp\",\"pos\":[0,1],\"size\":[2,2],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"studio\"}},"
        "{\"id\":\"toggle-only\",\"type\":\"light\",\"pos\":[2,1],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"switch-only\"}},"
        "{\"id\":\"missing\",\"type\":\"light\",\"pos\":[2,2],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"missing-light\"}}]}]}";

    slate_config_t *config = NULL;
    slate_config_report_t report;
    slate_config_parse_status_t status =
        slate_config_parse(JSON, sizeof(JSON) - 1, &config, &report);
    slate_config_report_free(&report);
    return status == SLATE_CONFIG_PARSE_OK ? config : NULL;
}

static slate_config_t *light_layout_test_config(void)
{
    static const char JSON[] =
        "{\"schema\":1,\"theme\":\"minimal-light\",\"home_page\":\"layouts\",\"pages\":[{"
        "\"id\":\"layouts\",\"title\":\"Light layout fallbacks #22\",\"tiles\":["
        "{\"id\":\"vertical\",\"type\":\"light\",\"pos\":[0,0],\"size\":[1,2],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"living-room\"}},"
        "{\"id\":\"panoramic\",\"type\":\"light\",\"pos\":[0,2],\"size\":[4,1],"
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"kitchen\"}}]}]}";

    slate_config_t *config = NULL;
    slate_config_report_t report;
    slate_config_parse_status_t status =
        slate_config_parse(JSON, sizeof(JSON) - 1, &config, &report);
    slate_config_report_free(&report);
    return status == SLATE_CONFIG_PARSE_OK ? config : NULL;
}

static slate_config_t *light_action_test_config(void)
{
    static const char JSON[] =
        "{\"schema\":1,\"theme\":\"minimal-light\",\"home_page\":\"actions\",\"pages\":[{"
        "\"id\":\"actions\",\"title\":\"Light actions #22\",\"tiles\":["
        "{\"id\":\"toggle\",\"type\":\"light\",\"pos\":[0,0],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"action-toggle\"}},"
        "{\"id\":\"dimmer\",\"type\":\"light\",\"pos\":[1,0],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"action-dimmer\"}},"
        "{\"id\":\"temperature\",\"type\":\"light\",\"pos\":[0,1],\"size\":[2,2],"
        "\"binding\":{\"provider\":\"ui-fixture\","
        "\"resource\":\"action-temperature\"}}]}]}";

    slate_config_t *config = NULL;
    slate_config_report_t report;
    slate_config_parse_status_t status =
        slate_config_parse(JSON, sizeof(JSON) - 1, &config, &report);
    slate_config_report_free(&report);
    return status == SLATE_CONFIG_PARSE_OK ? config : NULL;
}

static slate_config_t *cover_test_config(void)
{
    static const char JSON[] =
        "{\"schema\":1,\"theme\":\"minimal-light\",\"home_page\":\"covers\",\"pages\":[{"
        "\"id\":\"covers\",\"title\":\"Cover component #25\",\"tiles\":["
        "{\"id\":\"compact\",\"type\":\"cover\",\"pos\":[0,0],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"cover-compact\"}},"
        "{\"id\":\"horizontal\",\"type\":\"cover\",\"pos\":[1,0],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"cover-horizontal\"}},"
        "{\"id\":\"missing\",\"type\":\"cover\",\"pos\":[3,0],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"cover-missing\"}},"
        "{\"id\":\"vertical\",\"type\":\"cover\",\"label\":\"Bedroom curtain\","
        "\"icon\":\"curtains\",\"pos\":[0,1],\"size\":[1,2],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"cover-vertical\"}},"
        "{\"id\":\"unavailable\",\"type\":\"cover\",\"pos\":[1,1],\"size\":[2,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"cover-unavailable\"}},"
        "{\"id\":\"wrong\",\"type\":\"cover\",\"pos\":[3,1],\"size\":[1,1],"
        "\"binding\":{\"provider\":\"ui-fixture\",\"resource\":\"cover-wrong\"}}]}]}";

    slate_config_t *config = NULL;
    slate_config_report_t report;
    slate_config_parse_status_t status =
        slate_config_parse(JSON, sizeof(JSON) - 1, &config, &report);
    slate_config_report_free(&report);
    return status == SLATE_CONFIG_PARSE_OK ? config : NULL;
}

static slate_config_t *scene_test_config(void)
{
    static const char JSON[] =
        "{\"schema\":1,\"theme\":\"minimal-light\",\"home_page\":\"scenes\",\"pages\":[{"
        "\"id\":\"scenes\",\"title\":\"Scene component #26\",\"tiles\":["
        "{\"id\":\"bar\",\"type\":\"scene\",\"pos\":[0,0],\"size\":[4,1],"
        "\"bindings\":["
        "{\"provider\":\"ui-fixture\",\"resource\":\"scene-movie\"},"
        "{\"provider\":\"ui-fixture\",\"resource\":\"scene-relax\"},"
        "{\"provider\":\"ui-fixture\",\"resource\":\"scene-dinner\"},"
        "{\"provider\":\"ui-fixture\",\"resource\":\"scene-away\"},"
        "{\"provider\":\"ui-fixture\",\"resource\":\"scene-missing\"}]},"
        "{\"id\":\"compact\",\"type\":\"scene\",\"label\":\"Good night\","
        "\"icon\":\"sleep\",\"pos\":[0,1],\"size\":[1,1],"
        "\"bindings\":[{\"provider\":\"ui-fixture\","
        "\"resource\":\"scene-goodnight\"}]}]}]}";

    slate_config_t *config = NULL;
    slate_config_report_t report;
    slate_config_parse_status_t status =
        slate_config_parse(JSON, sizeof(JSON) - 1, &config, &report);
    slate_config_report_free(&report);
    return status == SLATE_CONFIG_PARSE_OK ? config : NULL;
}

static void settle_frame(void)
{
    /* The real RGB flush completes from VSYNC. Yielding the owner task lets the
     * ISR release the previous frame before forcing this tree's draw-time
     * allocations, which is the part a build-only heap sample would miss. */
    vTaskDelay(pdMS_TO_TICKS(30));
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
}

static lv_obj_t *activate_selftest_anchor(void)
{
    lv_obj_t *anchor = lv_obj_create(NULL);
    if (anchor == NULL) {
        return NULL;
    }
    style_plain(anchor);
    lv_obj_set_style_bg_color(anchor, lv_color_hex(slate_theme_default()->bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(anchor, LV_OPA_COVER, LV_PART_MAIN);

    ui_tree_t *old = s_tree;
    lv_obj_t *previous = lv_screen_active();
    lv_screen_load(anchor);
    s_tree = NULL;
    if (old != NULL) {
        tree_destroy(old);
    } else if (previous != NULL && previous != anchor) {
        lv_obj_delete(previous);
    }
    settle_frame();
    return anchor;
}

static esp_err_t measured_cycle(int cycle, lv_obj_t *anchor,
                                heap_sample_t *built, heap_sample_t *after)
{
    slate_config_t *config = test_config(cycle);
    if (config == NULL) {
        return ESP_FAIL;
    }

    size_t binding_count = 0;
    slate_binding_t *bindings = config_bindings(config, &binding_count);
    if (binding_count > 0 && bindings == NULL) {
        slate_config_free(config);
        return ESP_ERR_NO_MEM;
    }
    ui_tree_t *fresh = build_config_tree(config);
    if (fresh == NULL) {
        free(bindings);
        slate_config_free(config);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = slate_state_bind(bindings, binding_count);
    free(bindings);
    slate_config_free(config);
    if (err != ESP_OK) {
        tree_destroy(fresh);
        return err;
    }

    lv_screen_load(fresh->screen);
    s_tree = fresh;
    slate_state_drain(discard_changed, NULL);
    update_all();
    settle_frame();
    sample_heap(built);

    err = slate_state_bind(NULL, 0);
    if (err == ESP_OK) {
        lv_screen_load(anchor);
        s_tree = NULL;
        tree_destroy(fresh);
        settle_frame();
        sample_heap(after);
    } else {
        lv_screen_load(anchor);
        s_tree = NULL;
        tree_destroy(fresh);
    }
    return err;
}

static bool verify_layout(const slate_config_t *config)
{
    const slate_config_page_t *page = home_page(config);
    if (page == NULL || s_tree == NULL) {
        return false;
    }
    lv_obj_update_layout(s_tree->screen);
    /* Child zero is the fixed system bar; tiles follow in configuration order. */
    for (size_t i = 0; i < page->tile_count; i++) {
        const slate_config_tile_t *tile = &page->tiles[i];
        lv_obj_t *object = lv_obj_get_child(s_tree->screen, (int32_t) i + 1);
        int32_t want_x = UI_MARGIN + tile->column * (UI_CELL_WIDTH + UI_GAP);
        int32_t want_y = UI_BAR_HEIGHT + UI_MARGIN + tile->row * (UI_CELL_HEIGHT + UI_GAP);
        int32_t want_w = tile->width * UI_CELL_WIDTH + (tile->width - 1) * UI_GAP;
        int32_t want_h = tile->height * UI_CELL_HEIGHT + (tile->height - 1) * UI_GAP;
        if (object == NULL || lv_obj_get_x(object) != want_x || lv_obj_get_y(object) != want_y ||
            lv_obj_get_width(object) != want_w || lv_obj_get_height(object) != want_h) {
            return false;
        }
    }
    return true;
}

static binding_view_t *find_view(const char *provider, const char *resource)
{
    if (s_tree == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < s_tree->view_count; i++) {
        if (strcmp(s_tree->views[i].provider, provider) == 0 &&
            strcmp(s_tree->views[i].resource, resource) == 0) {
            return &s_tree->views[i];
        }
    }
    return NULL;
}

static esp_err_t publish_test_states(void)
{
    const slate_snapshot_t direct = {
        .resource = "living-room",
        .kind = SLATE_KIND_LIGHT,
        .name = "Living room",
        .available = true,
        .capabilities = {.actions = 1u << SLATE_ACTION_TOGGLE},
        .state.light = {.on = true,
                        .brightness = SLATE_STATE_ABSENT,
                        .color_temperature = SLATE_STATE_ABSENT},
    };
    const slate_snapshot_t fixture = {
        .resource = "temperature",
        .kind = SLATE_KIND_SENSOR,
        .name = "Fixture temperature",
        .available = true,
        .state.sensor = {.numeric = true,
                         .value = 21.4,
                         .unit = "°C",
                         .measurement = SLATE_MEASUREMENT_TEMPERATURE},
    };
    esp_err_t err = slate_state_publish("direct", &direct);
    if (err == ESP_OK) {
        err = slate_state_publish("ui-fixture", &fixture);
    }
    if (err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    return err;
}

static esp_err_t publish_sensor_test_states(void)
{
    static const slate_snapshot_t SENSORS[] = {
        {
            .resource = "temperature",
            .kind = SLATE_KIND_SENSOR,
            .name = "Outdoor temperature",
            .available = true,
            .state.sensor = {.numeric = true, .value = -12.4, .unit = "°C",
                             .measurement = SLATE_MEASUREMENT_TEMPERATURE},
        },
        {
            .resource = "humidity",
            .kind = SLATE_KIND_SENSOR,
            .name = "Bathroom humidity",
            .available = true,
            .state.sensor = {.numeric = true, .value = 58.6, .unit = "%",
                             .measurement = SLATE_MEASUREMENT_HUMIDITY},
        },
        {
            .resource = "pressure",
            .kind = SLATE_KIND_SENSOR,
            .name = "Air pressure",
            .available = true,
            .state.sensor = {.numeric = true, .value = 1013.25, .unit = "hPa",
                             .measurement = SLATE_MEASUREMENT_PRESSURE},
        },
        {
            .resource = "power",
            .kind = SLATE_KIND_SENSOR,
            .name = "House power",
            .available = true,
            .state.sensor = {.numeric = true, .value = 42.75, .unit = "W",
                             .measurement = SLATE_MEASUREMENT_POWER},
        },
        {
            .resource = "status",
            .kind = SLATE_KIND_SENSOR,
            .name = "Air quality",
            .available = true,
            .state.sensor = {.numeric = false, .text = "Air quality needs attention",
                             .measurement = SLATE_MEASUREMENT_NONE},
        },
    };

    for (size_t i = 0; i < sizeof(SENSORS) / sizeof(SENSORS[0]); i++) {
        esp_err_t err = slate_state_publish("direct", &SENSORS[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    const slate_snapshot_t wrong = {
        .resource = "wrong-sensor",
        .kind = SLATE_KIND_LIGHT,
        .available = true,
        .state.light = {.brightness = SLATE_STATE_ABSENT,
                        .color_temperature = SLATE_STATE_ABSENT},
    };
    slate_resource_t bound;
    if (slate_state_get("direct", "wrong-sensor", &bound) == ESP_OK) {
        if (slate_state_publish("direct", &wrong) != ESP_ERR_INVALID_STATE) {
            return ESP_FAIL;
        }
    }
    slate_state_drain(discard_changed, NULL);
    update_all();
    return ESP_OK;
}

static esp_err_t publish_light_test_states(void)
{
    static const slate_snapshot_t LIGHTS[] = {
        {
            .resource = "living-room",
            .kind = SLATE_KIND_LIGHT,
            .name = "Living room",
            .available = true,
            .capabilities = {.actions = 1u << SLATE_ACTION_TOGGLE},
            .state.light = {.on = true, .brightness = SLATE_STATE_ABSENT,
                            .color_temperature = SLATE_STATE_ABSENT},
        },
        {
            .resource = "kitchen",
            .kind = SLATE_KIND_LIGHT,
            .name = "Kitchen pendants with a long name",
            .available = true,
            .capabilities = {
                .actions = (1u << SLATE_ACTION_TOGGLE) |
                           (1u << SLATE_ACTION_SET_BRIGHTNESS),
                .brightness_min = 5,
                .brightness_max = 95,
            },
            .state.light = {.on = true, .brightness = 62,
                            .color_temperature = SLATE_STATE_ABSENT},
        },
        {
            .resource = "readonly",
            .kind = SLATE_KIND_LIGHT,
            .name = "Read-only light",
            .available = true,
            .state.light = {.on = false, .brightness = SLATE_STATE_ABSENT,
                            .color_temperature = SLATE_STATE_ABSENT},
        },
        {
            .resource = "studio",
            .kind = SLATE_KIND_LIGHT,
            .name = "Provider name must not replace the label",
            .available = true,
            .capabilities = {
                .actions = (1u << SLATE_ACTION_TOGGLE) |
                           (1u << SLATE_ACTION_SET_BRIGHTNESS) |
                           (1u << SLATE_ACTION_SET_COLOR_TEMPERATURE),
                .brightness_min = 0,
                .brightness_max = 100,
                .color_temperature_min = 2200,
                .color_temperature_max = 6500,
            },
            .state.light = {.on = true, .brightness = 48, .color_temperature = 3200},
        },
        {
            .resource = "switch-only",
            .kind = SLATE_KIND_LIGHT,
            .name = "Switch only",
            .available = true,
            .capabilities = {.actions = 1u << SLATE_ACTION_TOGGLE},
            .state.light = {.on = false, .brightness = SLATE_STATE_ABSENT,
                            .color_temperature = SLATE_STATE_ABSENT},
        },
    };

    for (size_t i = 0; i < sizeof(LIGHTS) / sizeof(LIGHTS[0]); i++) {
        esp_err_t err = slate_state_publish("direct", &LIGHTS[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    slate_state_drain(discard_changed, NULL);
    update_all();
    return ESP_OK;
}

static esp_err_t publish_light_layout_test_states(void)
{
    const slate_snapshot_t vertical = {
        .resource = "living-room",
        .kind = SLATE_KIND_LIGHT,
        .name = "Living room",
        .available = true,
        .capabilities = {.actions = 1u << SLATE_ACTION_TOGGLE},
        .state.light = {.on = true, .brightness = SLATE_STATE_ABSENT,
                        .color_temperature = SLATE_STATE_ABSENT},
    };
    const slate_snapshot_t panoramic = {
        .resource = "kitchen",
        .kind = SLATE_KIND_LIGHT,
        .name = "Kitchen",
        .available = true,
        .capabilities = {
            .actions = 1u << SLATE_ACTION_SET_BRIGHTNESS,
            .brightness_min = 0,
            .brightness_max = 100,
        },
        .state.light = {.on = true, .brightness = 62,
                        .color_temperature = SLATE_STATE_ABSENT},
    };
    esp_err_t err = slate_state_publish("direct", &vertical);
    if (err == ESP_OK) {
        err = slate_state_publish("direct", &panoramic);
    }
    if (err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    return err;
}

static esp_err_t publish_action_test_states(bool toggled, int16_t brightness,
                                            int16_t color_temperature)
{
    const slate_snapshot_t toggle = {
        .resource = "action-toggle",
        .kind = SLATE_KIND_LIGHT,
        .name = "Action toggle",
        .available = true,
        .capabilities = {.actions = 1u << SLATE_ACTION_TOGGLE},
        .state.light = {.on = toggled, .brightness = SLATE_STATE_ABSENT,
                        .color_temperature = SLATE_STATE_ABSENT},
    };
    const slate_snapshot_t dimmer = {
        .resource = "action-dimmer",
        .kind = SLATE_KIND_LIGHT,
        .name = "Action dimmer",
        .available = true,
        .capabilities = {
            .actions = 1u << SLATE_ACTION_SET_BRIGHTNESS,
            .brightness_min = 0,
            .brightness_max = 100,
        },
        .state.light = {.on = true, .brightness = brightness,
                        .color_temperature = SLATE_STATE_ABSENT},
    };
    const slate_snapshot_t temperature = {
        .resource = "action-temperature",
        .kind = SLATE_KIND_LIGHT,
        .name = "Action temperature",
        .available = true,
        .capabilities = {
            .actions = 1u << SLATE_ACTION_SET_COLOR_TEMPERATURE,
        },
        .state.light = {.on = true, .brightness = SLATE_STATE_ABSENT,
                        .color_temperature = color_temperature},
    };
    esp_err_t err = slate_state_publish("ui-fixture", &toggle);
    if (err == ESP_OK) {
        err = slate_state_publish("ui-fixture", &dimmer);
    }
    if (err == ESP_OK) {
        err = slate_state_publish("ui-fixture", &temperature);
    }
    if (err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    return err;
}

static slate_capabilities_t cover_test_capabilities(void)
{
    return (slate_capabilities_t) {
        .actions = (1u << SLATE_ACTION_TOGGLE) | (1u << SLATE_ACTION_OPEN) |
                   (1u << SLATE_ACTION_STOP) | (1u << SLATE_ACTION_CLOSE),
    };
}

static esp_err_t publish_cover_state(const char *resource, int16_t position,
                                     slate_cover_motion_t motion, bool available)
{
    const slate_snapshot_t snapshot = {
        .resource = resource,
        .kind = SLATE_KIND_COVER,
        .name = "Provider cover name",
        .available = available,
        .capabilities = cover_test_capabilities(),
        .state.cover = {.position = position, .motion = motion},
    };
    esp_err_t err = slate_state_publish("ui-fixture", &snapshot);
    if (err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    return err;
}

static esp_err_t publish_cover_test_states(void)
{
    esp_err_t err = publish_cover_state("cover-compact", 0, SLATE_COVER_IDLE, true);
    if (err == ESP_OK) {
        err = publish_cover_state("cover-horizontal", 43, SLATE_COVER_OPENING, true);
    }
    if (err == ESP_OK) {
        err = publish_cover_state("cover-vertical", 100, SLATE_COVER_IDLE, true);
    }
    if (err == ESP_OK) {
        err = publish_cover_state("cover-unavailable", 62, SLATE_COVER_IDLE, false);
    }
    if (err == ESP_OK) {
        const slate_snapshot_t wrong = {
            .resource = "cover-wrong",
            .kind = SLATE_KIND_SENSOR,
            .available = true,
            .state.sensor = {.numeric = false, .text = "wrong kind"},
        };
        err = slate_state_publish("ui-fixture", &wrong);
        if (err == ESP_ERR_INVALID_STATE) {
            slate_state_drain(discard_changed, NULL);
            update_all();
            err = ESP_OK;
        }
    }
    return err;
}

static esp_err_t publish_scene_test_states(void)
{
    static const slate_snapshot_t SCENES[] = {
        {.resource = "scene-movie", .kind = SLATE_KIND_SCENE, .name = "Movie",
         .available = true, .capabilities.actions = 1u << SLATE_ACTION_ACTIVATE},
        {.resource = "scene-relax", .kind = SLATE_KIND_SCENE, .name = "Relax",
         .available = true, .capabilities.actions = 1u << SLATE_ACTION_ACTIVATE},
        {.resource = "scene-dinner", .kind = SLATE_KIND_SCENE, .name = "Dinner",
         .available = true, .capabilities.actions = 1u << SLATE_ACTION_ACTIVATE},
        {.resource = "scene-away", .kind = SLATE_KIND_SCENE, .name = "Away",
         .available = true, .capabilities.actions = 1u << SLATE_ACTION_ACTIVATE},
        {.resource = "scene-missing", .kind = SLATE_KIND_SCENE, .name = "Work",
         .available = true, .capabilities.actions = 1u << SLATE_ACTION_ACTIVATE},
        {.resource = "scene-goodnight", .kind = SLATE_KIND_SCENE,
         .name = "Provider name ignored", .available = true,
         .capabilities.actions = 1u << SLATE_ACTION_ACTIVATE},
    };

    for (size_t i = 0; i < sizeof(SCENES) / sizeof(SCENES[0]); i++) {
        esp_err_t err = slate_state_publish("ui-fixture", &SCENES[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    slate_state_drain(discard_changed, NULL);
    update_all();
    return ESP_OK;
}

static bool sensor_view_text(const char *resource, const char *value, const char *unit)
{
    binding_view_t *view = find_view("direct", resource);
    return view != NULL && view->kind == SLATE_KIND_SENSOR && view->sensor.value != NULL &&
           view->sensor.unit != NULL &&
           strcmp(lv_label_get_text(view->sensor.value), value) == 0 &&
           strcmp(lv_label_get_text(view->sensor.unit), unit) == 0;
}

static esp_err_t selftest_on_task(void)
{
    unsigned checks = 0;
    unsigned failures = 0;
#define UI_CHECK(condition, description)                                      \
    do {                                                                       \
        bool passed_ = (condition);                                            \
        checks++;                                                              \
        failures += !passed_;                                                  \
        ESP_LOGI(TAG, "selftest: %-48s %s", description, passed_ ? "PASS" : "FAIL"); \
    } while (0)

    const slate_theme_t *midnight = slate_theme_find("midnight");
    const slate_theme_t *minimal_light = slate_theme_find("minimal-light");
    UI_CHECK(slate_theme_count() == 2 && slate_theme_at(0) == midnight &&
                 slate_theme_at(1) == minimal_light &&
                 slate_theme_default() == midnight,
             "both themes registered with Midnight default");
    UI_CHECK(midnight != NULL && minimal_light != NULL &&
                 midnight->radius == minimal_light->radius &&
                 midnight->gap == minimal_light->gap &&
                 midnight->pad == minimal_light->pad &&
                 midnight->hero == minimal_light->hero &&
                 midnight->body == minimal_light->body &&
                 midnight->caption == minimal_light->caption &&
                 midnight->icons == minimal_light->icons &&
                 midnight->icons_large == minimal_light->icons_large,
             "theme switch preserves geometry and typography");
    UI_CHECK(midnight != NULL && midnight->bg == 0x101114 &&
                 midnight->surface == 0x1A1C21 &&
                 midnight->surface_alt == 0x22252B &&
                 midnight->text_hi == 0xF2F5F9 && midnight->text_lo == 0x8A94A6 &&
                 midnight->accent == 0x6C8CFF && midnight->on_accent == 0x101114 &&
                 midnight->warn == 0xF5A524 && minimal_light != NULL &&
                 minimal_light->bg == 0xD8D3C9 &&
                 minimal_light->surface == 0xF7F5F0 &&
                 minimal_light->surface_alt == 0xC9C3B8 &&
                 minimal_light->text_hi == 0x1F2933 &&
                 minimal_light->text_lo == 0x4A5661 &&
                 minimal_light->accent == 0x2F6671 &&
                 minimal_light->on_accent == 0xFFFFFF &&
                 minimal_light->warn == 0x9A442F,
             "both themes supply exact complete colour palettes");

    memset(&s_fixture, 0, sizeof(s_fixture));
    const slate_state_provider_t registration = {
        .id = "ui-fixture",
        .subscribe = fixture_subscribe,
        .ctx = &s_fixture,
    };
    esp_err_t register_err = slate_state_provider_register(&registration);
    UI_CHECK(register_err == ESP_OK || register_err == ESP_ERR_INVALID_STATE,
             "fixture provider registered");
    UI_CHECK(slate_state_provider_set_status("ui-fixture", SLATE_PROVIDER_ONLINE) == ESP_OK,
             "fixture provider online");
    const slate_action_provider_t action_registration = {
        .id = "ui-fixture",
        .dispatch = fixture_dispatch,
        .ctx = &s_fixture,
    };
    esp_err_t action_register_err = slate_action_provider_register(&action_registration);
    UI_CHECK(action_register_err == ESP_OK || action_register_err == ESP_ERR_INVALID_STATE,
             "fixture action adapter registered");

    lv_obj_t *anchor = activate_selftest_anchor();
    if (anchor == NULL) {
        UI_CHECK(false, "persistent heap baseline activated");
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *padded_label = make_label(anchor, "Short label", midnight->body,
                                        midnight->text_hi);
    if (padded_label != NULL) {
        lv_obj_set_width(padded_label, 180);
        lv_obj_set_style_pad_top(padded_label, 3, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(padded_label, 5, LV_PART_MAIN);
        slate_component_label_one_line(padded_label);
        lv_obj_update_layout(anchor);
    }
    UI_CHECK(padded_label != NULL &&
                 strcmp(lv_label_get_text(padded_label), "Short label") == 0 &&
                 lv_obj_get_height(padded_label) ==
                     lv_font_get_line_height(midnight->body) +
                         lv_obj_get_style_space_top(padded_label, LV_PART_MAIN) +
                         lv_obj_get_style_space_bottom(padded_label, LV_PART_MAIN),
             "one-line labels retain a full content row with padding");
    if (padded_label != NULL) {
        lv_obj_delete(padded_label);
    }

    for (int i = -UI_TEST_WARMUP; i < 0; i++) {
        heap_sample_t built;
        heap_sample_t after;
        if (measured_cycle(i, anchor, &built, &after) != ESP_OK) {
            UI_CHECK(false, "warm-up rebuild completed");
            return ESP_FAIL;
        }
    }

    heap_sample_t endpoint_start;
    sample_heap(&endpoint_start);

    heap_series_t series = {0};
    for (int i = 1; i <= UI_TEST_CYCLES; i++) {
        heap_sample_t built;
        heap_sample_t after;
        if (measured_cycle(i, anchor, &built, &after) != ESP_OK) {
            UI_CHECK(false, "measured rebuild completed");
            return ESP_FAIL;
        }
        series_add(&series, (unsigned) i, &built, &after);
    }

    heap_sample_t endpoint_end;
    sample_heap(&endpoint_end);

    int64_t delta = (int64_t) series.last - series.first;
    double delta_pct = 100.0 * (double) delta / (double) series.first;
    double slope = series_slope(&series);
    ESP_LOGI(TAG,
             "selftest: %u cycles, LVGL free %" PRIu32 " -> %" PRIu32
             " B (%+.4f%%), range %" PRIu32 "..%" PRIu32 " B",
             series.count, series.first, series.last, delta_pct, series.min, series.max);
    ESP_LOGI(TAG,
             "selftest: slope %+.4f B/cycle, tree cost %" PRIu32 "..%" PRIu32
             " B, fragmentation %u..%u%%",
             slope, series.tree_min, series.tree_max, series.frag_min, series.frag_max);
    ESP_LOGI(TAG,
             "selftest: live allocations %" PRIu32 " -> %" PRIu32
             " (range %" PRIu32 "..%" PRIu32 ")",
             series.used_first, series.used_last, series.used_min, series.used_max);
    ESP_LOGI(TAG,
             "selftest: ESP heaps internal %" PRIu32 " -> %" PRIu32
             " B, PSRAM %" PRIu32 " -> %" PRIu32 " B",
             series.int_first, series.int_last, series.psram_first, series.psram_last);
    ESP_LOGI(TAG,
             "selftest: persistent LVGL baseline free %" PRIu32 " -> %" PRIu32
             " B, live %" PRIu32 " -> %" PRIu32 ", fragmentation %u -> %u%%",
             endpoint_start.free_size, endpoint_end.free_size,
             endpoint_start.used_count, endpoint_end.used_count,
             endpoint_start.frag_pct, endpoint_end.frag_pct);

    UI_CHECK(series.count == UI_TEST_CYCLES, "500 varied rebuilds measured");
    UI_CHECK(series.tree_max > 0, "standing trees allocated LVGL memory");
    UI_CHECK(endpoint_end.free_size == endpoint_start.free_size,
             "LVGL free heap returned exactly to its baseline");
    UI_CHECK(slope >= -0.5, "LVGL free heap has no downward trend");
    UI_CHECK(endpoint_end.used_count == endpoint_start.used_count,
             "LVGL live allocation count returned to its baseline");
    UI_CHECK(endpoint_end.frag_pct == endpoint_start.frag_pct,
             "LVGL fragmentation returned to its baseline");
    slate_config_t *final = test_config(0);
    UI_CHECK(final != NULL && rebuild_on_task(final) == ESP_OK,
             "final mixed-provider tree activated");
    UI_CHECK(final != NULL && verify_layout(final),
             "all tiles match the exact grid coordinates");
    if (final != NULL) {
        slate_config_free(final);
    }
    UI_CHECK(s_fixture.calls > 0 && s_fixture.subscriptions >= 3,
             "fixture received subscriptions from every page");
    UI_CHECK(publish_test_states() == ESP_OK,
             "direct and fixture normalized states published");
    binding_view_t *direct_view = find_view("direct", "living-room");
    binding_view_t *fixture_view = find_view("ui-fixture", "temperature");
    UI_CHECK(direct_view != NULL && direct_view->light.icon != NULL &&
                 strcmp(lv_label_get_text(direct_view->name), "Living room") == 0 &&
                 strcmp(lv_label_get_text(direct_view->light.icon),
                        SLATE_ICON_LIGHTBULB_ON) == 0,
             "the direct-provider tile observed its state");
    UI_CHECK(fixture_view != NULL &&
                 strcmp(lv_label_get_text(fixture_view->name), "Fixture temperature") == 0 &&
                 strcmp(lv_label_get_text(fixture_view->sensor.value), "21.4") == 0 &&
                 strcmp(lv_label_get_text(fixture_view->sensor.unit), "°C") == 0,
             "the fixture tile observed the same normalized path");

    ui_tree_t *mixed_tree = s_tree;
    slate_provider_status_t direct_status_before =
        slate_state_provider_status("direct");
    esp_err_t mixed_offline_err =
        slate_state_provider_set_status("direct", SLATE_PROVIDER_OFFLINE);
    if (mixed_offline_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(mixed_offline_err == ESP_OK && s_tree == mixed_tree && direct_view != NULL &&
                 lv_obj_get_style_opa(direct_view->tile, LV_PART_MAIN) == LV_OPA_50 &&
                 strcmp(lv_label_get_text(direct_view->light.icon), "-") == 0 &&
                 fixture_view != NULL &&
                 lv_obj_get_style_opa(fixture_view->tile, LV_PART_MAIN) == LV_OPA_COVER &&
                 strcmp(lv_label_get_text(fixture_view->sensor.value), "21.4") == 0 &&
                 strcmp(lv_label_get_text(s_tree->provider), "DIRECT OFFLINE") == 0,
             "one provider offline leaves its peer fresh without rebuilding");

    esp_err_t connecting_err =
        slate_state_provider_set_status("direct", SLATE_PROVIDER_CONNECTING);
    if (connecting_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(connecting_err == ESP_OK && s_tree == mixed_tree &&
                 strcmp(lv_label_get_text(s_tree->provider), "DIRECT CONNECTING") == 0 &&
                 lv_color_eq(lv_obj_get_style_text_color(s_tree->provider, LV_PART_MAIN),
                             lv_color_hex(s_tree->theme->warn)),
             "a connecting provider keeps the warning treatment");

    esp_err_t fixture_offline_err =
        slate_state_provider_set_status("ui-fixture", SLATE_PROVIDER_OFFLINE);
    if (fixture_offline_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(fixture_offline_err == ESP_OK && s_tree == mixed_tree &&
                 direct_view != NULL &&
                 lv_obj_get_style_opa(direct_view->tile, LV_PART_MAIN) == LV_OPA_50 &&
                 fixture_view != NULL &&
                 lv_obj_get_style_opa(fixture_view->tile, LV_PART_MAIN) == LV_OPA_50 &&
                 strcmp(lv_label_get_text(s_tree->provider),
                        "DIRECT CONNECTING + UI-FIXTURE OFFLINE") == 0,
             "the bar names simultaneous unavailable providers");

    esp_err_t direct_degraded_err =
        slate_state_provider_set_status("direct", SLATE_PROVIDER_DEGRADED);
    if (direct_degraded_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(direct_degraded_err == ESP_OK && s_tree == mixed_tree &&
                 direct_view != NULL &&
                 lv_obj_get_style_opa(direct_view->tile, LV_PART_MAIN) == LV_OPA_COVER &&
                 fixture_view != NULL &&
                 lv_obj_get_style_opa(fixture_view->tile, LV_PART_MAIN) == LV_OPA_50 &&
                 strcmp(lv_label_get_text(s_tree->provider), "UI-FIXTURE OFFLINE") == 0,
             "a degraded provider stays fresh without crowding an outage warning");
    slate_state_provider_set_status("direct", direct_status_before);
    slate_state_provider_set_status("ui-fixture", SLATE_PROVIDER_ONLINE);
    slate_state_drain(discard_changed, NULL);
    update_all();

    slate_config_t *sensors = sensor_test_config();
    UI_CHECK(sensors != NULL && rebuild_on_task(sensors) == ESP_OK,
             "sensor component test dashboard activated");
    binding_view_t *temperature_view = find_view("direct", "temperature");
    UI_CHECK(s_tree != NULL && s_tree->theme == minimal_light &&
                 lv_color_eq(lv_obj_get_style_bg_color(s_tree->screen, LV_PART_MAIN),
                             lv_color_hex(minimal_light->bg)) &&
                 temperature_view != NULL &&
                 lv_color_eq(lv_obj_get_style_bg_color(temperature_view->tile, LV_PART_MAIN),
                             lv_color_hex(minimal_light->surface)) &&
                 verify_layout(sensors),
             "Minimal Light changes palette without moving the grid");
    if (sensors != NULL) {
        slate_config_free(sensors);
    }

    binding_view_t *power = find_view("direct", "power");
    binding_view_t *status = find_view("direct", "status");
    binding_view_t *wrong_sensor = find_view("direct", "wrong-sensor");
    binding_view_t *future_sensor = find_view("future", "outside");
    UI_CHECK(power != NULL && strcmp(lv_label_get_text(power->name), "Solar output") == 0 &&
                 !lv_obj_has_flag(power->sensor.identity, LV_OBJ_FLAG_HIDDEN) &&
                 strcmp(lv_label_get_text(power->sensor.identity), "direct:power") == 0,
             "a configured label survives the initial missing placeholder");
    UI_CHECK(status != NULL && status->sensor.icon != NULL &&
                 strcmp(lv_label_get_text(status->sensor.icon), SLATE_ICON_FIRE) == 0,
             "a configured icon overrides the measurement default");

    UI_CHECK(publish_sensor_test_states() == ESP_OK,
             "all normalized sensor measurements published");
    UI_CHECK(sensor_view_text("temperature", "-12.4", "°C"),
             "temperature keeps one decimal and its unit");
    UI_CHECK(sensor_view_text("humidity", "59", "%"),
             "humidity rounds to a readable whole percent");
    UI_CHECK(sensor_view_text("pressure", "1013.25", "hPa"),
             "pressure preserves the out-of-range fixture value");
    UI_CHECK(sensor_view_text("power", "42.8", "W"),
             "power keeps useful precision and its unit");
    UI_CHECK(sensor_view_text("status", "Air quality needs attention", ""),
             "textual normalized sensor values render unchanged");
    wrong_sensor = find_view("direct", "wrong-sensor");
    future_sensor = find_view("future", "outside");
    UI_CHECK(wrong_sensor != NULL &&
                 !lv_obj_has_flag(wrong_sensor->sensor.identity, LV_OBJ_FLAG_HIDDEN) &&
                 strcmp(lv_label_get_text(wrong_sensor->sensor.identity),
                        "direct:wrong-sensor\nExpected sensor, got light") == 0,
             "incompatible sensor explains expected and received kinds");
    UI_CHECK(future_sensor != NULL &&
                 !lv_obj_has_flag(future_sensor->sensor.identity, LV_OBJ_FLAG_HIDDEN) &&
                 strcmp(lv_label_get_text(future_sensor->sensor.identity),
                        "future:outside") == 0,
             "missing-provider sensor keeps its provider-qualified identity");

    binding_view_t *temperature = find_view("direct", "temperature");
    binding_view_t *humidity = find_view("direct", "humidity");
    binding_view_t *pressure = find_view("direct", "pressure");
    power = find_view("direct", "power");
    status = find_view("direct", "status");
    UI_CHECK(temperature != NULL &&
                 lv_label_get_long_mode(temperature->name) == LV_LABEL_LONG_DOT &&
                 pressure != NULL &&
                 lv_label_get_long_mode(pressure->name) == LV_LABEL_LONG_DOT,
             "sensor names use glyph-safe ellipsis");
    lv_obj_update_layout(s_tree->screen);
    UI_CHECK(temperature != NULL &&
                 lv_obj_get_style_text_font(temperature->sensor.value, LV_PART_MAIN) ==
                     s_tree->theme->hero &&
                 pressure != NULL &&
                 lv_obj_get_style_text_font(pressure->sensor.value, LV_PART_MAIN) ==
                     s_tree->theme->body &&
                 status != NULL &&
                 lv_obj_get_style_text_font(status->sensor.value, LV_PART_MAIN) ==
                     s_tree->theme->caption,
             "sensor type scale steps down as rendered values grow");
    UI_CHECK(status != NULL &&
                 lv_label_get_long_mode(status->sensor.value) == LV_LABEL_LONG_DOT &&
                 lv_obj_get_height(status->sensor.value) ==
                     lv_font_get_line_height(s_tree->theme->caption) +
                         lv_obj_get_style_space_top(status->sensor.value, LV_PART_MAIN) +
                         lv_obj_get_style_space_bottom(status->sensor.value, LV_PART_MAIN),
             "long textual sensor recomputes one-line height after changing font");
    UI_CHECK(temperature != NULL && temperature->sensor.icon == NULL && humidity != NULL &&
                 humidity->sensor.icon == NULL,
             "1x1 sensors omit the leading icon");
    UI_CHECK(pressure != NULL && pressure->sensor.icon != NULL &&
                 strcmp(lv_label_get_text(pressure->sensor.icon), SLATE_ICON_GAUGE) == 0 &&
                 power != NULL && power->sensor.icon != NULL &&
                 strcmp(lv_label_get_text(power->sensor.icon), SLATE_ICON_LIGHTNING_BOLT) == 0,
             "2x1 sensors select icons from normalized measurement");
    UI_CHECK(power != NULL && strcmp(lv_label_get_text(power->name), "Solar output") == 0,
             "a configured label is restored when its resource becomes available");
    UI_CHECK(status != NULL && status->sensor.icon != NULL &&
                 strcmp(lv_label_get_text(status->sensor.icon), SLATE_ICON_FIRE) == 0,
             "an icon override remains selected after a state update");
    UI_CHECK(temperature != NULL && !lv_obj_has_flag(temperature->tile, LV_OBJ_FLAG_CLICKABLE) &&
                 pressure != NULL && !lv_obj_has_flag(pressure->tile, LV_OBJ_FLAG_CLICKABLE),
             "sensor variants expose no action target");

    const slate_snapshot_t unavailable = {
        .resource = "humidity",
        .kind = SLATE_KIND_SENSOR,
        .name = "Bathroom humidity",
        .available = false,
        .state.sensor = {.numeric = true, .value = 58.6, .unit = "%",
                         .measurement = SLATE_MEASUREMENT_HUMIDITY},
    };
    esp_err_t unavailable_err = slate_state_publish("direct", &unavailable);
    if (unavailable_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(unavailable_err == ESP_OK && sensor_view_text("humidity", "-", "%") &&
                 humidity != NULL &&
                 lv_obj_get_style_opa(humidity->tile, LV_PART_MAIN) == LV_OPA_50,
             "unavailable sensor keeps its unit and renders a dimmed dash");

    ui_tree_t *sensor_tree = s_tree;
    slate_provider_status_t sensor_direct_status =
        slate_state_provider_status("direct");
    esp_err_t offline_err =
        slate_state_provider_set_status("direct", SLATE_PROVIDER_OFFLINE);
    if (offline_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(offline_err == ESP_OK && sensor_view_text("temperature", "-", "°C") &&
                 temperature != NULL &&
                 lv_obj_get_style_opa(temperature->tile, LV_PART_MAIN) == LV_OPA_50 &&
                 strcmp(lv_label_get_text(s_tree->provider), "DIRECT OFFLINE") == 0 &&
                 s_tree == sensor_tree,
             "an offline provider dims its sensor and is named in the bar");

    esp_err_t online_err =
        slate_state_provider_set_status("direct", SLATE_PROVIDER_ONLINE);
    if (online_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(online_err == ESP_OK && sensor_view_text("temperature", "-12.4", "°C") &&
                 temperature != NULL &&
                 lv_obj_get_style_opa(temperature->tile, LV_PART_MAIN) == LV_OPA_COVER &&
                 s_tree == sensor_tree,
             "a sensor restores its value when the provider returns online");

    esp_err_t degraded_err =
        slate_state_provider_set_status("direct", SLATE_PROVIDER_DEGRADED);
    if (degraded_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(degraded_err == ESP_OK && sensor_view_text("temperature", "-12.4", "°C") &&
                 temperature != NULL &&
                 lv_obj_get_style_opa(temperature->tile, LV_PART_MAIN) == LV_OPA_COVER &&
                 strcmp(lv_label_get_text(s_tree->provider), "DIRECT DEGRADED") == 0 &&
                 s_tree == sensor_tree,
             "a degraded provider keeps state fresh and is named in the bar");
    slate_state_provider_set_status("direct", sensor_direct_status);
    slate_state_drain(discard_changed, NULL);
    update_all();

    slate_config_t *icons = sensor_icon_test_config();
    UI_CHECK(icons != NULL && rebuild_on_task(icons) == ESP_OK,
             "measurement icon test dashboard activated");
    if (icons != NULL) {
        slate_config_free(icons);
    }
    UI_CHECK(publish_sensor_test_states() == ESP_OK,
             "sensor measurements republished for icon verification");
    temperature = find_view("direct", "temperature");
    humidity = find_view("direct", "humidity");
    pressure = find_view("direct", "pressure");
    power = find_view("direct", "power");
    UI_CHECK(temperature != NULL && temperature->sensor.icon != NULL &&
                 strcmp(lv_label_get_text(temperature->sensor.icon),
                        SLATE_ICON_THERMOMETER) == 0 &&
                 humidity != NULL && humidity->sensor.icon != NULL &&
                 strcmp(lv_label_get_text(humidity->sensor.icon),
                        SLATE_ICON_WATER_PERCENT) == 0 &&
                 pressure != NULL && pressure->sensor.icon != NULL &&
                 strcmp(lv_label_get_text(pressure->sensor.icon), SLATE_ICON_GAUGE) == 0 &&
                 power != NULL && power->sensor.icon != NULL &&
                 strcmp(lv_label_get_text(power->sensor.icon),
                        SLATE_ICON_LIGHTNING_BOLT) == 0,
             "all normalized measurements select their semantic icon");

    slate_config_t *lights = light_test_config();
    UI_CHECK(lights != NULL && rebuild_on_task(lights) == ESP_OK,
             "light component test dashboard activated");
    if (lights != NULL) {
        slate_config_free(lights);
    }
    UI_CHECK(publish_light_test_states() == ESP_OK,
             "all light variants received direct-provider state");

    binding_view_t *compact = find_view("direct", "living-room");
    binding_view_t *wide = find_view("direct", "kitchen");
    binding_view_t *readonly = find_view("direct", "readonly");
    binding_view_t *large = find_view("direct", "studio");
    binding_view_t *toggle_only = find_view("direct", "switch-only");
    binding_view_t *missing_light = find_view("direct", "missing-light");
    UI_CHECK(compact != NULL && compact->light.compact && compact->light.state_dot != NULL &&
                 compact->light.brightness_slider == NULL &&
                 lv_obj_has_flag(compact->tile, LV_OBJ_FLAG_CLICKABLE) &&
                 !lv_obj_has_flag(compact->light.state_dot, LV_OBJ_FLAG_CLICKABLE) &&
                 !lv_obj_has_flag(compact->light.state_dot, LV_OBJ_FLAG_SCROLLABLE) &&
                 strcmp(lv_label_get_text(compact->light.icon),
                        SLATE_ICON_LIGHTBULB_ON) == 0,
             "1x1 light exposes toggle without a dead state-dot target");

    lv_obj_update_layout(s_tree->screen);
    UI_CHECK(compact != NULL &&
                 lv_label_get_long_mode(compact->name) == LV_LABEL_LONG_DOT &&
                 lv_obj_get_y(compact->name) >=
                     lv_obj_get_y(compact->light.icon) +
                         lv_obj_get_height(compact->light.icon),
             "a name longer than its cell is truncated, not wrapped over the icon");

    const slate_snapshot_t compact_with_late_dimming = {
        .resource = "living-room",
        .kind = SLATE_KIND_LIGHT,
        .name = "Living room",
        .available = true,
        .capabilities = {
            .actions = (1u << SLATE_ACTION_TOGGLE) |
                       (1u << SLATE_ACTION_SET_BRIGHTNESS),
            .brightness_min = 0,
            .brightness_max = 100,
        },
        .state.light = {.on = false, .brightness = 0,
                        .color_temperature = SLATE_STATE_ABSENT},
    };
    esp_err_t compact_late_err =
        slate_state_publish("direct", &compact_with_late_dimming);
    if (compact_late_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(compact_late_err == ESP_OK && compact != NULL &&
                 compact->light.brightness_slider == NULL &&
                 lv_obj_has_flag(compact->tile, LV_OBJ_FLAG_CLICKABLE),
             "late dimming capability is safe on compact light");
    UI_CHECK(wide != NULL && wide->light.brightness_slider != NULL &&
                 wide->light.temperature_slider == NULL &&
                 !lv_obj_has_flag(wide->light.brightness_slider, LV_OBJ_FLAG_HIDDEN) &&
                 lv_slider_get_min_value(wide->light.brightness_slider) == 5 &&
                 lv_slider_get_max_value(wide->light.brightness_slider) == 95 &&
                 strcmp(lv_label_get_text(wide->light.brightness_value), "62%") == 0,
             "2x1 light renders advertised brightness range");
    UI_CHECK(wide != NULL &&
                 lv_label_get_long_mode(wide->name) == LV_LABEL_LONG_DOT &&
                 strcmp(lv_label_get_text(wide->name),
                        "Kitchen pendants with a long name") == 0,
             "long light names use glyph-safe ellipsis without changing text");
    UI_CHECK(large != NULL && large->light.state_dot != NULL &&
                 large->light.brightness_slider != NULL &&
                 large->light.temperature_slider != NULL &&
                 lv_slider_get_min_value(large->light.temperature_slider) == 2200 &&
                 lv_slider_get_max_value(large->light.temperature_slider) == 6500 &&
                 strcmp(lv_label_get_text(large->name), "Mood light") == 0 &&
                 strcmp(lv_label_get_text(large->light.icon), SLATE_ICON_FLOOR_LAMP) == 0,
             "2x2 light renders large override and colour temperature");
    UI_CHECK(readonly != NULL &&
                 !lv_obj_has_flag(readonly->tile, LV_OBJ_FLAG_CLICKABLE),
             "read-only light exposes no action target");
    UI_CHECK(toggle_only != NULL && toggle_only->light.brightness_slider != NULL &&
                 lv_obj_has_flag(toggle_only->light.brightness_slider, LV_OBJ_FLAG_HIDDEN),
             "unadvertised brightness control stays hidden");
    UI_CHECK(missing_light != NULL &&
                 !lv_obj_has_flag(missing_light->light.identity, LV_OBJ_FLAG_HIDDEN) &&
                 strcmp(lv_label_get_text(missing_light->light.identity),
                        "direct:missing-light") == 0,
             "missing light identifies its provider-qualified resource");

    const slate_snapshot_t wrong_light = {
        .resource = "missing-light",
        .kind = SLATE_KIND_SENSOR,
        .available = true,
        .state.sensor = {.numeric = false, .text = "wrong kind"},
    };
    esp_err_t wrong_light_err = slate_state_publish("direct", &wrong_light);
    if (wrong_light_err == ESP_ERR_INVALID_STATE) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(wrong_light_err == ESP_ERR_INVALID_STATE && missing_light != NULL &&
                 strcmp(lv_label_get_text(missing_light->light.identity),
                        "direct:missing-light\nExpected light, got sensor") == 0,
             "incompatible light explains expected and received kinds");

    const slate_snapshot_t local_temperature = {
        .resource = "studio",
        .kind = SLATE_KIND_LIGHT,
        .name = "Studio",
        .available = true,
        .capabilities = {
            .actions = (1u << SLATE_ACTION_SET_BRIGHTNESS) |
                       (1u << SLATE_ACTION_SET_COLOR_TEMPERATURE),
            .brightness_min = 0,
            .brightness_max = 100,
        },
        .state.light = {.on = true, .brightness = 50, .color_temperature = 3300},
    };
    esp_err_t local_temperature_err = slate_state_publish("direct", &local_temperature);
    if (local_temperature_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(local_temperature_err == ESP_OK && large != NULL &&
                 lv_slider_get_min_value(large->light.temperature_slider) == 1800 &&
                 lv_slider_get_max_value(large->light.temperature_slider) == 4800,
             "unstated colour range is bounded around current state");

    slate_snapshot_t unknown_temperature = local_temperature;
    unknown_temperature.state.light.color_temperature = SLATE_STATE_ABSENT;
    esp_err_t unknown_temperature_err = slate_state_publish("direct", &unknown_temperature);
    if (unknown_temperature_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(unknown_temperature_err == ESP_OK && large != NULL &&
                 !lv_obj_has_flag(large->light.temperature_slider, LV_OBJ_FLAG_HIDDEN) &&
                 lv_slider_get_min_value(large->light.temperature_slider) == 1800 &&
                 lv_slider_get_max_value(large->light.temperature_slider) == 4800 &&
                 strcmp(lv_label_get_text(large->light.temperature_value), "-") == 0,
             "unstated colour range remains stable when the value disappears");

    slate_snapshot_t no_temperature = unknown_temperature;
    no_temperature.capabilities.actions = 1u << SLATE_ACTION_SET_BRIGHTNESS;
    esp_err_t no_temperature_err = slate_state_publish("direct", &no_temperature);
    if (no_temperature_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    esp_err_t default_temperature_err = slate_state_publish("direct", &unknown_temperature);
    if (default_temperature_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(no_temperature_err == ESP_OK && default_temperature_err == ESP_OK &&
                 large != NULL &&
                 lv_slider_get_min_value(large->light.temperature_slider) == 2000 &&
                 lv_slider_get_max_value(large->light.temperature_slider) == 6500,
             "new unstated colour control receives the neutral fallback range");

    const slate_snapshot_t unavailable_light = {
        .resource = "kitchen",
        .kind = SLATE_KIND_LIGHT,
        .name = "Kitchen",
        .available = false,
        .capabilities = {
            .actions = 1u << SLATE_ACTION_SET_BRIGHTNESS,
            .brightness_min = 0,
            .brightness_max = 100,
        },
        .state.light = {.on = true, .brightness = 62,
                        .color_temperature = SLATE_STATE_ABSENT},
    };
    esp_err_t unavailable_light_err = slate_state_publish("direct", &unavailable_light);
    if (unavailable_light_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(unavailable_light_err == ESP_OK && wide != NULL &&
                 strcmp(lv_label_get_text(wide->light.icon), "-") == 0 &&
                 strcmp(lv_label_get_text(wide->light.brightness_value), "-") == 0 &&
                 lv_obj_has_state(wide->light.brightness_slider, LV_STATE_DISABLED) &&
                 lv_obj_get_style_opa(wide->tile, LV_PART_MAIN) == LV_OPA_50,
             "unavailable light keeps controls but renders a dimmed dash");
    slate_snapshot_t unavailable_compact = compact_with_late_dimming;
    unavailable_compact.available = false;
    esp_err_t unavailable_compact_err =
        slate_state_publish("direct", &unavailable_compact);
    if (unavailable_compact_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(unavailable_compact_err == ESP_OK && compact != NULL &&
                 strcmp(lv_label_get_text(compact->light.icon), "-") == 0 &&
                 lv_obj_has_flag(compact->light.state_dot, LV_OBJ_FLAG_HIDDEN) &&
                 lv_obj_get_style_opa(compact->tile, LV_PART_MAIN) == LV_OPA_50,
             "compact unavailable light has an unambiguous dimmed dash");
    slate_config_t *light_layouts = light_layout_test_config();
    bool light_layouts_active = light_layouts != NULL &&
                                rebuild_on_task(light_layouts) == ESP_OK;
    UI_CHECK(light_layouts_active && verify_layout(light_layouts),
             "valid 1x2 and 4x1 light layouts keep exact grid geometry");
    if (light_layouts != NULL) {
        slate_config_free(light_layouts);
    }
    UI_CHECK(light_layouts_active && publish_light_layout_test_states() == ESP_OK,
             "fallback light layouts received normalized state");
    binding_view_t *vertical = find_view("direct", "living-room");
    binding_view_t *panoramic = find_view("direct", "kitchen");
    UI_CHECK(vertical != NULL && vertical->light.compact &&
                 vertical->light.brightness_slider == NULL &&
                 vertical->light.state_dot != NULL &&
                 !lv_obj_has_flag(vertical->light.state_dot, LV_OBJ_FLAG_CLICKABLE),
             "1x2 light uses the width-safe compact presentation");
    UI_CHECK(panoramic != NULL && panoramic->light.brightness_slider != NULL &&
                 lv_obj_get_width(panoramic->light.brightness_slider) ==
                     lv_obj_get_content_width(panoramic->tile) - 64,
             "4x1 brightness control expands without leaving its tile");

    slate_config_t *light_actions = light_action_test_config();
    UI_CHECK(light_actions != NULL && rebuild_on_task(light_actions) == ESP_OK,
             "semantic light action dashboard activated");
    if (light_actions != NULL) {
        slate_config_free(light_actions);
    }
    UI_CHECK(publish_action_test_states(true, 62, 3200) == ESP_OK,
             "action fixtures published normalized light state");

    binding_view_t *action_toggle = find_view("ui-fixture", "action-toggle");
    binding_view_t *action_dimmer = find_view("ui-fixture", "action-dimmer");
    binding_view_t *action_temperature = find_view("ui-fixture", "action-temperature");
    unsigned calls_before = s_fixture.action_calls;
    /* Where the tile's name sits before it is touched. The pending pulse must
     * not move it: a border would, because LVGL counts border width into the
     * content area every child is placed against. */
    lv_obj_update_layout(s_tree->screen);
    int32_t idle_name_x = action_toggle != NULL ? lv_obj_get_x(action_toggle->name) : 0;
    int32_t idle_name_y = action_toggle != NULL ? lv_obj_get_y(action_toggle->name) : 0;
    lv_result_t toggle_result = action_toggle != NULL
                                    ? lv_obj_send_event(action_toggle->tile,
                                                        LV_EVENT_CLICKED, NULL)
                                    : LV_RESULT_INVALID;
    slate_action_feedback_t toggle_feedback = {0};
    esp_err_t toggle_feedback_err = slate_action_feedback(
        "ui-fixture", "action-toggle", &toggle_feedback);
    update_all();
    UI_CHECK(toggle_result == LV_RESULT_OK &&
                 s_fixture.action_calls == calls_before + 1 &&
                 strcmp(s_fixture.action_resource, "action-toggle") == 0 &&
                 s_fixture.action == SLATE_ACTION_TOGGLE &&
                 s_fixture.value_type == SLATE_ACTION_VALUE_NONE &&
                 toggle_feedback_err == ESP_OK &&
                 toggle_feedback.phase == SLATE_ACTION_PENDING &&
                 !toggle_feedback.optimistic.light.on &&
                 action_toggle->light.pending_animation &&
                 lv_obj_get_style_outline_width(action_toggle->tile,
                                                LV_PART_MAIN) == 2 &&
                 lv_obj_get_style_border_width(action_toggle->tile,
                                               LV_PART_MAIN) == 0,
             "1x1 tap emits optimistic toggle with a pending pulse");
    lv_obj_update_layout(s_tree->screen);
    UI_CHECK(action_toggle != NULL &&
                 lv_obj_get_x(action_toggle->name) == idle_name_x &&
                 lv_obj_get_y(action_toggle->name) == idle_name_y,
             "the pending pulse leaves tile content where it was");
    UI_CHECK(publish_action_test_states(false, 62, 3200) == ESP_OK,
             "matching state confirms optimistic toggle");
    UI_CHECK(slate_action_feedback("ui-fixture", "action-toggle", &toggle_feedback) == ESP_OK &&
                 toggle_feedback.phase == SLATE_ACTION_IDLE &&
                 !action_toggle->light.pending_animation,
             "matching toggle snapshot clears pending state");

    if (action_dimmer != NULL) {
        lv_slider_set_value(action_dimmer->light.brightness_slider, 37, LV_ANIM_OFF);
    }
    calls_before = s_fixture.action_calls;
    lv_result_t brightness_result =
        action_dimmer != NULL
            ? lv_obj_send_event(action_dimmer->light.brightness_slider,
                                LV_EVENT_RELEASED, NULL)
            : LV_RESULT_INVALID;
    slate_action_feedback_t brightness_feedback = {0};
    esp_err_t brightness_feedback_err = slate_action_feedback(
        "ui-fixture", "action-dimmer", &brightness_feedback);
    update_all();
    UI_CHECK(brightness_result == LV_RESULT_OK &&
                 s_fixture.action_calls == calls_before + 1 &&
                 strcmp(s_fixture.action_resource, "action-dimmer") == 0 &&
                 s_fixture.action == SLATE_ACTION_SET_BRIGHTNESS &&
                 s_fixture.value_type == SLATE_ACTION_VALUE_NUMBER &&
                 s_fixture.value == 37 && brightness_feedback_err == ESP_OK &&
                 brightness_feedback.phase == SLATE_ACTION_PENDING &&
                 brightness_feedback.optimistic.light.brightness == 37 &&
                 strcmp(lv_label_get_text(action_dimmer->light.brightness_value), "37%") == 0 &&
                 lv_obj_has_state(action_dimmer->light.brightness_slider, LV_STATE_DISABLED) &&
                 action_dimmer->light.pending_animation,
             "brightness release emits one semantic optimistic action");

    slate_action_result("ui-fixture", s_fixture.action_id, false, "fixture_failure");
    update_all();
    UI_CHECK(action_dimmer != NULL &&
                 strcmp(lv_label_get_text(action_dimmer->light.brightness_value), "62%") == 0 &&
                 lv_obj_get_style_outline_width(action_dimmer->tile, LV_PART_MAIN) == 2 &&
                 !action_dimmer->light.pending_animation,
             "failed brightness action reverts and renders error state");

    if (action_dimmer != NULL) {
        lv_slider_set_value(action_dimmer->light.brightness_slider, 55, LV_ANIM_OFF);
        lv_obj_send_event(action_dimmer->light.brightness_slider, LV_EVENT_RELEASED, NULL);
    }
    UI_CHECK(publish_action_test_states(false, 55, 3200) == ESP_OK,
             "matching brightness snapshot confirms action");
    UI_CHECK(slate_action_feedback("ui-fixture", "action-dimmer", &brightness_feedback) == ESP_OK &&
                 brightness_feedback.phase == SLATE_ACTION_IDLE && action_dimmer != NULL &&
                 strcmp(lv_label_get_text(action_dimmer->light.brightness_value), "55%") == 0,
             "confirmed brightness clears pending presentation");

    int32_t temperature_min = action_temperature != NULL
                                  ? lv_slider_get_min_value(
                                        action_temperature->light.temperature_slider)
                                  : 0;
    int32_t temperature_max = action_temperature != NULL
                                  ? lv_slider_get_max_value(
                                        action_temperature->light.temperature_slider)
                                  : 0;
    if (action_temperature != NULL) {
        lv_slider_set_value(action_temperature->light.temperature_slider,
                            4100, LV_ANIM_OFF);
    }
    calls_before = s_fixture.action_calls;
    lv_result_t temperature_result =
        action_temperature != NULL
            ? lv_obj_send_event(action_temperature->light.temperature_slider,
                                LV_EVENT_RELEASED, NULL)
            : LV_RESULT_INVALID;
    slate_action_feedback_t temperature_feedback = {0};
    esp_err_t temperature_feedback_err = slate_action_feedback(
        "ui-fixture", "action-temperature", &temperature_feedback);
    update_all();
    UI_CHECK(temperature_result == LV_RESULT_OK &&
                 s_fixture.action_calls == calls_before + 1 &&
                 strcmp(s_fixture.action_resource, "action-temperature") == 0 &&
                 s_fixture.action == SLATE_ACTION_SET_COLOR_TEMPERATURE &&
                 s_fixture.value_type == SLATE_ACTION_VALUE_NUMBER &&
                 s_fixture.value == 4100 && temperature_feedback_err == ESP_OK &&
                 temperature_feedback.phase == SLATE_ACTION_PENDING &&
                 temperature_feedback.optimistic.light.color_temperature == 4100 &&
                 action_temperature->light.pending_animation &&
                 lv_slider_get_min_value(action_temperature->light.temperature_slider) ==
                     temperature_min &&
                 lv_slider_get_max_value(action_temperature->light.temperature_slider) ==
                     temperature_max,
             "colour release emits one action without recentering its range");
    UI_CHECK(publish_action_test_states(false, 55, 4100) == ESP_OK,
             "matching colour-temperature snapshot confirms action");
    UI_CHECK(slate_action_feedback("ui-fixture", "action-temperature",
                                   &temperature_feedback) == ESP_OK &&
                 temperature_feedback.phase == SLATE_ACTION_IDLE &&
                 action_temperature != NULL &&
                 !action_temperature->light.pending_animation &&
                 strcmp(lv_label_get_text(action_temperature->light.temperature_value),
                        "4100 K") == 0,
             "confirmed colour temperature clears pending presentation");

    slate_config_t *covers = cover_test_config();
    UI_CHECK(covers != NULL && rebuild_on_task(covers) == ESP_OK && verify_layout(covers),
             "all three cover layouts activated on the exact grid");
    UI_CHECK(publish_cover_test_states() == ESP_OK,
             "cover variants received normalized state");

    binding_view_t *compact_cover = find_view("ui-fixture", "cover-compact");
    binding_view_t *horizontal_cover = find_view("ui-fixture", "cover-horizontal");
    binding_view_t *vertical_cover = find_view("ui-fixture", "cover-vertical");
    binding_view_t *missing_cover = find_view("ui-fixture", "cover-missing");
    binding_view_t *unavailable_cover = find_view("ui-fixture", "cover-unavailable");
    binding_view_t *wrong_cover = find_view("ui-fixture", "cover-wrong");
    lv_obj_update_layout(s_tree->screen);
    bool cover_targets_ok = horizontal_cover != NULL && vertical_cover != NULL &&
                            lv_obj_get_width(horizontal_cover->cover.open_button) >= 48 &&
                            lv_obj_get_height(horizontal_cover->cover.open_button) >= 48 &&
                            lv_obj_get_width(horizontal_cover->cover.stop_button) >= 48 &&
                            lv_obj_get_height(horizontal_cover->cover.stop_button) >= 48 &&
                            lv_obj_get_width(horizontal_cover->cover.close_button) >= 48 &&
                            lv_obj_get_height(horizontal_cover->cover.close_button) >= 48 &&
                            lv_obj_get_width(vertical_cover->cover.open_button) >= 48 &&
                            lv_obj_get_height(vertical_cover->cover.open_button) >= 48 &&
                            lv_obj_get_width(vertical_cover->cover.stop_button) >= 48 &&
                            lv_obj_get_height(vertical_cover->cover.stop_button) >= 48 &&
                            lv_obj_get_width(vertical_cover->cover.close_button) >= 48 &&
                            lv_obj_get_height(vertical_cover->cover.close_button) >= 48;
    UI_CHECK(cover_targets_ok, "1x2 and 2x1 cover controls meet the 48 px target");
    UI_CHECK(vertical_cover != NULL &&
                 lv_obj_get_y(vertical_cover->cover.name) +
                         lv_obj_get_height(vertical_cover->cover.name) <=
                     lv_obj_get_y(vertical_cover->cover.open_button),
             "vertical cover name keeps clear of its first control");
    UI_CHECK(compact_cover != NULL && compact_cover->cover.compact &&
                 strcmp(lv_label_get_text(compact_cover->cover.icon),
                        SLATE_ICON_WINDOW_SHUTTER) == 0 &&
                 strcmp(lv_label_get_text(compact_cover->cover.position), "0%") == 0 &&
                 lv_obj_has_flag(compact_cover->tile, LV_OBJ_FLAG_CLICKABLE),
             "compact cover icon follows position and remains actionable");
    UI_CHECK(vertical_cover != NULL && vertical_cover->cover.vertical &&
                 strcmp(lv_label_get_text(vertical_cover->cover.name),
                        "Bedroom curtain") == 0 &&
                 strcmp(lv_label_get_text(vertical_cover->cover.icon),
                        SLATE_ICON_CURTAINS) == 0,
             "vertical cover keeps configured label and icon overrides");
    UI_CHECK(horizontal_cover != NULL && horizontal_cover->cover.motion_animation &&
                 !lv_obj_has_flag(horizontal_cover->cover.motion, LV_OBJ_FLAG_HIDDEN) &&
                 strcmp(lv_label_get_text(horizontal_cover->cover.motion),
                        SLATE_ICON_ARROW_UP) == 0 &&
                 lv_obj_has_flag(horizontal_cover->cover.stop_button,
                                 LV_OBJ_FLAG_CLICKABLE),
             "opening cover animates its direction and enables stop");
    UI_CHECK(horizontal_cover != NULL &&
                 lv_label_get_long_mode(horizontal_cover->cover.name) ==
                     LV_LABEL_LONG_DOT &&
                 vertical_cover != NULL &&
                 lv_label_get_long_mode(vertical_cover->cover.name) ==
                     LV_LABEL_LONG_DOT,
             "cover names use glyph-safe ellipsis");
    UI_CHECK(missing_cover != NULL &&
                 !lv_obj_has_flag(missing_cover->cover.identity, LV_OBJ_FLAG_HIDDEN) &&
                 strcmp(lv_label_get_text(missing_cover->cover.identity),
                        "ui-fixture:cover-missing") == 0 &&
                 wrong_cover != NULL &&
                 !lv_obj_has_flag(wrong_cover->cover.identity, LV_OBJ_FLAG_HIDDEN) &&
                 strcmp(lv_label_get_text(wrong_cover->cover.identity),
                        "ui-fixture:cover-wrong\nExpected cover, got sensor") == 0,
             "cover placeholders identify missing resources and kind mismatches");
    UI_CHECK(unavailable_cover != NULL &&
                 strcmp(lv_label_get_text(unavailable_cover->cover.position), "-") == 0 &&
                 lv_obj_get_style_opa(unavailable_cover->tile, LV_PART_MAIN) == LV_OPA_50 &&
                 !lv_obj_has_flag(unavailable_cover->cover.open_button,
                                  LV_OBJ_FLAG_CLICKABLE),
             "unavailable cover renders a dimmed dash and disables controls");

    unsigned cover_calls = s_fixture.action_calls;
    lv_result_t compact_toggle = compact_cover != NULL
                                     ? lv_obj_send_event(compact_cover->tile,
                                                         LV_EVENT_CLICKED, NULL)
                                     : LV_RESULT_INVALID;
    slate_action_feedback_t cover_feedback = {0};
    esp_err_t cover_feedback_err =
        slate_action_feedback("ui-fixture", "cover-compact", &cover_feedback);
    update_all();
    UI_CHECK(compact_toggle == LV_RESULT_OK &&
                 s_fixture.action_calls == cover_calls + 1 &&
                 s_fixture.action == SLATE_ACTION_TOGGLE &&
                 strcmp(s_fixture.action_resource, "cover-compact") == 0 &&
                 cover_feedback_err == ESP_OK &&
                 cover_feedback.phase == SLATE_ACTION_PENDING &&
                 cover_feedback.optimistic.cover.motion == SLATE_COVER_OPENING &&
                 compact_cover != NULL &&
                 lv_obj_get_style_outline_width(compact_cover->tile,
                                                LV_PART_MAIN) == 2,
             "1x1 cover tap emits an optimistic semantic toggle");
    UI_CHECK(publish_cover_state("cover-compact", 0, SLATE_COVER_OPENING, true) == ESP_OK &&
                 slate_action_feedback("ui-fixture", "cover-compact", &cover_feedback) ==
                     ESP_OK &&
                 cover_feedback.phase == SLATE_ACTION_IDLE && compact_cover != NULL &&
                 compact_cover->cover.motion_animation,
             "confirmed opening state clears pending while motion keeps animating");

    cover_calls = s_fixture.action_calls;
    lv_result_t stop_result = horizontal_cover != NULL
                                  ? lv_obj_send_event(horizontal_cover->cover.stop_button,
                                                      LV_EVENT_CLICKED, NULL)
                                  : LV_RESULT_INVALID;
    cover_feedback_err =
        slate_action_feedback("ui-fixture", "cover-horizontal", &cover_feedback);
    update_all();
    UI_CHECK(stop_result == LV_RESULT_OK && s_fixture.action_calls == cover_calls + 1 &&
                 s_fixture.action == SLATE_ACTION_STOP &&
                 strcmp(s_fixture.action_resource, "cover-horizontal") == 0 &&
                 cover_feedback_err == ESP_OK &&
                 cover_feedback.phase == SLATE_ACTION_PENDING &&
                 cover_feedback.optimistic.cover.motion == SLATE_COVER_IDLE &&
                 horizontal_cover != NULL && !horizontal_cover->cover.motion_animation,
             "mid-travel stop immediately settles the optimistic motion indicator");
    slate_action_result("ui-fixture", s_fixture.action_id, true, NULL);
    update_all();
    UI_CHECK(publish_cover_state("cover-horizontal", 45, SLATE_COVER_IDLE, true) == ESP_OK &&
                 slate_action_feedback("ui-fixture", "cover-horizontal", &cover_feedback) ==
                     ESP_OK &&
                 cover_feedback.phase == SLATE_ACTION_IDLE && horizontal_cover != NULL &&
                 !horizontal_cover->cover.motion_animation &&
                 lv_obj_has_flag(horizontal_cover->cover.open_button,
                                 LV_OBJ_FLAG_CLICKABLE) &&
                 !lv_obj_has_flag(horizontal_cover->cover.stop_button,
                                  LV_OBJ_FLAG_CLICKABLE) &&
                 lv_obj_has_flag(horizontal_cover->cover.close_button,
                                 LV_OBJ_FLAG_CLICKABLE),
             "idle snapshot confirms stop and restores directional controls");

    bool directional_actions_ok = horizontal_cover != NULL;
    if (horizontal_cover != NULL) {
        lv_obj_t *buttons[] = {
            horizontal_cover->cover.open_button,
            horizontal_cover->cover.close_button,
        };
        slate_action_t actions[] = {SLATE_ACTION_OPEN, SLATE_ACTION_CLOSE};
        for (size_t i = 0; i < 2; i++) {
            cover_calls = s_fixture.action_calls;
            directional_actions_ok = directional_actions_ok &&
                                     lv_obj_send_event(buttons[i], LV_EVENT_CLICKED, NULL) ==
                                         LV_RESULT_OK &&
                                     s_fixture.action_calls == cover_calls + 1 &&
                                     s_fixture.action == actions[i];
            slate_action_result("ui-fixture", s_fixture.action_id, false,
                                "fixture_reset");
            update_all();
        }
    }
    UI_CHECK(directional_actions_ok,
             "2x1 controls emit provider-neutral open and close actions");

    esp_err_t cover_offline_err =
        slate_state_provider_set_status("ui-fixture", SLATE_PROVIDER_OFFLINE);
    if (cover_offline_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(cover_offline_err == ESP_OK && horizontal_cover != NULL &&
                 strcmp(lv_label_get_text(horizontal_cover->cover.position), "-") == 0 &&
                 lv_obj_get_style_opa(horizontal_cover->tile, LV_PART_MAIN) == LV_OPA_50 &&
                 !lv_obj_has_flag(horizontal_cover->cover.open_button,
                                  LV_OBJ_FLAG_CLICKABLE),
             "offline cover stays visible with stale controls disabled");
    slate_state_provider_set_status("ui-fixture", SLATE_PROVIDER_ONLINE);
    slate_state_drain(discard_changed, NULL);
    update_all();
    if (covers != NULL) {
        slate_config_free(covers);
    }

    slate_config_t *scenes = scene_test_config();
    UI_CHECK(scenes != NULL && rebuild_on_task(scenes) == ESP_OK,
             "scene component test dashboard activated");
    UI_CHECK(publish_scene_test_states() == ESP_OK,
             "scene bar received stateless normalized resources");

    static const char *SCENE_RESOURCES[] = {
        "scene-movie", "scene-relax", "scene-dinner", "scene-away", "scene-missing",
    };
    bool bar_layout_ok = true;
    bool all_activated = true;
    bool all_confirmed = true;
    lv_obj_update_layout(s_tree->screen);
    for (size_t i = 0; i < sizeof(SCENE_RESOURCES) / sizeof(SCENE_RESOURCES[0]); i++) {
        binding_view_t *scene_view = find_view("ui-fixture", SCENE_RESOURCES[i]);
        bar_layout_ok = bar_layout_ok && scene_view != NULL &&
                        scene_view->scene.button != NULL &&
                        lv_obj_get_width(scene_view->scene.button) >= 48 &&
                        lv_obj_get_height(scene_view->scene.button) >= 48 &&
                        lv_obj_has_flag(scene_view->scene.button, LV_OBJ_FLAG_CLICKABLE);
        unsigned scene_calls = s_fixture.action_calls;
        lv_result_t scene_result = scene_view != NULL
                                       ? lv_obj_send_event(scene_view->scene.button,
                                                           LV_EVENT_CLICKED, NULL)
                                       : LV_RESULT_INVALID;
        slate_action_feedback_t scene_feedback = {0};
        esp_err_t scene_feedback_err = slate_action_feedback(
            "ui-fixture", SCENE_RESOURCES[i], &scene_feedback);
        update_all();
        unsigned pending_outline = scene_view != NULL
                                       ? lv_obj_get_style_outline_width(
                                             scene_view->scene.button, LV_PART_MAIN)
                                       : 0;
        all_activated = all_activated && scene_result == LV_RESULT_OK &&
                        s_fixture.action_calls == scene_calls + 1 &&
                        strcmp(s_fixture.action_resource, SCENE_RESOURCES[i]) == 0 &&
                        s_fixture.action == SLATE_ACTION_ACTIVATE &&
                        s_fixture.value_type == SLATE_ACTION_VALUE_NONE &&
                        scene_feedback_err == ESP_OK &&
                        scene_feedback.phase == SLATE_ACTION_PENDING &&
                        scene_view != NULL &&
                        pending_outline == 2;

        slate_action_result("ui-fixture", s_fixture.action_id, true, NULL);
        update_all();
        scene_feedback_err = slate_action_feedback(
            "ui-fixture", SCENE_RESOURCES[i], &scene_feedback);
        all_confirmed = all_confirmed && scene_feedback_err == ESP_OK &&
                        scene_feedback.phase == SLATE_ACTION_SUCCESS &&
                        scene_view != NULL &&
                        lv_color_eq(lv_obj_get_style_bg_color(scene_view->scene.button,
                                                              LV_PART_MAIN),
                                    lv_color_hex(s_tree->theme->accent)) &&
                        lv_color_eq(lv_obj_get_style_text_color(scene_view->scene.icon,
                                                                LV_PART_MAIN),
                                    lv_color_hex(s_tree->theme->on_accent)) &&
                        lv_color_eq(lv_obj_get_style_text_color(scene_view->scene.name,
                                                                LV_PART_MAIN),
                                    lv_color_hex(s_tree->theme->on_accent));
    }
    UI_CHECK(bar_layout_ok, "4x1 bar gives every scene an independent touch target");
    UI_CHECK(all_activated, "every scene button emits its own activate action");
    UI_CHECK(all_confirmed,
             "accepted scenes flash only their button with a legible foreground");

    binding_view_t *compact_scene = find_view("ui-fixture", "scene-goodnight");
    binding_view_t *missing_scene = find_view("ui-fixture", "scene-missing");
    UI_CHECK(compact_scene != NULL && compact_scene->scene.compact &&
                 strcmp(lv_label_get_text(compact_scene->scene.name), "Good night") == 0 &&
                 strcmp(lv_label_get_text(compact_scene->scene.icon), SLATE_ICON_SLEEP) == 0,
             "1x1 scene renders label and icon overrides");
    UI_CHECK(compact_scene != NULL &&
                 lv_label_get_long_mode(compact_scene->scene.name) ==
                     LV_LABEL_LONG_DOT &&
                 missing_scene != NULL &&
                 lv_label_get_long_mode(missing_scene->scene.name) ==
                     LV_LABEL_LONG_DOT,
             "scene names use glyph-safe ellipsis");
    bool rebound_without_snapshots = scenes != NULL &&
                                       slate_state_bind(NULL, 0) == ESP_OK &&
                                       rebuild_on_task(scenes) == ESP_OK;
    missing_scene = find_view("ui-fixture", "scene-missing");
    binding_view_t *peer_scene = find_view("ui-fixture", "scene-movie");
    UI_CHECK(rebound_without_snapshots && missing_scene != NULL &&
                 !lv_obj_has_flag(missing_scene->scene.identity, LV_OBJ_FLAG_HIDDEN) &&
                 strcmp(lv_label_get_text(missing_scene->scene.identity),
                        "ui-fixture:scene-missing") == 0 &&
                 !lv_obj_has_flag(missing_scene->scene.button, LV_OBJ_FLAG_CLICKABLE),
             "missing scene identifies its provider-qualified resource");

    const slate_snapshot_t wrong_scene = {
        .resource = "scene-missing",
        .kind = SLATE_KIND_LIGHT,
        .available = true,
        .state.light = {.brightness = SLATE_STATE_ABSENT,
                        .color_temperature = SLATE_STATE_ABSENT},
    };
    esp_err_t wrong_scene_err = slate_state_publish("ui-fixture", &wrong_scene);
    if (wrong_scene_err == ESP_ERR_INVALID_STATE) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(wrong_scene_err == ESP_ERR_INVALID_STATE && missing_scene != NULL &&
                 peer_scene != NULL &&
                 missing_scene->tile == missing_scene->scene.button &&
                 peer_scene->tile == peer_scene->scene.button &&
                 missing_scene->scene.button != peer_scene->scene.button &&
                 strcmp(lv_label_get_text(missing_scene->scene.identity),
                        "ui-fixture:scene-missing\nExpected scene, got light") == 0 &&
                 lv_obj_get_style_outline_width(missing_scene->scene.button,
                                                LV_PART_MAIN) == 2 &&
                 lv_color_eq(lv_obj_get_style_outline_color(
                                 missing_scene->scene.button, LV_PART_MAIN),
                             lv_color_hex(s_tree->theme->warn)) &&
                 lv_obj_get_style_outline_width(peer_scene->scene.button,
                                                LV_PART_MAIN) == 0,
             "incompatible scene explains and isolates its warning");

    const slate_snapshot_t unavailable_scene = {
        .resource = "scene-missing",
        .kind = SLATE_KIND_SCENE,
        .name = "Unavailable scene",
        .available = false,
        .capabilities.actions = 1u << SLATE_ACTION_ACTIVATE,
    };
    esp_err_t unavailable_scene_err =
        slate_state_publish("ui-fixture", &unavailable_scene);
    if (unavailable_scene_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(unavailable_scene_err == ESP_OK && missing_scene != NULL &&
                 peer_scene != NULL &&
                 lv_obj_has_flag(missing_scene->scene.identity, LV_OBJ_FLAG_HIDDEN) &&
                 strcmp(lv_label_get_text(missing_scene->scene.icon), "-") == 0 &&
                 lv_obj_get_style_opa(missing_scene->scene.button, LV_PART_MAIN) ==
                     LV_OPA_50 &&
                 lv_obj_get_style_opa(peer_scene->scene.button, LV_PART_MAIN) ==
                     LV_OPA_COVER &&
                 !lv_obj_has_flag(missing_scene->scene.button, LV_OBJ_FLAG_CLICKABLE),
             "unavailable scene dims only its button and stays disabled");
    esp_err_t scene_offline_err =
        slate_state_provider_set_status("ui-fixture", SLATE_PROVIDER_OFFLINE);
    if (scene_offline_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(scene_offline_err == ESP_OK && missing_scene != NULL &&
                 strcmp(lv_label_get_text(missing_scene->scene.icon), "-") == 0 &&
                 lv_obj_get_style_opa(missing_scene->scene.button, LV_PART_MAIN) ==
                     LV_OPA_50 &&
                 !lv_obj_has_flag(missing_scene->scene.button, LV_OBJ_FLAG_CLICKABLE),
             "offline scene renders a dimmed dash and stays disabled");
    slate_state_provider_set_status("ui-fixture", SLATE_PROVIDER_ONLINE);
    slate_state_drain(discard_changed, NULL);
    update_all();
    if (scenes != NULL) {
        slate_config_free(scenes);
    }

    lights = light_test_config();
    UI_CHECK(lights != NULL && rebuild_on_task(lights) == ESP_OK,
             "direct-provider light dashboard restored for inspection");
    if (lights != NULL) {
        slate_config_free(lights);
    }
    UI_CHECK(publish_light_test_states() == ESP_OK,
             "direct-provider light dashboard left populated");

    ESP_LOGI(TAG, "selftest: %u check(s), %u failure(s)", checks, failures);
#undef UI_CHECK
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

static void selftest_work(void *ctx)
{
    selftest_request_t *request = ctx;
    request->result = selftest_on_task();
    xSemaphoreGive(request->done);
}

esp_err_t slate_ui_selftest(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    selftest_request_t request = {.done = done, .result = ESP_ERR_INVALID_STATE};
    esp_err_t err = slate_display_post(selftest_work, &request, UI_REBUILD_POST_TIMEOUT_MS);
    if (err == ESP_OK) {
        xSemaphoreTake(done, portMAX_DELAY);
        err = request.result;
    }
    vSemaphoreDelete(done);
    return err;
}

#endif /* SLATE_UI_SELFTEST */
