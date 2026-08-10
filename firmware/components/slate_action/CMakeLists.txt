#
# Provider-neutral semantic action bus (§5.3). It knows normalized state and
# timers, but no transport, JSON protocol, LVGL object or automation system.
# Adapters register the narrow dispatch callback in slate_action.h.
#
idf_component_register(
    SRCS "slate_action.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer slate_state
)

if(DEFINED SLATE_ACTION_SELFTEST)
    target_compile_definitions(
        ${COMPONENT_LIB} PRIVATE SLATE_ACTION_SELFTEST=${SLATE_ACTION_SELFTEST})
endif()
