#
# `esp_netif` is how `/info.network` learns whether #55's access point is up
# without keeping a second copy of that fact — see network_json().
#
idf_component_register(
    SRCS "slate_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_app_format esp_http_server esp_netif esp_timer json slate_display slate_state
             slate_store slate_theme slate_wifi
)

if(DEFINED SLATE_API_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SLATE_API_SELFTEST=${SLATE_API_SELFTEST})
endif()
