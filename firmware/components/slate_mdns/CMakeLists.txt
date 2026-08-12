#
# `slate_api` for the model id and the API base path the service advertises,
# `slate_store` for the device name, `esp_app_format` for the running version.
# The responder itself is the managed component pinned in idf_component.yml.
#
idf_component_register(
    SRCS "slate_mdns.c"
    INCLUDE_DIRS "include"
    REQUIRES slate_api slate_store esp_app_format espressif__mdns
)
