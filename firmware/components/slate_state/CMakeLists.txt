#
# The provider-neutral core. Its dependency list is the design's claim in build
# form: no HTTP server, no WebSocket client, no cJSON, no LVGL. `esp_timer` is
# here for the timestamp on a snapshot and nothing else.
#
# A REQUIRES line that grows a transport is ADR-3 being lost, and this is the
# cheapest place to notice.
#
idf_component_register(
    SRCS "slate_state.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
)

if(DEFINED SLATE_STATE_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SLATE_STATE_SELFTEST=${SLATE_STATE_SELFTEST})
endif()
