idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES slate_api slate_ota slate_setup slate_store slate_wifi slate_time app_update
             esp_system esp_event
)

#
# Build-time knobs, same arrangement as the spike harnesses.
#
#   -DSLATE_STORE_SELFTEST=1   exercise the LittleFS configuration round-trip
#                              at boot. Off by default; see main.c for why it
#                              exists and when it goes away.
#   -DSLATE_API_SELFTEST=1     exercise the real loopback HTTP server without
#                              logging the device token. Off by default.
#   -DSLATE_OTA_ROLLBACK_SELFTEST=1
#                              panic before API/network startup, but only when
#                              the running image is pending verification. OTA
#                              this build to prove unattended rollback; a serial
#                              flash or the restored image does not panic.
#
#   -DSLATE_SETUP_AP_PASSWORD='"..."'  §9.2's optional WPA2 passphrase for the
#                                      setup access point, which lives in NVS and
#                                      has no writer until #35 puts the field on
#                                      the page. Quoted twice on purpose — the
#                                      shell eats one layer and the compiler needs
#                                      a string literal. Never given a default
#                                      here: §12 keeps passphrases out of the
#                                      repository.
#
#   -DSLATE_TIMEZONE='"Europe/Warsaw"'  the §3.3 setting, until #19 parses the
#                                       configuration it really lives in.
#
# The station credentials used to arrive the same way. They do not any more —
# `POST /wifi` and the setup page are what M1 exists to replace that with.
#
foreach(knob SLATE_STORE_SELFTEST SLATE_API_SELFTEST SLATE_OTA_ROLLBACK_SELFTEST
             SLATE_SETUP_AP_PASSWORD SLATE_TIMEZONE)
    if(DEFINED ${knob})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${knob}=${${knob}})
    endif()
endforeach()
