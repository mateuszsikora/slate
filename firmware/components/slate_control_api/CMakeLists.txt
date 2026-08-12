idf_component_register(
    SRCS "slate_control_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_system esp_timer json slate_api slate_display slate_store slate_ws
)
