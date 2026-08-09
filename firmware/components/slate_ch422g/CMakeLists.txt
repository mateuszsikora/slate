idf_component_register(
    SRCS "slate_ch422g.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES freertos
)
