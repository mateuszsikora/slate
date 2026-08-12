/* Slate — behavior shared by every semantic component (DESIGN.md §7.5). */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "slate_state.h"

#define SLATE_COMPONENT_PLACEHOLDER_MAX 128

/**
 * @brief Apply the action policy for a mode transition.
 *
 * Entering edit mode suppresses actions immediately. Leaving it keeps them
 * suppressed until slate_component_actions_restore_complete() confirms that
 * the persisted tree replaced any transient preview.
 */
void slate_component_actions_mode_set(bool edit);

/**
 * @brief Re-enable actions after the persisted presentation is restored.
 *
 * This is a compare-and-transition rather than an unconditional enable: if a
 * client re-entered edit mode while restoration was running, it does nothing.
 */
void slate_component_actions_restore_complete(void);

/** @brief Whether a component event may reach the provider-neutral action bus. */
bool slate_component_actions_enabled(void);

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
