/*
 * Slate — development OTA and boot health (§11.1, §11.2).
 *
 * DESIGN.md §11.1 (the endpoint), §11.2 (rollback), §4.1 (the route table),
 * §4.3 and §14 (the token applies to every write, development OTA included),
 * §6.3 (the two application slots this writes between).
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
 *     200 {"partition": "ota_1", "bytes": 1157296, "version": "1.0.0-3-gd81fdc4"}
 *
 * The 200 is what says the image was accepted and is about to boot; the body
 * carries only what the status cannot. `version` is the description read out
 * of the image that has just been written, not the one that is running — it is
 * the answer to "did the file I meant to send arrive", which is the question a
 * development flash asks. `partition` is the slot it went into, which is the
 * only handle a client has on which of the two it is looking at, and #12 will
 * want it. On an allocation failure the body degrades to `{}` rather than to
 * an error, because the outcome is in the status and the device is booting
 * either way.
 *
 * Failures answer §4's `{"error": "..."}` with one of `empty_body`,
 * `too_large`, `not_an_image`, `truncated`, `invalid_image`, `pending_verify`,
 * `no_ota_partition`, `ota_failed` or `out_of_memory`. None of them changes
 * what the device boots — `esp_ota_set_boot_partition()` is the last call
 * before the success answer, so any failure leaves the running partition
 * running.
 *
 * What a failure can cost is the OTHER slot, and the line is exactly
 * `esp_ota_begin()`. Refused before it, and nothing on the device is touched:
 * `empty_body`, `too_large`, `not_an_image`, `no_ota_partition`, and a
 * `truncated` that arrives inside the first 288 bytes. After it — a
 * `truncated` mid-upload, an `invalid_image` out of `esp_ota_end()` — the
 * target slot has already been erased and partly rewritten, because
 * `esp_ota_begin()` is given the Content-Length and erases that much up front.
 * #12 needs to know this: an interrupted flash is not a spare image to fall
 * back to.
 *
 * `pending_verify` is unreachable until #12 turns rollback on, and is then
 * what an upload gets while the running image is still unverified.
 *
 *
 * BOOT HEALTH BOUNDARY
 *
 * With rollback enabled, a newly uploaded image starts in PENDING_VERIFY. A
 * short-lived task waits for either the station or setup access point to own an
 * IPv4 address, then makes a real GET /api/v1/info request to that address. A
 * 200 marks the image valid; no answer within 60 seconds marks it invalid and
 * reboots into the previous slot. Home Assistant and station connectivity are
 * deliberately absent from that decision: a reachable setup AP can accept the
 * next OTA just as the station can.
 *
 * It is not §11.4's release OTA: no manifest, no versioning, no HTTPS, no
 * daily check, no reboot the device decides on by itself.
 *
 *
 * THREADING
 *
 * slate_ota_init() is called once from app_main, after slate_api_init(), to
 * register the upload route. slate_ota_start_boot_health() is called separately
 * after every API and network startup attempt, so an HTTP startup failure cannot
 * accidentally disarm the check whose job is to detect it. Only a pending image
 * creates the health task. The upload runs on the HTTP server's task and occupies
 * it for the duration, so the rest of the API does not answer while an image is
 * arriving. That is accepted rather than worked around: an upload is tens of
 * seconds of a development cycle, and a second HTTP worker would cost more DIRAM
 * than #55 can spare for its access point (§6.2).
 *
 * Accepted, but bounded — otherwise "does not answer for a while" is a client
 * away from "does not answer". Two wall-clock deadlines run over the request:
 * 30 s of silence, and 300 s for the whole body. The second one is what makes
 * a client that trickles a byte at a time terminate; a counter of consecutive
 * timeouts would not, because any single byte resets it.
 */

#pragma once

#include <stddef.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * How much of an image has to be in hand before it can be recognised: the image
 * header, the first segment header, and the application description ESP-IDF
 * places immediately after them.
 */
#define SLATE_OTA_IMAGE_PREFIX_BYTES \
    (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

/**
 * @brief Recognise the beginning of an application image for this board.
 *
 * @param prefix the first bytes of a candidate image
 * @param len    how many of them are in hand
 * @return the image's application description, or NULL when @p prefix is too
 *         short, is not an ESP-IDF image, or is an image for another chip.
 *
 * The returned pointer is into @p prefix and lives exactly as long as it does;
 * `version` is a fixed-size array that need not be NUL-terminated.
 *
 * Shared rather than duplicated because both ways an image reaches this device
 * — §11.1's upload and §11.4's release channel — must refuse the same three
 * things before a partition is erased, and two copies of that rule is one copy
 * that can be relaxed by accident. An ESP32 image on an ESP32-S3 is a boot
 * loop, and it is a plain mistake to make with two boards on one desk.
 */
const esp_app_desc_t *slate_ota_image_prefix(const void *prefix, size_t len);

/**
 * @brief Register `POST /api/v1/ota/upload`.
 *
 * Call after slate_api_init().
 */
esp_err_t slate_ota_init(void);

/**
 * @brief Start the §11.2 health check when this boot is pending verification.
 *
 * Call after the API and network have both attempted startup. The 60-second
 * deadline is measured from boot, not from this call. A normal or already-valid
 * image returns ESP_OK without creating a task.
 */
esp_err_t slate_ota_start_boot_health(void);

#ifdef __cplusplus
}
#endif
