#
# The provider-neutral LVGL runtime. Configuration and normalized state enter
# here; provider transports deliberately do not. All functions that touch LVGL
# execute through slate_display's single-owner task.
#
idf_component_register(
    SRCS "slate_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES slate_action slate_config slate_display slate_state slate_store slate_theme
             slate_time lvgl__lvgl
)

foreach(knob SLATE_UI_SELFTEST SLATE_UI_LEAK_BYTES)
    if(DEFINED ${knob})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${knob}=${${knob}})
    endif()
endforeach()
