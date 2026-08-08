cmake_minimum_required(VERSION 3.16)

#
# Slate firmware. One binary per board model (ADR-1); the dashboard arrives as
# JSON at runtime and nothing about it is compiled in.
#
# The project name decides the image name, and §11.1 fixes that: the host-side
# OTA helper is a `curl --data-binary @build/slate.bin`.
#
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# design.md §6.2 puts LVGL's fixed 2 MiB heap in PSRAM. LVGL's built-in
# allocator normally declares that pool as a static array, which would put it
# in internal RAM and fail long before reaching two megabytes. These definitions
# redirect the one pool allocation to slate_display. They have to be build-wide:
# the call site is compiled inside the managed LVGL component, not inside ours.
idf_build_set_property(
    COMPILE_DEFINITIONS
    "CONFIG_LV_MEM_POOL_INCLUDE=\"slate_display_lvgl_pool.h\""
    APPEND)
idf_build_set_property(
    COMPILE_DEFINITIONS
    "CONFIG_LV_MEM_POOL_ALLOC=slate_display_lvgl_pool_alloc"
    APPEND)
idf_build_set_property(
    COMPILE_OPTIONS
    "-I${CMAKE_CURRENT_LIST_DIR}/components/slate_display/include"
    APPEND)

project(slate)
