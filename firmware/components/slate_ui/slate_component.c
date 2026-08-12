/* Slate — behavior shared by every semantic component (DESIGN.md §7.5). */

#include "slate_component.h"

#include <stdio.h>

bool slate_component_is_placeholder(slate_presentation_t presentation)
{
    return presentation == SLATE_PRESENT_MISSING ||
           presentation == SLATE_PRESENT_MISSING_PROVIDER ||
           presentation == SLATE_PRESENT_INCOMPATIBLE;
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
