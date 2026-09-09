#
# The second adapter that reaches a device by itself (ADR-3), and the first that
# is pushed to rather than polling. eISCP is a small binary frame over a plain
# TCP socket, so this needs lwIP and nothing else — no vendor SDK, no broker,
# not even an HTTP client. `slate_shelly` proved the provider boundary carries a
# poller; this one is here to prove it carries an event stream too.
#
idf_component_register(
    SRCS "slate_onkyo.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_timer lwip slate_action slate_state slate_wifi
)

if(DEFINED SLATE_ONKYO_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SLATE_ONKYO_SELFTEST=${SLATE_ONKYO_SELFTEST})
endif()
