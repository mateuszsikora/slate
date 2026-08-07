cmake_minimum_required(VERSION 3.16)

#
# design.md §6.2 puts the LVGL heap at 2 MB in PSRAM. LVGL's built-in allocator
# keeps its pool in a static array, which at 2 MB cannot live in internal SRAM,
# so the pool is handed over from PSRAM through LV_MEM_POOL_ALLOC. There is no
# Kconfig entry for that macro; lv_conf_internal.h picks it up from
# CONFIG_LV_MEM_POOL_ALLOC, and it has to be build-wide because the call site is
# inside the LVGL component rather than inside main. Same arrangement as S-1 —
# see spikes/s1-rebuild-idempotency/CMakeLists.txt in PR #46.
#
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(COMPILE_DEFINITIONS "CONFIG_LV_MEM_POOL_INCLUDE=\"s2_lvgl_pool.h\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CONFIG_LV_MEM_POOL_ALLOC=s2_lvgl_pool_alloc" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_LIST_DIR}/main" APPEND)

project(s2_render_performance)
