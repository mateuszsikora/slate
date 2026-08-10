idf_component_register(
    SRCS "slate_ws.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_timer json slate_api slate_display slate_state slate_store
             slate_wifi
)

if(DEFINED SLATE_WS_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SLATE_WS_SELFTEST=${SLATE_WS_SELFTEST})
endif()
