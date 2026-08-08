#
# `espcoredump` is the flash reader and the summary parser; `esp_partition` is
# how the bytes are read out for the response, because the address
# esp_core_dump_image_get() reports is absolute and a partition is what bounds
# it. `esp_app_format` carries esp_app_desc.h, which is where the running
# image's ELF SHA-256 comes from — the boot report compares it against the one
# in the dump.
#
# `slate_api` is where the route and the token check live (§4.3) — this
# component never touches esp_http_server without going through it.
#
idf_component_register(
    SRCS "slate_coredump.c"
    INCLUDE_DIRS "include"
    REQUIRES slate_api espcoredump esp_app_format esp_partition esp_timer
)

#
#   -DSLATE_COREDUMP_SELFTEST=1   compile slate_coredump_partition_is_blank(),
#                                 which exists only for main's forced panic (see
#                                 firmware/main/CMakeLists.txt). Passed to this
#                                 component as well as to main, because the
#                                 declaration and the definition have to agree —
#                                 the same arrangement slate_api makes for its own
#                                 selftest.
#
if(DEFINED SLATE_COREDUMP_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
                               SLATE_COREDUMP_SELFTEST=${SLATE_COREDUMP_SELFTEST})
endif()
