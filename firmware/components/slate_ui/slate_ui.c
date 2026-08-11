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
#include "slate_display.h"
#include "slate_sensor.h"
#include "slate_state.h"
#include "slate_store.h"
#include "slate_theme.h"
#include "slate_time.h"

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

typedef struct {
    char provider[SLATE_PROVIDER_ID_MAX + 1];
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    slate_kind_t kind;
    bool label_override;
    lv_obj_t *tile;
    lv_obj_t *name;
    lv_obj_t *detail;
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

static void update_bar(ui_tree_t *tree)
{
    if (tree == NULL) {
        return;
    }

    char clock[8] = "--:--";
    if (slate_time_synced()) {
        time_t now = time(NULL);
        struct tm local;
        if (localtime_r(&now, &local) != NULL) {
            strftime(clock, sizeof(clock), "%H:%M", &local);
        }
    }
    lv_label_set_text(tree->clock, clock);

    size_t provider_count = slate_state_provider_count();
    bool offline = false;
    bool degraded = false;
    for (size_t i = 0; i < provider_count; i++) {
        slate_state_provider_info_t info;
        if (slate_state_provider_at(i, &info) != ESP_OK) {
            continue;
        }
        switch (info.status) {
        case SLATE_PROVIDER_CONNECTING:
        case SLATE_PROVIDER_OFFLINE:
        case SLATE_PROVIDER_ERROR:
            offline = true;
            break;
        case SLATE_PROVIDER_UNCONFIGURED:
        case SLATE_PROVIDER_DEGRADED:
            degraded = true;
            break;
        case SLATE_PROVIDER_ONLINE:
            break;
        }
    }

    const char *text = offline ? "PROVIDERS OFFLINE"
                                : degraded ? "PROVIDERS DEGRADED" : "PROVIDERS ONLINE";
    uint32_t color = offline ? tree->theme->warn
                             : degraded ? tree->theme->text_lo : tree->theme->accent;
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
    lv_obj_set_style_border_width(view->tile,
                                  warn || phase == SLATE_ACTION_PENDING ? 2 : 0,
                                  LV_PART_MAIN);
    lv_obj_set_style_border_color(
        view->tile,
        lv_color_hex(warn ? theme->warn : theme->accent),
        LV_PART_MAIN);
}

static void update_sensor_view(binding_view_t *view, const slate_resource_t *resource,
                               const slate_theme_t *theme)
{
    slate_sensor_update(&view->sensor, resource, theme);

    if (resource->presentation == SLATE_PRESENT_MISSING ||
        resource->presentation == SLATE_PRESENT_MISSING_PROVIDER ||
        resource->presentation == SLATE_PRESENT_INCOMPATIBLE) {
        char identity[SLATE_PROVIDER_ID_MAX + SLATE_RESOURCE_ID_MAX + 2];
        snprintf(identity, sizeof(identity), "%s:%s", view->provider, view->resource);
        if (view->label_override) {
            lv_label_set_text(view->sensor.value, identity);
            lv_obj_set_style_text_font(view->sensor.value, theme->caption, LV_PART_MAIN);
        } else {
            lv_label_set_text(view->name, identity);
        }
    } else if (!view->label_override) {
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
    lv_label_set_long_mode(page_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(page_title, 320);
    lv_obj_align(page_title, LV_ALIGN_CENTER, 0, 0);

    tree->provider = make_label(bar, "PROVIDERS DEGRADED", theme->caption, theme->text_lo);
    if (tree->provider == NULL) {
        tree_destroy(tree);
        return NULL;
    }
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
    return slate_sensor_build(object, tile, theme, &view->sensor, &view->name);
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
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_label, lv_pct(100));
    lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 0, 0);

    char identity[SLATE_PROVIDER_ID_MAX + SLATE_RESOURCE_ID_MAX + 16];
    snprintf(identity, sizeof(identity), "%s:%s  ·  missing", first->provider, first->resource);
    lv_obj_t *detail = make_label(object, identity, theme->caption, theme->text_lo);
    if (detail == NULL) {
        return false;
    }
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
    lv_obj_set_width(detail, lv_pct(100));
    lv_obj_align(detail, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Every binding enters the state store. Until #26 owns the scene bar, its
     * generic shell has one shared presentation label; updates to any scene
     * binding still wake and redraw that same semantic tile. */
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
        } else if (tile->component == SLATE_COMPONENT_SENSOR) {
            if (!build_sensor_tile(tree, tile, object, &view_index)) {
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

    if (config->settings.timezone != NULL) {
        (void) slate_time_set_timezone(config->settings.timezone);
    }
    activate_tree(fresh);
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
    lv_obj_set_size(card, 650, 250);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(theme->surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, theme->radius, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 30, LV_PART_MAIN);

    lv_obj_t *heading = make_label(card, title, theme->body, theme->warn);
    if (heading == NULL) {
        tree_destroy(tree);
        return NULL;
    }
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *body = make_label(card, message, theme->caption, theme->text_lo);
    if (body == NULL) {
        tree_destroy(tree);
        return NULL;
    }
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, 590);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 55);
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

    char *json = NULL;
    size_t len = 0;
    esp_err_t read_err = slate_store_config_read(&json, &len);
    if (read_err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "no stored dashboard configuration");
        return show_message("NO DASHBOARD", "Push a schema-1 configuration to this panel.", true);
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
                            future ? "This dashboard was created for newer Slate firmware."
                                   : "The stored dashboard is invalid. The device API remains available.",
                            true);
    }
    slate_config_report_free(&report);

    esp_err_t err = slate_ui_rebuild(config);
    slate_config_free(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stored dashboard activation failed: %s", esp_err_to_name(err));
        return show_message("DASHBOARD ERROR",
                            "The dashboard could not be built. The device API remains available.",
                            true);
    }
    return ESP_OK;
}

bool slate_ui_ready(void)
{
    return s_ready;
}

#ifdef SLATE_UI_SELFTEST

#define UI_TEST_WARMUP 10
#define UI_TEST_CYCLES 500

typedef struct {
    size_t subscriptions;
    unsigned calls;
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
        "\"bindings\":[{\"provider\":\"direct\",\"resource\":\"relax\"}]}",

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
    int written = snprintf(json, capacity,
                           "{\"schema\":1,\"theme\":\"midnight\","
                           "\"home_page\":\"home\",\"pages\":[{\"id\":\"home\","
                           "\"title\":\"Runtime cycle %d\",\"tiles\":[%s]},"
                           "{\"id\":\"secondary\",\"title\":\"Secondary\",\"tiles\":["
                           "{\"id\":\"offscreen\",\"type\":\"sensor\","
                           "\"pos\":[0,0],\"size\":[1,1],\"binding\":{"
                           "\"provider\":\"ui-fixture\",\"resource\":\"offscreen\"}}]}]}",
                           cycle, layout);
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
        "{\"schema\":1,\"theme\":\"midnight\",\"home_page\":\"sensors\",\"pages\":[{"
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
        "\"binding\":{\"provider\":\"direct\",\"resource\":\"status\"}}]}]}";

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
        "{\"schema\":1,\"theme\":\"midnight\",\"home_page\":\"icons\",\"pages\":[{"
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
            .state.sensor = {.numeric = false, .text = "Nominal",
                             .measurement = SLATE_MEASUREMENT_NONE},
        },
    };

    for (size_t i = 0; i < sizeof(SENSORS) / sizeof(SENSORS[0]); i++) {
        esp_err_t err = slate_state_publish("direct", &SENSORS[i]);
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

    lv_obj_t *anchor = activate_selftest_anchor();
    if (anchor == NULL) {
        UI_CHECK(false, "persistent heap baseline activated");
        return ESP_ERR_NO_MEM;
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
    UI_CHECK(direct_view != NULL && strcmp(lv_label_get_text(direct_view->name), "Living room") == 0 &&
                 strstr(lv_label_get_text(direct_view->detail), "ok") != NULL,
             "the direct-provider tile observed its state");
    UI_CHECK(fixture_view != NULL &&
                 strcmp(lv_label_get_text(fixture_view->name), "Fixture temperature") == 0 &&
                 strcmp(lv_label_get_text(fixture_view->sensor.value), "21.4") == 0 &&
                 strcmp(lv_label_get_text(fixture_view->sensor.unit), "°C") == 0,
             "the fixture tile observed the same normalized path");

    slate_config_t *sensors = sensor_test_config();
    UI_CHECK(sensors != NULL && rebuild_on_task(sensors) == ESP_OK,
             "sensor component test dashboard activated");
    if (sensors != NULL) {
        slate_config_free(sensors);
    }

    binding_view_t *power = find_view("direct", "power");
    binding_view_t *status = find_view("direct", "status");
    UI_CHECK(power != NULL && strcmp(lv_label_get_text(power->name), "Solar output") == 0 &&
                 strcmp(lv_label_get_text(power->sensor.value), "direct:power") == 0,
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
    UI_CHECK(sensor_view_text("pressure", "1013", "hPa"),
             "pressure rounds to a readable whole value");
    UI_CHECK(sensor_view_text("power", "42.8", "W"),
             "power keeps useful precision and its unit");
    UI_CHECK(sensor_view_text("status", "Nominal", ""),
             "textual normalized sensor values render unchanged");

    binding_view_t *temperature = find_view("direct", "temperature");
    binding_view_t *humidity = find_view("direct", "humidity");
    binding_view_t *pressure = find_view("direct", "pressure");
    power = find_view("direct", "power");
    status = find_view("direct", "status");
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

    esp_err_t offline_err =
        slate_state_provider_set_status("direct", SLATE_PROVIDER_OFFLINE);
    if (offline_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(offline_err == ESP_OK && sensor_view_text("temperature", "-", "°C") &&
                 temperature != NULL &&
                 lv_obj_get_style_opa(temperature->tile, LV_PART_MAIN) == LV_OPA_50,
             "an offline provider renders its stale sensor as a dimmed dash");

    esp_err_t online_err =
        slate_state_provider_set_status("direct", SLATE_PROVIDER_ONLINE);
    if (online_err == ESP_OK) {
        slate_state_drain(discard_changed, NULL);
        update_all();
    }
    UI_CHECK(online_err == ESP_OK && sensor_view_text("temperature", "-12.4", "°C") &&
                 temperature != NULL &&
                 lv_obj_get_style_opa(temperature->tile, LV_PART_MAIN) == LV_OPA_COVER,
             "a sensor restores its value when the provider returns online");

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
