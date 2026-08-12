#
# `slate_api` is where the route and §4.3's authentication policy live — this
# component never touches esp_http_server without going through it — and
# `slate_store` owns the filesystem the bundle is seeded onto. `mbedtls` is the
# SHA-256 that decides whether that seed is needed.
#
idf_component_register(
    SRCS "slate_editor.c"
    INCLUDE_DIRS "include"
    REQUIRES slate_api slate_store esp_timer mbedtls
)

#
# The editor bundle, built by Node and compressed into the image (see
# include/slate_editor.h for why it travels with the firmware). Generated
# rather than committed, like #21's fonts: a 200 KB artifact in the repository
# reviews as binary and goes stale silently.
#
# ../../cmake/gzip.cmake enforces §10's 400 KB budget on every build that
# touches the bundle, so the number is measured rather than remembered.
#
get_filename_component(editor_dir "${CMAKE_SOURCE_DIR}/../editor" ABSOLUTE)
set(bundle_source "${editor_dir}/dist/index.html")
set(bundle_gzipped "${CMAKE_CURRENT_BINARY_DIR}/editor.html.gz")
set(gzip_script "${CMAKE_SOURCE_DIR}/cmake/gzip.cmake")

if(NOT EXISTS "${bundle_source}")
    message(FATAL_ERROR
        "Missing editor bundle ${bundle_source}. Run tools/editor/build.sh from the "
        "repository root (it needs Node), or fetch the `editor` artifact from CI.")
endif()

add_custom_command(
    OUTPUT "${bundle_gzipped}"
    COMMAND "${CMAKE_COMMAND}"
            -D "SOURCE=${bundle_source}"
            -D "OUTPUT=${bundle_gzipped}"
            -D "BUDGET=409600"
            -D "LABEL=editor bundle"
            -P "${gzip_script}"
    MAIN_DEPENDENCY "${bundle_source}"
    DEPENDS "${gzip_script}"
    COMMENT "Compressing the editor bundle"
    VERBATIM)

# The symbols are _binary_editor_html_gz_start and _binary_editor_html_gz_end,
# which slate_editor.c declares.
target_add_binary_data(${COMPONENT_LIB} "${bundle_gzipped}" BINARY
                       DEPENDS "${bundle_gzipped}")
