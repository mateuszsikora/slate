/*
 * Slate — core dumps off the panel without a cable (§11.3).
 *
 * design.md §11.3 (the bullet this implements), §4.1 (`GET /coredump` in the
 * route table), §4.3 and §14 (the device token), §6.3 (the 128 KB `coredump`
 * partition and why it is that size).
 *
 *
 * WHAT THIS COMPONENT IS FOR
 *
 * §11.1 made flashing possible from a desk and §11.2 made a bad image
 * survivable. Neither says why the image was bad. A panic writes an ELF core
 * dump into the `coredump` partition, which survives the reboot and both
 * application slots, and one endpoint carries it to the host that has the
 * matching `.elf`:
 *
 *     GET /api/v1/coredump    Authorization: Bearer
 *
 *     200 application/octet-stream   the dump, for esp-coredump
 *     404 {"error": "no_coredump"}       nothing has crashed since the last erase
 *     500 {"error": "corrupt_coredump"}  a dump is there and its checksum is wrong
 *     500 {"error": "coredump_read_failed"}  flash would not read
 *     500 {"error": "out_of_memory"}     no buffer to stream it through
 *
 * The body is the flash image verbatim — ESP-IDF's 24-byte header, the ELF, and
 * the trailing checksum, exactly as the panic handler wrote them. That is what
 * `esp-coredump --core-format raw` takes, and it is the same artifact
 * `idf.py coredump-info` pulls over a cable, so the two paths do not produce two
 * formats to remember. tools/coredump/fetch.sh is the host side of it.
 *
 * A response that stops early is the one outcome with no error document, because
 * the 200 has already gone: the terminating chunk is absent, so the client sees a
 * transfer that failed rather than a dump that is short. §4.1 says the same.
 *
 * `corrupt_coredump` is a named answer rather than a body the host has to
 * diagnose, and S-3 is why. Its log showed `esp_core_dump_flash` reporting "Core
 * dump has been saved to flash" three lines after the write had failed
 * (docs/spikes/s3.md, quoted in #14). A dump whose checksum does not match is
 * the one case where serving the bytes anyway would let that lie travel to the
 * host, which spends its time on gdb instead.
 *
 * There is deliberately no DELETE. §4.1 does not have one, and a panic
 * overwrites the partition rather than appending to it, so nothing accumulates
 * and nothing has to be cleared before the next crash. It also keeps GET
 * repeatable, which matters when the first attempt is over WiFi at -83 dBm and
 * the panel is the only copy.
 *
 * `no_coredump` costs one read of the partition's first word, and that is not
 * belt-and-braces. esp_core_dump_image_check() documents ESP_ERR_NOT_FOUND for
 * "no core dump is stored in the partition", but ESP-IDF v5.5 returns it only
 * when the partition is absent from the table; a blank partition and a dump with
 * a nonsense length field are the same ESP_ERR_INVALID_SIZE. Trusting the
 * documented code turns a panel that has simply never crashed into a 500. See
 * dump_status() in the implementation.
 *
 *
 * THREADING AND COST
 *
 * slate_coredump_init() is called once from app_main, after slate_api_init().
 * The response is streamed on the HTTP server's task out of a heap buffer, which
 * follows §11.1's arrangement for the same reason: the one HTTP task is occupied
 * for the duration, and a dump is tens of kilobytes rather than a megabyte. It
 * carries §11.1's other half too — a wall-clock budget on the whole response, so
 * that a slow link cannot hold the API past §11.2's health deadline.
 *
 * Nothing writes the partition at runtime — only the panic handler does, and it
 * does not return — so no lock stands between a read and a write, and the state
 * of the partition is resolved once per boot and remembered. That matters more
 * than it looks: the integrity check reads and checksums the whole dump in
 * 32-byte units, which is hundreds of cache-disabling flash transactions per
 * call.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register `GET /api/v1/coredump`.
 *
 * Call after slate_api_init(). Returns what registration returned; a panel
 * whose endpoint could not be registered is degraded, not broken, and the
 * caller keeps booting.
 */
esp_err_t slate_coredump_init(void);

/**
 * @brief Log what the coredump partition holds: nothing, a crash, or a mess.
 *
 * Part of the boot report, and independent of slate_coredump_init() on purpose —
 * a boot whose HTTP server did not start is exactly the boot where this line is
 * the only account of the crash anybody will get. For a dump it names the crashed
 * task, the program counter and the ELF SHA-256 prefix of the image that produced
 * it, and whether that is the image now running. The last part is the one that
 * decides whether the `.elf` on the desk can symbolicate it at all: §11.2 rolls a
 * panicking image back, so the panel serving a dump is routinely NOT the panel
 * that produced it. When #13 lands, this is the first thing a remote log says
 * about a panel which came back from a crash.
 */
void slate_coredump_report(void);

#ifdef SLATE_COREDUMP_SELFTEST

/**
 * @brief Whether the coredump partition is blank — no dump has ever been written.
 *
 * "Blank", not "no readable dump": a corrupt dump means a panic did happen and
 * its record failed, which is both worth keeping and a reason not to crash on top
 * of it. main's forced-panic knob needs exactly that distinction, and it is the
 * difference between a knob that arms once and one that boot-loops a panel whose
 * dump write failed.
 */
bool slate_coredump_partition_is_blank(void);

#endif

#ifdef __cplusplus
}
#endif
