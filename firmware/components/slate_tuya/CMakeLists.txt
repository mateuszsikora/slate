idf_component_register(
    SRCS "slate_tuya.c" "slate_tuya_client.c" "slate_tuya_map.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_http_client esp_http_server esp_timer json mbedtls
             slate_action slate_api slate_state slate_store slate_wifi
)

if(DEFINED SLATE_TUYA_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SLATE_TUYA_SELFTEST=${SLATE_TUYA_SELFTEST})
endif()
