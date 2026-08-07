#
# `bootloader_support` is here for bootloader_random_enable() — see the entropy
# note in slate_store.c. It is not a bootloader build; the header is documented
# as callable from app code when the RF subsystem has not started yet, which is
# exactly when the device token is minted.
#
idf_component_register(
    SRCS "slate_store.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_hw_support bootloader_support
)
