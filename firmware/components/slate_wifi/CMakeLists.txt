#
# `esp_wifi` and `esp_netif` are the station itself; `esp_event` carries both
# the driver's events in and this component's hand-off to #55 and #6 out.
#
# `lwip` is §9.6's ARP proof. esp_netif has no interface for asking whether a
# host answers, and that question has to be answered before a static address is
# kept, so this reaches past it to etharp_request() — always through
# esp_netif_tcpip_exec(), never from this component's own task.
#
# `slate_store` is public rather than private: slate_wifi.h spells its buffer
# sizes and its slate_ipv4_config_t into slate_wifi_status_t and
# slate_wifi_connect(), so anything that includes one includes the other.
#
idf_component_register(
    SRCS "slate_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES slate_store esp_wifi esp_netif esp_event esp_timer lwip
)
