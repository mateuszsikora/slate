set(SLATE_FONT_DIR "${CMAKE_CURRENT_LIST_DIR}/assets")
set(SLATE_ICON_HEADER "${SLATE_FONT_DIR}/slate_icons.h")
set(SLATE_FONT_SRCS
    "${SLATE_FONT_DIR}/slate_font_hero_44.c"
    "${SLATE_FONT_DIR}/slate_font_body_20.c"
    "${SLATE_FONT_DIR}/slate_font_caption_15.c"
    "${SLATE_FONT_DIR}/slate_font_icons_28.c"
    "${SLATE_FONT_DIR}/slate_font_icons_48.c"
)

foreach(font ${SLATE_FONT_SRCS})
    if(NOT EXISTS "${font}")
        message(FATAL_ERROR
            "Missing generated Slate font ${font}. Run tools/fonts/generate.sh from the repository root.")
    endif()
endforeach()

if(NOT EXISTS "${SLATE_ICON_HEADER}")
    message(FATAL_ERROR
        "Missing generated Slate icon header ${SLATE_ICON_HEADER}. Run tools/fonts/generate.sh from the repository root.")
endif()

idf_component_register(
    SRCS "slate_theme.c" ${SLATE_FONT_SRCS}
    INCLUDE_DIRS "include" "assets"
    REQUIRES lvgl__lvgl
)
