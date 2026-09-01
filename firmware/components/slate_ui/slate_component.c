/* Slate — behavior shared by every semantic component (DESIGN.md §7.5). */

#include "slate_component.h"

#include <stdio.h>
#include <stdatomic.h>

typedef enum {
    ACTIONS_NORMAL = 0,
    ACTIONS_EDIT,
    ACTIONS_RESTORING,
} actions_state_t;

static atomic_int s_actions_state = ATOMIC_VAR_INIT(ACTIONS_NORMAL);

void slate_component_actions_mode_set(bool edit)
{
    atomic_store_explicit(&s_actions_state, edit ? ACTIONS_EDIT : ACTIONS_RESTORING,
                          memory_order_release);
}

void slate_component_actions_restore_complete(void)
{
    int expected = ACTIONS_RESTORING;
    atomic_compare_exchange_strong_explicit(&s_actions_state, &expected, ACTIONS_NORMAL,
                                            memory_order_acq_rel, memory_order_acquire);
}

bool slate_component_actions_enabled(void)
{
    return atomic_load_explicit(&s_actions_state, memory_order_acquire) == ACTIONS_NORMAL;
}

bool slate_component_is_placeholder(slate_presentation_t presentation)
{
    return presentation == SLATE_PRESENT_MISSING ||
           presentation == SLATE_PRESENT_MISSING_PROVIDER ||
           presentation == SLATE_PRESENT_INCOMPATIBLE;
}

void slate_component_label_one_line(lv_obj_t *label)
{
    if (label == NULL) {
        return;
    }
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(label, lv_font_get_line_height(font));
}

void slate_component_placeholder_text(const slate_resource_t *resource,
                                      char *out, size_t size)
{
    if (out == NULL || size == 0) {
        return;
    }
    out[0] = '\0';
    if (resource == NULL) {
        return;
    }

    if (resource->presentation == SLATE_PRESENT_INCOMPATIBLE) {
        snprintf(out, size, "%s:%s\nExpected %s, got %s",
                 resource->provider, resource->resource,
                 slate_kind_str(resource->kind),
                 slate_kind_str(resource->mismatch_kind));
        return;
    }
    snprintf(out, size, "%s:%s", resource->provider, resource->resource);
}
