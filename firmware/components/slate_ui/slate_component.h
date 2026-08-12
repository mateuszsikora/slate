/* Slate — behavior shared by every semantic component (DESIGN.md §7.5). */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "slate_state.h"

#define SLATE_COMPONENT_PLACEHOLDER_MAX 128

/** Whether the resource replaces normal component content with an identity. */
bool slate_component_is_placeholder(slate_presentation_t presentation);

/**
 * Format §7.5's provider-qualified placeholder.
 *
 * Missing resources remain easy to locate in the editor. An incompatible
 * publication additionally names the expected and received semantic kinds,
 * which turns a warning border into a diagnosis.
 */
void slate_component_placeholder_text(const slate_resource_t *resource,
                                      char *out, size_t size);
