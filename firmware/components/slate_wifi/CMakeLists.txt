#
# `esp_wifi` and `esp_netif` are the station itself; `esp_event` carries both
# the driver's events in and this component's hand-off to #55 and #6 out.
#
# `slate_store` is public rather than private: slate_wifi.h spells its buffer
# sizes into slate_wifi_status_t, so anything that includes one includes the
# other.
#
idf_component_register(
    SRCS "slate_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES slate_store esp_wifi esp_netif esp_event esp_timer
)
