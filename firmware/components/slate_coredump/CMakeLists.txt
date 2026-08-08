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
    REQUIRES slate_api espcoredump esp_app_format esp_partition
)
