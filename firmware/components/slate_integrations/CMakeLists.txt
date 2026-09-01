idf_component_register(
    SRCS "slate_integrations.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json slate_api slate_store slate_ws
)
