#
# `bootloader_support` is here for bootloader_random_enable() — see the entropy
# note in slate_store.c. It is not a bootloader build; the header is documented
# as callable from app code when the RF subsystem has not started yet.
#
# `mbedtls` supplies mbedtls_ct_memcmp() for the token comparison and
# mbedtls_sha256() for the log fingerprint. Both are things worth not
# hand-rolling: a constant-time loop written here would have to be re-audited on
# every toolchain bump, and upstream's carries the barriers and the tests.
#
idf_component_register(
    SRCS "slate_store.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_hw_support bootloader_support mbedtls
)
