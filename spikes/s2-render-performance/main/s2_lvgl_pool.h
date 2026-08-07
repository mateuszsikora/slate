/*
 * LVGL heap backing store — design.md §6.2 (2 MB in PSRAM).
 *
 * Wired in through CONFIG_LV_MEM_POOL_ALLOC from the top-level CMakeLists.txt.
 * See the comment there for why it cannot be a Kconfig setting.
 */
#pragma once

#include <stddef.h>

void *s2_lvgl_pool_alloc(size_t size);
