idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES slate_api slate_store slate_wifi slate_time esp_system esp_event
)

#
# Build-time knobs, same arrangement as the spike harnesses.
#
#   -DSLATE_STORE_SELFTEST=1   exercise the LittleFS configuration round-trip
#                              at boot. Off by default; see main.c for why it
#                              exists and when it goes away.
#   -DSLATE_API_SELFTEST=1     exercise the real loopback HTTP server without
#                              logging the device token. Off by default.
#
#   -DSLATE_WIFI_SSID='"..."'      station credentials from the build, until
#   -DSLATE_WIFI_PASSWORD='"..."'  #55 serves a setup page that can be typed
#                                  into. Quoted twice on purpose — the shell
#                                  eats one layer and the compiler needs a
#                                  string literal. Never given a default here:
#                                  §12 keeps passphrases out of the repository.
#
#   -DSLATE_TIMEZONE='"Europe/Warsaw"'  the §3.3 setting, until #19 parses the
#                                       configuration it really lives in.
#
foreach(knob SLATE_STORE_SELFTEST SLATE_API_SELFTEST SLATE_WIFI_SSID SLATE_WIFI_PASSWORD
             SLATE_TIMEZONE)
    if(DEFINED ${knob})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${knob}=${${knob}})
    endif()
endforeach()
