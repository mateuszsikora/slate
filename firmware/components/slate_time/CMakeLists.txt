#
# `slate_time_zones.inc` is included by slate_time.c out of the component
# directory, which is already on the include path for the source's own
# directory — it is listed here so a reconfigure notices when the generator
# rewrites it.
#
# `slate_wifi` is a dependency in the direction that matters: the clock starts
# on association (§9.4), and the station has no business knowing what a clock
# is.
#
idf_component_register(
    SRCS "slate_time.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES slate_wifi esp_netif esp_event freertos
)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/slate_time_zones.inc")
