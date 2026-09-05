# `esp_driver_ledc` is only reached in a build that has §16's backlight
# modification turned on (`Kconfig`), and is required unconditionally because a
# component's dependencies are not a Kconfig-time decision.
#
# `esp_lcd` owns the RGB peripheral and its PSRAM framebuffers. `slate_ch422g`
# and the GT911 share the current ESP-IDF I2C master bus created here. LVGL
# arrives through idf_component.yml at the exact version S-2 booted.
#
# `slate_touch` is required in this direction because the bus and the expander
# are brought up here and lent there — the display can exist without an input
# device, and does when the controller fails to answer. It is private because
# `slate_display.h` does not mention it: a public edge would put the GT911 and
# the expander headers on the include path of slate_api, slate_ws and main,
# none of which has any business reaching the expander.
idf_component_register(
    SRCS "slate_display.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_timer esp_psram slate_theme
    PRIV_REQUIRES esp_driver_i2c esp_driver_ledc slate_ch422g slate_touch
)

if(DEFINED SLATE_DISPLAY_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SLATE_DISPLAY_SELFTEST=${SLATE_DISPLAY_SELFTEST})
endif()
