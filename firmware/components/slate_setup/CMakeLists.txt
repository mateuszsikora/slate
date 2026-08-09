#
# `esp_wifi` and `esp_netif` are the second interface and its DHCP server;
# `lwip` is the captive DNS responder's socket. `slate_api` is where the routes
# and §4.3's token exception live — this component never touches
# esp_http_server without going through it. `slate_display` is private because
# only this implementation presents setup state; slate_setup.h exposes none of
# its types.
#
idf_component_register(
    SRCS "slate_setup.c" "slate_setup_api.c" "slate_setup_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES slate_api slate_store slate_wifi esp_wifi esp_netif esp_event esp_timer lwip json
    PRIV_REQUIRES slate_display
)

#
# The setup page, compressed at build time and linked into the image (§9.2: it
# has to work on a device that has never had a filesystem, and LittleFS is what a
# bad OTA or a first flash is most likely to leave empty).
#
# Generated rather than committed: a .gz in the repository is a second copy of
# the page that reviews as binary and goes stale silently. portal/gzip.cmake is
# also where §9.2's 24 KB budget is enforced, so the number is measured by the
# build on every change to the page rather than by somebody remembering.
#
set(portal_source "${COMPONENT_DIR}/portal/portal.html")
set(portal_gzipped "${CMAKE_CURRENT_BINARY_DIR}/portal.html.gz")

add_custom_command(
    OUTPUT "${portal_gzipped}"
    COMMAND "${CMAKE_COMMAND}"
            -D "SOURCE=${portal_source}"
            -D "OUTPUT=${portal_gzipped}"
            -D "BUDGET=24576"
            -P "${COMPONENT_DIR}/portal/gzip.cmake"
    MAIN_DEPENDENCY "${portal_source}"
    DEPENDS "${COMPONENT_DIR}/portal/gzip.cmake"
    COMMENT "Compressing the setup page"
    VERBATIM)

# The symbols are _binary_portal_html_gz_start and _binary_portal_html_gz_end,
# which slate_setup_api.c declares.
target_add_binary_data(${COMPONENT_LIB} "${portal_gzipped}" BINARY
                       DEPENDS "${portal_gzipped}")

#
#   -DSLATE_SETUP_SELFTEST=1   check §4.3's token exception from the device, on
#                              the access point's own interface, every time the
#                              access point comes up. Off by default; see the
#                              bottom of slate_setup_api.c for why it can only be
#                              done from there.
#
if(DEFINED SLATE_SETUP_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SLATE_SETUP_SELFTEST=${SLATE_SETUP_SELFTEST})
endif()
