# `esp_lcd` owns the RGB peripheral and its PSRAM framebuffers; `driver` is the
# shared legacy I2C bus forced by the CH422G package (see #7). LVGL and the
# expander arrive through idf_component.yml at the exact versions S-2 booted.
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
    REQUIRES esp_lcd driver esp_timer esp_psram
    PRIV_REQUIRES slate_touch
)
