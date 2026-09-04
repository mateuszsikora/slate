/*
 * The backing allocation for LVGL's built-in heap (DESIGN.md §6.2).
 *
 * This header is included while the managed LVGL component is compiled; see
 * firmware/CMakeLists.txt. It intentionally contains no Slate component types
 * so it remains safe at that side of the dependency boundary.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *slate_display_lvgl_pool_alloc(size_t size);

#ifdef __cplusplus
}
#endif
