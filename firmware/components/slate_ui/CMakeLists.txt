#
# The provider-neutral LVGL runtime. Configuration and normalized state enter
# here; provider transports deliberately do not. All functions that touch LVGL
# execute through slate_display's single-owner task.
#
idf_component_register(
    SRCS "slate_ui.c" "slate_component.c" "slate_cover.c" "slate_light.c"
         "slate_scene.c" "slate_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES slate_action slate_brightness slate_config slate_display slate_state slate_store
             slate_theme slate_time lvgl__lvgl
)

if(DEFINED SLATE_UI_LEAK_BYTES AND NOT DEFINED SLATE_UI_SELFTEST)
    message(FATAL_ERROR "SLATE_UI_LEAK_BYTES is a self-test-only negative control")
endif()

foreach(knob SLATE_UI_SELFTEST SLATE_UI_LEAK_BYTES)
    if(DEFINED ${knob})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${knob}=${${knob}})
    endif()
endforeach()
