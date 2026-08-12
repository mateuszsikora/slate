#
# Home Assistant is an adapter, never a dependency of the neutral store, action
# bus or UI runtime (ADR-3). Its two transports are private implementation
# details here: mDNS discovers an instance and esp_websocket_client owns the HA
# protocol connection. The responder those queries go out on belongs to
# slate_mdns, which advertises the panel itself (§4.3).
#
idf_component_register(
    SRCS "slate_ha.c" "slate_ha_actions.c" "slate_ha_catalog.c" "slate_ha_entities.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_http_server http_parser json mbedtls slate_action slate_api slate_mdns
             slate_state slate_store slate_wifi
             espressif__esp_websocket_client espressif__mdns
)

if(DEFINED SLATE_HA_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SLATE_HA_SELFTEST=${SLATE_HA_SELFTEST})
endif()
