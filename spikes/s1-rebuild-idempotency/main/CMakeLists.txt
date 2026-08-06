idf_component_register(
    SRCS "s1_main.c"
    INCLUDE_DIRS "."
    REQUIRES json esp_timer
)

# Negative control: `idf.py -DS1_LEAK_BYTES=64 build` leaks that many bytes of
# LVGL heap per tile per cycle, which must turn the verdict red. See the header
# of s1_main.c for why a spike like this needs one.
if(DEFINED S1_LEAK_BYTES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE S1_LEAK_BYTES=${S1_LEAK_BYTES})
endif()
