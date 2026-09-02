idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES slate_action slate_api slate_brightness slate_config_api slate_control_api
             slate_coredump slate_direct slate_display slate_editor slate_ha slate_integrations
             slate_mdns slate_ota slate_setup slate_state slate_store slate_ui slate_wifi slate_time
             slate_ws app_update json
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
#   -DSLATE_STATE_SELFTEST=1   exercise the provider-neutral state store against
#                              fixture providers, before there is a transport or
#                              a tree to reach it through. Off by default; it
#                              goes away when #74 and #20 can drive the same
#                              paths from outside the device.
#   -DSLATE_DIRECT_SELFTEST=1  bind a fixture resource set, check the §5.4
#                              invariants that have no external surface, and
#                              hand an attaching action consumer one action to
#                              answer. The public configuration API supersedes
#                              its binding fixture for end-to-end verification;
#                              the component-level checks remain off by default.
#   -DSLATE_ACTION_SELFTEST=1  exercise capability validation, optimistic
#                              overlays and every completion path against the
#                              real state store and a fixture provider.
#   -DSLATE_CONFIG_API_SELFTEST=1
#                              check that only the exact transient query can
#                              select RAM-only replacement semantics. It does
#                              not write flash.
#   -DSLATE_HA_SELFTEST=1      exercise WebSocket URL/backoff rules, compressed
#                              entity diffs, and light/sensor/scene normalization.
#   -DSLATE_UI_SELFTEST=1      exercise the real declarative runtime on the
#                              panel, including 500 varied rebuild cycles.
#   -DSLATE_UI_LEAK_BYTES=64   negative control for the UI heap verifier. It is
#                              meaningful only together with UI_SELFTEST and
#                              must make that run fail.
#   -DSLATE_BRIGHTNESS_SELFTEST=1
#                              verify schedule boundaries, unsynced/setup
#                              behavior and dark-touch consumption at boot.
#   -DSLATE_TIME_SELFTEST=1    resolve Europe/Warsaw and verify both sides of
#                              its 2026 daylight-saving boundaries at boot.
#   -DSLATE_OTA_ROLLBACK_SELFTEST=1
#                              panic before API/network startup, but only when
#                              the running image is pending verification. OTA
#                              this build to prove unattended rollback; a serial
#                              flash or the restored image does not panic.
#   -DSLATE_COREDUMP_SELFTEST=1
#                              panic at the end of startup, but only when the
#                              coredump partition is empty. §11.3's forced crash:
#                              OTA this build and the panel writes a dump, rolls
#                              back to the previous slot, and serves the dump
#                              from there. Erasing the partition re-arms it.
#
#   -DSLATE_SETUP_AP_PASSWORD='"..."'  §9.2's optional WPA2 passphrase for the
#                                      setup access point, as a first-boot
#                                      provisioning override for #35's page.
#                                      Quoted twice on purpose — the
#                                      shell eats one layer and the compiler needs
#                                      a string literal. Never given a default
#                                      here: §12 keeps passphrases out of the
#                                      repository.
#
# The station credentials used to arrive the same way. They do not any more —
# `POST /wifi` and the setup page are what M1 exists to replace that with.
#
foreach(knob SLATE_STORE_SELFTEST SLATE_API_SELFTEST SLATE_WS_SELFTEST SLATE_STATE_SELFTEST
             SLATE_DIRECT_SELFTEST SLATE_ACTION_SELFTEST SLATE_CONFIG_API_SELFTEST SLATE_HA_SELFTEST
             SLATE_OTA_ROLLBACK_SELFTEST
             SLATE_UI_SELFTEST SLATE_UI_LEAK_BYTES SLATE_BRIGHTNESS_SELFTEST SLATE_TIME_SELFTEST
             SLATE_COREDUMP_SELFTEST SLATE_SETUP_AP_PASSWORD)
    if(DEFINED ${knob})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${knob}=${${knob}})
    endif()
endforeach()
