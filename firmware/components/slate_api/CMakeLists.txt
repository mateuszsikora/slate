idf_component_register(
    SRCS "slate_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_app_format esp_http_server esp_timer json slate_store slate_wifi
)

if(DEFINED SLATE_API_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SLATE_API_SELFTEST=${SLATE_API_SELFTEST})
endif()
