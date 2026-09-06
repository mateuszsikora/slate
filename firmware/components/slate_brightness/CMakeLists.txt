idf_component_register(
    SRCS "slate_brightness.c"
    INCLUDE_DIRS "include"
    REQUIRES slate_config slate_display slate_store slate_time slate_touch esp_timer
)

if(DEFINED SLATE_BRIGHTNESS_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
                               SLATE_BRIGHTNESS_SELFTEST=${SLATE_BRIGHTNESS_SELFTEST})
endif()
