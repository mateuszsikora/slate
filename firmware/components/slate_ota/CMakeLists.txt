#
# `app_update` is esp_ota_ops. The two headers this component reads the arriving
# image with live apart: `bootloader_support` carries esp_app_format.h — the
# image header and its magic — and `esp_app_format` carries esp_app_desc.h.
# Both arrive through app_update anyway; they are named because they are
# included directly.
#
# `slate_api` is where the route and the token check live (§4.3) — this
# component never touches esp_http_server without going through it.
#
idf_component_register(
    SRCS "slate_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES slate_api app_update bootloader_support esp_app_format esp_partition esp_timer json
)
