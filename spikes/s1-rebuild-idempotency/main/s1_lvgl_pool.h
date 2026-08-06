/*
 * Backing allocator for the LVGL heap (design.md §6.2: 2 MB in PSRAM).
 *
 * Included by LVGL itself, not by application code — the project CMakeLists
 * points CONFIG_LV_MEM_POOL_INCLUDE at this header so that lv_mem_init() can
 * reach s1_lvgl_pool_alloc(). Keep it free of LVGL includes to avoid a cycle.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called once by lv_mem_init() with LV_MEM_SIZE. Returns NULL on failure, which
 * LVGL turns into an assert — a silent fallback to internal RAM would make the
 * whole measurement describe the wrong heap. */
void *s1_lvgl_pool_alloc(size_t size);

#ifdef __cplusplus
}
#endif
