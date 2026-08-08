# `esp_lcd` owns the RGB peripheral and its PSRAM framebuffers; `driver` is the
# shared legacy I2C bus forced by the CH422G package (see #7). LVGL and the
# expander arrive through idf_component.yml at the exact versions S-2 booted.
idf_component_register(
    SRCS "slate_display.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd driver esp_timer esp_psram
)
