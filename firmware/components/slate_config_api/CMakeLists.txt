idf_component_register(
    SRCS "slate_config_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server slate_api slate_config slate_store slate_ui slate_ws
)

if(DEFINED SLATE_CONFIG_API_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
                               SLATE_CONFIG_API_SELFTEST=${SLATE_CONFIG_API_SELFTEST})
endif()
