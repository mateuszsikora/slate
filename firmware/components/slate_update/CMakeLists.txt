#
# §11.4's release channel. `esp_http_client` and `esp-tls`'s certificate bundle
# are the transport, `mbedtls` is the SHA-256 over the stream, and `app_update`
# is esp_ota_ops on the far side — the same three esp_https_ota composes
# internally, which is what include/slate_update.h explains this component for.
#
# `slate_ota` is here for one exported check: both ways an image can arrive have
# to refuse the same header before erasing a partition (SLATE_OTA_IMAGE_PREFIX).
# `slate_wifi` answers "is the station up" so a check made while the router is
# absent reports being offline rather than a mysterious TLS failure, and
# `slate_config` carries SLATE_CONFIG_SCHEMA_MAX, which is what a manifest's
# `min_schema` is compared against. `slate_store` holds one NVS key: whether
# this panel makes the daily check at all (#155). That is a setting and not a
# second build knob below, because the panels the browser flasher installs are
# exactly the ones nobody can rebuild.
#
idf_component_register(
    SRCS "slate_update.c"
    INCLUDE_DIRS "include"
    REQUIRES slate_api slate_config slate_ota slate_store slate_wifi
             app_update esp_app_format esp_http_client esp_partition esp_system esp_timer
             esp-tls freertos json mbedtls
)

#
#   -DSLATE_UPDATE_MANIFEST_URL='"https://example.invalid/manifest.json"'
#
#       Point this build at another channel, or at none with an empty string.
#       Quoted twice for the reason SLATE_SETUP_AP_PASSWORD is: the shell eats
#       one layer and the compiler needs a string literal. It is a build knob
#       rather than a setting because a manifest URL decides whose firmware a
#       panel installs — see include/slate_update.h.
#
if(DEFINED SLATE_UPDATE_MANIFEST_URL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
                               SLATE_UPDATE_MANIFEST_URL=${SLATE_UPDATE_MANIFEST_URL})
endif()
