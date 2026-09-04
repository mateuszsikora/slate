/*
 * Slate — the release update channel (§11.4).
 *
 * DESIGN.md §11.4 (the manifest, the daily check, installation on explicit
 * request), §4.1 (the three routes below are contract), §6.3 (the two
 * application slots and the partitions that survive an update), §12.
 *
 *
 * WHAT THIS COMPONENT IS FOR, AND WHAT IT IS NOT
 *
 * §11.1's `POST /ota/upload` is a development tool: a desk pushes an image at a
 * wall. This is the other direction — the panel learns that a release exists,
 * says so, and installs it when a person asks it to:
 *
 *     GET  /api/v1/update           what is running, what is offered, what happened
 *     POST /api/v1/update/check     look now instead of waiting for the daily check
 *     POST /api/v1/update/install   install the offered release, then reboot
 *
 * The two mechanisms share nothing but the OTA partitions, and that is
 * deliberate: slate_ota's endpoint has no manifest, no versioning and no TLS,
 * and giving it any of the three would make the development flash slower for
 * an audience that does not need it.
 *
 * **Nothing here reboots a panel on its own.** A check downloads a few hundred
 * bytes of JSON and changes what `GET /update` answers; that is all it does.
 * The install is a separate request, carries the version the person accepted,
 * and is the only thing in this component that writes flash. §11.4's own words
 * are the reason — a wall panel must not reboot itself mid-evening.
 *
 *
 * THE CHANNEL
 *
 * One URL, compiled in, pointing at the manifest the release workflow deploys
 * beside the browser flasher (`tools/ota/manifest.py`, `.github/workflows/
 * release.yml`):
 *
 *     {"version": "1.2.0", "board": "waveshare-s3-touch-7",
 *      "url": "https://.../firmware/1.2.0/slate.bin",
 *      "sha256": "…", "min_schema": 1}
 *
 * Override it for a private channel or a test server with
 *
 *     idf.py -DSLATE_UPDATE_MANIFEST_URL='"https://example.invalid/manifest.json"' build
 *
 * and pass an empty string to build a panel with no update channel at all: the
 * routes still answer, `manifest_url` is `null`, and no check is ever made. A
 * URL that is not `https://` becomes the same state at startup, with an error
 * in the boot log — the checksum in a manifest fetched over plain HTTP is worth
 * exactly what the network says it is, and so is the image URL beside it.
 *
 * An offer requires all four of: a `board` equal to §4.1's model id, a
 * `min_schema` no higher than the configuration schema this firmware supports,
 * a `version` newer than the running one, and an `https://` image URL. A
 * running version that is not `MAJOR.MINOR.PATCH[-suffix]` is a development
 * build, and every release is newer than one of those.
 *
 *
 * WHY THE DOWNLOAD IS NOT esp_https_ota()
 *
 * §11.4 names `esp_https_ota`, and this deviates from it for one reason worth
 * stating: that API never exposes the bytes it writes, so the manifest's
 * `sha256` can only be computed after `esp_https_ota_finish()` — which is the
 * call that moves the boot partition. Verifying a checksum after committing to
 * the image it describes is not verification; it is a rollback with extra
 * steps, and the window in between is a power cut away from booting an image
 * that was never checked.
 *
 * So the download is the same loop `esp_https_ota` runs internally — the same
 * `esp_http_client`, the same certificate bundle, `esp_ota_write()` on the far
 * side — with mbedtls hashing the stream on the way past. The comparison
 * happens while the target slot holds nothing anyone would boot, and
 * `esp_ota_set_boot_partition()` is reached only by an image whose bytes are
 * the bytes the manifest named.
 *
 * Three cheaper refusals come before it and cost nothing: the image header must
 * be an ESP32-S3 application image (§11.1 checks the same three fields for the
 * same reason), its `esp_app_desc` version must be the version the manifest
 * promised, and its length must be the `Content-Length` the server announced.
 *
 *
 * THREADING
 *
 * slate_update_init() registers the routes and starts one worker task. Every
 * network operation happens on that task and never on the HTTP server's, which
 * is the same constraint slate_ota.h describes from the other side: there is
 * one HTTP task, an upload owns it for the duration of an upload, and a TLS
 * handshake on it would stop the whole API for seconds at a time. Both POSTs
 * therefore answer `202 Accepted` immediately and `GET /update` is where the
 * outcome appears.
 *
 * State is behind a mutex and read by the HTTP task. Only one job runs at a
 * time; a second request while one is running is refused `409 busy`.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the three update routes and start the daily check.
 *
 * Call after slate_api_init(), like every other component that registers a
 * route. A panel built with an empty manifest URL registers the routes and
 * starts no task: `GET /update` still answers, and answers that there is no
 * channel.
 */
esp_err_t slate_update_init(void);

#ifdef __cplusplus
}
#endif
