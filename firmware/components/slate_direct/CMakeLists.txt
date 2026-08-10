#
# The reference provider (§5.4). Its dependency list is the second half of the
# claim `slate_state`'s makes: the neutral core names no transport, and the
# first adapter built on it names no automation system. There is no Home
# Assistant here, no MQTT and no broker client — only the device's own API, its
# WebSocket and the store.
#
# `slate_api` is where the route and the token check live (§4.3); this component
# never touches esp_http_server without going through it.
#
idf_component_register(
    SRCS "slate_direct.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_timer json slate_api slate_state slate_ws
)

if(DEFINED SLATE_DIRECT_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SLATE_DIRECT_SELFTEST=${SLATE_DIRECT_SELFTEST})
endif()
