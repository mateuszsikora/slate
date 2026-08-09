#
# `esp_lcd` is here for `esp_lcd_new_panel_io_i2c_v1()` — the touch driver takes
# a panel IO handle rather than a bus, which is what lets the controller share
# the legacy I²C stack the CH422G forces on this board (#7). `driver` is that
# bus and the interrupt GPIO. LVGL and the GT911 driver arrive through
# idf_component.yml at pinned versions.
#
# slate_display is deliberately absent: the bus and the expander arrive as
# arguments, so the dependency runs one way and the display can bring touch up
# without touch knowing what a display is.
#
idf_component_register(
    SRCS "slate_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd driver esp_timer
)
