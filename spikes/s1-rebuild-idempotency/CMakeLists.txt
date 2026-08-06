cmake_minimum_required(VERSION 3.16)

#
# design.md §6.2 puts the LVGL heap at 2 MB in PSRAM. LVGL's built-in allocator
# places its pool in a static array, which at 2 MB cannot live in the 512 KB of
# internal SRAM — the link fails outright. LVGL's escape hatch is LV_MEM_POOL_ALLOC:
# when LV_MEM_ADR is 0 and that macro is defined, lv_mem_init() calls it instead
# of using the static array.
#
# There is no Kconfig entry for it (see managed_components/lvgl__lvgl/Kconfig —
# the memory menu stops at LV_MEM_ADR), but lv_conf_internal.h picks the macro up
# from CONFIG_LV_MEM_POOL_ALLOC, so it is set here as a build-wide definition.
# It has to be build-wide rather than component-local: the call site is inside
# the LVGL component, not inside main.
#
# idf_build_set_property() only exists once project.cmake has been included, so
# the include comes first and project() comes last.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(COMPILE_DEFINITIONS "CONFIG_LV_MEM_POOL_INCLUDE=\"s1_lvgl_pool.h\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CONFIG_LV_MEM_POOL_ALLOC=s1_lvgl_pool_alloc" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_LIST_DIR}/main" APPEND)

project(s1_rebuild_idempotency)
