#
# The first provider that reaches a device by itself (ADR-3). Its dependency
# list is the argument for how cheap an integration is once the transports are
# in the image: `esp_http_client` is already linked for the release channel and
# `json` for the configuration parser, so nothing here is paid for twice and
# there is no vendor SDK, broker client or cloud library in it.
#
idf_component_register(
    SRCS "slate_shelly.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_http_client json slate_action slate_state slate_wifi
)
