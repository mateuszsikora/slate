idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES slate_store esp_system
)

#
# Build-time knobs, same arrangement as the spike harnesses.
#
#   -DSLATE_STORE_SELFTEST=1   exercise the LittleFS configuration round-trip
#                              at boot. Off by default; see main.c for why it
#                              exists and when it goes away.
#
foreach(knob SLATE_STORE_SELFTEST)
    if(DEFINED ${knob})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${knob}=${${knob}})
    endif()
endforeach()
