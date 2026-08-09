#
# `esp_lcd` is here for the v2 I2C panel IO used by GT911. The current I2C bus
# and the CH422G handle arrive as borrowed arguments. LVGL and the GT911 driver
# arrive through idf_component.yml at pinned versions.
#
# slate_display is deliberately absent: the bus and the expander arrive as
# arguments, so the dependency runs one way and the display can bring touch up
# without touch knowing what a display is.
#
idf_component_register(
    SRCS "slate_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c esp_timer slate_ch422g
)
