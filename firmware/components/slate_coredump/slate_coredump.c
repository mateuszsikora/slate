/*
 * Slate — core dump retrieval. See include/slate_coredump.h for the
 * integration contract.
 *
 * design.md §11.3, §4.1, §4.3, §6.3.
 */

#include "slate_coredump.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "slate_api.h"

/*
 * A dump nobody can read is the failure this endpoint exists to prevent, so it
 * is a build error rather than a 404 nobody understands. `firmware/sdkconfig` is
 * gitignored and ESP-IDF consults SDKCONFIG_DEFAULTS only when no sdkconfig
 * exists (docs/spikes/s3.md), so the way this fires in practice is a working
 * tree that was configured before these two options landed in
 * sdkconfig.defaults: delete firmware/sdkconfig and reconfigure.
 */
#if !CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH || !CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#error "slate_coredump requires CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH and CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF"
#endif

static const char *TAG = "coredump";

/*
 * Out of PSRAM where there is any, for §11.1's reason: the buffer only ever
 * reaches esp_partition_read() and httpd_resp_send_chunk(), neither of which
 * needs DMA or internal memory, and §6.2's internal budget belongs to the
 * display and the setup access point. 4096 B matches the OTA path so that the
 * two transfers over the same one HTTP task behave the same way.
 */
#define CHUNK_BYTES 4096

/* --- Refusals ----------------------------------------------------------- */

static esp_err_t refuse(httpd_req_t *req, const char *status, const char *error)
{
    slate_api_send_error(req, status, error);

    /* The bound the rest of the API applies, for the same reason: a body no
     * handler read is drained 32 bytes at a time on the one HTTP task, and a
     * client that trickles owns it for as long as it likes. A GET here has no
     * body worth reading, so a request that brought one loses its socket. */
    return req->content_len > 0 ? ESP_FAIL : ESP_OK;
}

/**
 * Turn a dump_status() code into the answer §4.1's client acts on.
 *
 * The three cases are genuinely different things to a person holding a boot
 * loop: nothing has crashed, something crashed and the record of it is intact,
 * and something crashed and the record is not. The middle one is the only one
 * with a body.
 */
static esp_err_t refuse_dump_status(httpd_req_t *req, esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_FOUND:
        return refuse(req, "404 Not Found", "no_coredump");

    case ESP_ERR_INVALID_SIZE:
    case ESP_ERR_INVALID_CRC:
        /* S-3 watched the coredump writer log success after a failed write
         * (#14). Naming the case on the device is what keeps that lie off the
         * host, which would otherwise spend the evening on gdb. */
        ESP_LOGE(TAG, "core dump on flash is corrupt: %s", esp_err_to_name(err));
        return refuse(req, "500 Internal Server Error", "corrupt_coredump");

    default:
        ESP_LOGE(TAG, "reading the core dump partition: %s", esp_err_to_name(err));
        return refuse(req, "500 Internal Server Error", "coredump_read_failed");
    }
}

/* --- Retrieval ---------------------------------------------------------- */

/* ESP-IDF writes the total length of the dump into the first word of the
 * partition, so erased flash reads as this. */
#define BLANK_LENGTH 0xFFFFFFFF

/**
 * Is there a dump, is it intact, and where in the partition does it sit.
 *
 * This is the one place ESP-IDF's vocabulary is corrected, and the correction is
 * the whole reason the function exists. esp_core_dump_image_check() documents
 * ESP_ERR_NOT_FOUND for "no core dump is stored in the partition", but v5.5
 * returns that only when the partition itself is missing from the table: a blank
 * partition and a dump whose length field is nonsense both come back as
 * ESP_ERR_INVALID_SIZE. Those are opposite answers to the question a client is
 * asking — "has this panel ever crashed" — and one of them must not be a 500 on
 * a panel that has simply never crashed. Observed on this panel, which held
 * unrelated bytes at 0xFE0000 from before #5 gave that address to `coredump`.
 *
 * So the length field is read first, directly, and a blank one is reported as
 * ESP_ERR_NOT_FOUND. Everything past that point is a dump that claims to exist,
 * and the checksum decides whether it can be believed.
 *
 * esp_core_dump_image_get() then reports an absolute flash address, while
 * esp_partition_read() wants an offset into a partition. Going through the
 * partition table rather than reading raw flash is also what makes a dump
 * claiming to extend past the 128 KB of §6.3 a refusal instead of a read of
 * whatever follows it.
 */
static esp_err_t dump_status(const esp_partition_t **out_partition, size_t *out_offset,
                            size_t *out_size)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (partition == NULL) {
        /* Not ESP_ERR_NOT_FOUND: a panel with no coredump partition can never
         * produce a dump, which is a flash layout fault (§6.3) and not the same
         * news as "nothing has crashed". */
        ESP_LOGE(TAG, "no coredump partition in the table");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t claimed = 0;
    esp_err_t err = esp_partition_read(partition, 0, &claimed, sizeof(claimed));
    if (err != ESP_OK) {
        return err;
    }
    if (claimed == BLANK_LENGTH) {
        return ESP_ERR_NOT_FOUND;
    }

    err = esp_core_dump_image_check();
    if (err != ESP_OK) {
        /* The partition was there a moment ago, so a NOT_FOUND from here is not
         * the blank case and must not be answered as one. */
        return err == ESP_ERR_NOT_FOUND ? ESP_ERR_INVALID_STATE : err;
    }

    size_t address = 0;
    size_t size = 0;
    err = esp_core_dump_image_get(&address, &size);
    if (err != ESP_OK) {
        return err;
    }
    if (size == 0 || address < partition->address ||
        address - partition->address + size > partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_partition = partition;
    *out_offset = address - partition->address;
    *out_size = size;
    return ESP_OK;
}

bool slate_coredump_available(void)
{
    const esp_partition_t *partition = NULL;
    size_t offset = 0;
    size_t size = 0;
    return dump_status(&partition, &offset, &size) == ESP_OK;
}

static esp_err_t coredump_handler(httpd_req_t *req)
{
    /* Resolved before a byte is sent: the checksum is the difference between a
     * dump and 128 KB of erased flash, and the status line is the only place
     * this endpoint can still say which one it found. */
    const esp_partition_t *partition = NULL;
    size_t offset = 0;
    size_t size = 0;
    esp_err_t err = dump_status(&partition, &offset, &size);
    if (err != ESP_OK) {
        return refuse_dump_status(req, err);
    }

    char *buffer = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        buffer = malloc(CHUNK_BYTES);
    }
    if (buffer == NULL) {
        /* One spelling of "the device is out of heap" for the whole API. */
        slate_api_send_json(req, NULL);
        return ESP_FAIL;
    }

    /*
     * The bytes as the panic handler wrote them: ESP-IDF's header, the ELF, and
     * the checksum. `esp-coredump --core-format raw` is what reads that, which
     * is also what `idf.py coredump-info` reads off a cable — one artifact, not
     * one per transport. The filename is for a browser that followed the route
     * out of curiosity; tools/coredump/fetch.sh names its own file.
     */
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");

    ESP_LOGI(TAG, "sending %u B of core dump from %s+0x%x", (unsigned) size, partition->label,
             (unsigned) offset);

    size_t sent = 0;
    while (sent < size) {
        size_t want = size - sent < CHUNK_BYTES ? size - sent : CHUNK_BYTES;
        err = esp_partition_read(partition, offset + sent, buffer, want);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "core dump read at 0x%x: %s", (unsigned) (offset + sent),
                     esp_err_to_name(err));
            break;
        }
        if (httpd_resp_send_chunk(req, buffer, want) != ESP_OK) {
            ESP_LOGW(TAG, "client stopped reading after %u of %u B", (unsigned) sent,
                     (unsigned) size);
            break;
        }
        sent += want;
    }
    free(buffer);

    /*
     * A failure here cannot be a §4 error document: the 200 and part of the body
     * have already gone. Abandoning the response without the terminating chunk
     * is what tells the client its file is incomplete — the alternative, closing
     * the stream cleanly, would hand esp-coredump a truncated dump and let it
     * decide the panel is confused rather than the transfer.
     */
    if (sent < size) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* --- Boot report -------------------------------------------------------- */

/**
 * Say on every boot whether there is a crash to fetch, and whose crash it was.
 *
 * The image identity is the part that is easy to leave out and expensive to be
 * without. §11.2 rolls a panicking image back, so the firmware serving a dump is
 * routinely not the firmware that produced it, and symbolicating against the
 * wrong `.elf` produces a plausible backtrace through the wrong functions. The
 * SHA-256 prefix of the ELF is the same string ESP-IDF prints in its own panic
 * output, so the comparison is one a person can also make by eye.
 */
static void report_dump_on_flash(void)
{
    const esp_partition_t *partition = NULL;
    size_t offset = 0;
    size_t size = 0;
    esp_err_t err = dump_status(&partition, &offset, &size);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "no core dump on flash");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "core dump on flash is unreadable (%s) — GET " SLATE_API_BASE_PATH
                      "/coredump says so too",
                 esp_err_to_name(err));
        return;
    }

    /* Off the stack: this runs on the main task, and the summary carries a
     * backtrace array that is not free. A boot report is also the one caller
     * that can simply say less when there is no heap for it. */
    esp_core_dump_summary_t *summary = malloc(sizeof(*summary));
    if (summary == NULL || esp_core_dump_get_summary(summary) != ESP_OK) {
        ESP_LOGW(TAG, "core dump on flash: %u B, no summary available — fetch it from GET "
                      SLATE_API_BASE_PATH "/coredump",
                 (unsigned) size);
        free(summary);
        return;
    }

    const char *running = esp_app_get_elf_sha256_str();
    bool same_image = strncmp((const char *) summary->app_elf_sha256, running,
                              APP_ELF_SHA256_SZ - 1) == 0;

    /* A warning rather than an info line: a panel that has crashed at some point
     * is a panel with something to explain, and this is the line #13 streams to
     * a desk. */
    ESP_LOGW(TAG, "core dump on flash: %u B, task \"%.*s\" at pc 0x%08" PRIx32
                  ", image %.*s (%s) — GET " SLATE_API_BASE_PATH "/coredump",
             (unsigned) size, (int) sizeof(summary->exc_task), summary->exc_task,
             summary->exc_pc, APP_ELF_SHA256_SZ - 1, (const char *) summary->app_elf_sha256,
             same_image ? "this firmware" : "NOT this firmware");
    free(summary);
}

esp_err_t slate_coredump_init(void)
{
    /* §4.3 and §14: a crash dump carries stack contents and therefore whatever
     * was in them, so it is a token route like every other. The check is #10's
     * wrapper, not a copy here. */
    static const httpd_uri_t coredump = {
        .uri = SLATE_API_BASE_PATH "/coredump",
        .method = HTTP_GET,
        .handler = coredump_handler,
    };

    esp_err_t err = slate_api_register_uri(&coredump, SLATE_API_AUTH_DEVICE_TOKEN);
    if (err == ESP_OK) {
        report_dump_on_flash();
    }
    return err;
}
