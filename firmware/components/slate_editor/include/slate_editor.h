/*
 * Slate — the editor bundle.
 *
 * design.md §10 (the editor), §4.3 (how a browser gets to it), §6.3 (where it
 * lives), ADR-3 and ADR-4 (it is a client of the public API, not a privileged
 * part of the firmware).
 *
 *
 * WHY THE BUNDLE IS IN THE IMAGE AND ON THE FILESYSTEM BOTH
 *
 * §10 serves the editor from LittleFS and §6.3 budgets the partition for it.
 * Nothing writes that partition from outside, though: §4.1 has no file
 * endpoint, `POST /ota/upload` (§11.1) writes an application slot and §11.4's
 * release OTA does the same. A bundle that only ever lived on LittleFS would
 * therefore arrive by serial flash — the cable M1 exists to put away — and
 * would then stay at whatever version that flash left behind while the API it
 * talks to moved on underneath it. ADR-2 makes the opposite promise for the
 * component library: the design system ships over OTA.
 *
 * So the gzipped bundle is linked into the image, and the firmware writes it
 * to `/slate/www/index.html.gz` when the copy stored there is not the copy it
 * carries. What is served is the file, which keeps §10's meaning intact: a
 * later asset manager (§15) or file endpoint can replace it without a reflash,
 * and this component will leave a replacement alone until the image's own
 * bundle changes again.
 *
 * The image's copy is also the answer to the failure §9.2 names for the setup
 * page — "LittleFS is what a bad OTA or a first flash is most likely to leave
 * empty". A panel whose filesystem is empty or unwritable still serves the
 * editor; it just serves it from flash and says so in the boot log.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Seed the bundle onto LittleFS if needed and serve it at `GET /`.
 *
 * Call after slate_api_init() and after slate_store_init(). Registers the
 * editor half of `GET /` (slate_api_register_root); the setup page keeps the
 * access point (§9.2).
 *
 * Degraded rather than fatal on every storage failure: an editor that could
 * not be written is served out of the image, and a `GET /` that could not be
 * registered leaves a panel that is still on the network and still takes the
 * next firmware.
 */
esp_err_t slate_editor_init(void);

#ifdef __cplusplus
}
#endif
