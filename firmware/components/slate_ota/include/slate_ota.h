/*
 * Slate — development OTA (§11.1).
 *
 * design.md §11.1 (the endpoint), §4.1 (the route table), §4.3 and §14 (the
 * token applies to every write, development OTA included), §6.3 (the two
 * application slots this writes between).
 *
 *
 * WHAT THIS COMPONENT IS FOR
 *
 * One endpoint, so that the board can hang on a wall while development
 * continues from a desk:
 *
 *     POST /api/v1/ota/upload    body: the raw .bin, Authorization: Bearer
 *
 * The body streams into the inactive application partition, the boot partition
 * is switched, and the device reboots. Success answers before the reboot,
 * because after it there is nobody left to answer:
 *
 *     {"status": "ok", "partition": "ota_1", "bytes": 1157296,
 *      "version": "1.0.0-3-gd81fdc4", "reboot_in_ms": 500}
 *
 * `version` is the description read out of the image that has just been
 * written, not the one that is running — it is the answer to "did the file I
 * meant to send arrive", which is the question a development flash asks.
 *
 * Failures answer §4's `{"error": "..."}` with one of `empty_body`,
 * `too_large`, `not_an_image`, `truncated`, `invalid_image`, `no_ota_partition`
 * or `ota_failed`, and — apart from the last two — change nothing on the
 * device: the running partition still boots.
 *
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *
 * It does not mark the new image valid and it does not enable rollback. §11.2
 * is #12, and it lands as a boot health check in whatever owns the boot, not
 * as a flag flipped by the code that happened to write the image. Until it
 * lands, a firmware that boot-loops still costs a USB cable — which is why
 * §11.2 calls rollback a precondition for the scheme rather than hardening.
 *
 * It is not §11.4's release OTA: no manifest, no versioning, no HTTPS, no
 * daily check, no reboot the device decides on by itself.
 *
 *
 * THREADING
 *
 * slate_ota_init() is called once from app_main, after slate_api_init(). The
 * upload runs on the HTTP server's task and occupies it for the duration, so
 * the rest of the API does not answer while an image is arriving. That is
 * accepted rather than worked around: an upload is tens of seconds of a
 * development cycle, and a second worker would cost more DIRAM than #55 can
 * spare for its access point (§6.2).
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register `POST /api/v1/ota/upload`.
 *
 * Call after slate_api_init(). Returns what the registration returned; there
 * is no state to bring up beyond the route.
 */
esp_err_t slate_ota_init(void);

#ifdef __cplusplus
}
#endif
