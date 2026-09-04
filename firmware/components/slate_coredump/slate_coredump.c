/*
 * Slate — core dump retrieval. See include/slate_coredump.h for the
 * integration contract.
 *
 * DESIGN.md §11.3, §4.1, §4.3, §6.3.
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
#include "esp_timer.h"

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
#error "slate_coredump needs CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH " \
       "and CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF"
#endif

static const char *TAG = "coredump";

/*
 * Out of PSRAM where there is any, for §11.1's reason: the buffer only ever
 * reaches esp_partition_read() and httpd_resp_send_chunk(), neither of which
 * needs DMA or internal memory, and §6.2's internal budget belongs to the
 * display and the setup access point.
 */
#define CHUNK_BYTES 4096

/*
 * A wall-clock bound on the response, which §11.1 makes mandatory rather than
 * optional for anything that occupies the one HTTP task: "'does not answer for a
 * while' is a client away from 'does not answer'". The number is the OTA path's
 * own budget, and if 300 s is generous for a 6 MB upload it is generous for a
 * dump of at most 128 KB.
 *
 * What it bounds and what it does not, because the difference is not obvious.
 * A client that has stopped reading altogether is already bounded without this,
 * by the socket's send_wait_timeout — httpd_resp_send_chunk() fails when nothing
 * moves for 5 s. This bounds the other case: a link slow enough that the whole
 * transfer would outlast §11.2's 60-second health window and take a healthy
 * image down with it. What neither bounds is a client that accepts a few bytes
 * inside every send window, because that resets the socket timeout and
 * httpd_resp_send_chunk() does not return until its chunk is out. Bounding that
 * needs a send path that owns a deadline of its own, which is #13's channel;
 * it is stated here rather than pretended, and it costs a valid device token.
 */
#define SEND_BUDGET_US (300 * 1000000LL)

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
        return slate_api_refuse(req, "404 Not Found", "no_coredump");

    case ESP_ERR_INVALID_SIZE:
    case ESP_ERR_INVALID_CRC:
        /* S-3 watched the coredump writer log success after a failed write
         * (#14). Naming the case on the device is what keeps that lie off the
         * host, which would otherwise spend the evening on gdb. */
        ESP_LOGE(TAG, "core dump on flash is corrupt: %s", esp_err_to_name(err));
        return slate_api_refuse(req, "500 Internal Server Error", "corrupt_coredump");

    default:
        ESP_LOGE(TAG, "reading the core dump partition: %s", esp_err_to_name(err));
        return slate_api_refuse(req, "500 Internal Server Error", "coredump_read_failed");
    }
}

/* --- What is on flash --------------------------------------------------- */

/* ESP-IDF writes the total length of the dump into the first word of the
 * partition, so erased flash reads as this. */
#define BLANK_LENGTH 0xFFFFFFFF

/*
 * Resolved once and remembered, because the answer cannot change while the
 * device is running: the only writer of this partition is the panic handler, and
 * it does not return. The cost of asking is what makes that worth stating —
 * esp_core_dump_image_check() reads and checksums the whole dump in 32-byte
 * units, which is some 800 flash transactions for the 26 KB dump #14 measured
 * and four thousand at the 128 KB ceiling, each one a window with the cache and
 * interrupts off on both cores. Asking per request, and three times per boot,
 * was buying a constant over and over.
 */
static struct {
    bool resolved;
    esp_err_t status;
    const esp_partition_t *partition;
    size_t size;
} s_dump;

/**
 * Is there a dump, is it intact, and how big is it.
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
 * The offset within the partition is deliberately not returned: ESP-IDF writes a
 * dump at the partition base and esp_core_dump_image_get() reports that base
 * back, so an offset would be a variable that is always zero. esp_partition_read()
 * refuses a read that would run past the partition, which is the bound §6.3
 * wants and not something this has to re-derive.
 */
static esp_err_t dump_status(const esp_partition_t **out_partition, size_t *out_size)
{
    if (s_dump.resolved) {
        *out_partition = s_dump.partition;
        *out_size = s_dump.size;
        return s_dump.status;
    }

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    esp_err_t err;
    uint32_t claimed = 0;
    size_t address = 0;
    size_t size = 0;

    if (partition == NULL) {
        /* Not ESP_ERR_NOT_FOUND: a panel with no coredump partition can never
         * produce a dump, which is a flash layout fault (§6.3) and not the same
         * news as "nothing has crashed". */
        ESP_LOGE(TAG, "no coredump partition in the table");
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }

    err = esp_partition_read(partition, 0, &claimed, sizeof(claimed));
    if (err != ESP_OK) {
        goto done;
    }
    if (claimed == BLANK_LENGTH) {
        err = ESP_ERR_NOT_FOUND;
        goto done;
    }

    err = esp_core_dump_image_check();
    if (err != ESP_OK) {
        goto done;
    }
    err = esp_core_dump_image_get(&address, &size);

done:
    switch (err) {
    case ESP_OK:              /* a dump, and it checks out */
    case ESP_ERR_NOT_FOUND:   /* blank partition */
    case ESP_ERR_INVALID_SIZE:
    case ESP_ERR_INVALID_CRC: /* something is there and cannot be believed */
    case ESP_ERR_INVALID_STATE: /* no coredump partition in this flash layout */
        s_dump.resolved = true;
        s_dump.status = err;
        s_dump.partition = partition;
        s_dump.size = size;
        break;

    default:
        /* A failed flash transaction is the one outcome that can differ on a
         * second attempt, so it is not remembered: answering 500 for the rest of
         * the uptime because of one bad read would be worse than asking again. */
        break;
    }

    *out_partition = partition;
    *out_size = size;
    return err;
}

#ifdef SLATE_COREDUMP_SELFTEST
bool slate_coredump_partition_is_blank(void)
{
    const esp_partition_t *partition = NULL;
    size_t size = 0;
    return dump_status(&partition, &size) == ESP_ERR_NOT_FOUND;
}
#endif

/* --- Retrieval ---------------------------------------------------------- */

static esp_err_t coredump_handler(httpd_req_t *req)
{
    /* Resolved before a byte is sent, because the status line is the only place
     * this endpoint can say which of the three states it found. */
    const esp_partition_t *partition = NULL;
    size_t size = 0;
    esp_err_t err = dump_status(&partition, &size);
    if (err != ESP_OK) {
        return refuse_dump_status(req, err);
    }

    char *buffer = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        buffer = malloc(CHUNK_BYTES);
    }
    if (buffer == NULL) {
        /* One spelling of "the device is out of heap" for the whole API. */
        return slate_api_refuse(req, "500 Internal Server Error", "out_of_memory");
    }

    const int64_t budget_ends = esp_timer_get_time() + SEND_BUDGET_US;
    size_t sent = 0;
    while (sent < size) {
        size_t want = size - sent < CHUNK_BYTES ? size - sent : CHUNK_BYTES;
        err = esp_partition_read(partition, sent, buffer, want);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "core dump read at 0x%x: %s", (unsigned) sent, esp_err_to_name(err));
            break;
        }

        /*
         * The response is committed here rather than before the loop, and that
         * ordering is the difference between a documented refusal and a socket
         * that closes with nothing in it. esp_http_server buffers the status and
         * the headers until the first chunk, so while `sent` is zero nothing has
         * reached the wire and a failed read can still be answered as §4's
         * `coredump_read_failed` — which it could not if the 200 had already
         * gone out. Reachable rather than theoretical: the buffer above is PSRAM
         * by preference, and esp_flash_read needs an internal staging buffer for
         * a PSRAM destination, so a read can fail on a device short of internal
         * memory (§6.2).
         *
         * The bytes are the flash image as the panic handler wrote it: ESP-IDF's
         * header, the ELF, and the checksum. `esp-coredump --core-format raw` is
         * what reads that, which is also what `idf.py coredump-info` reads off a
         * cable — one artifact, not one per transport. The filename is for a
         * browser that followed the route out of curiosity;
         * tools/coredump/fetch.sh names its own file.
         */
        if (sent == 0) {
            httpd_resp_set_type(req, "application/octet-stream");
            httpd_resp_set_hdr(req, "Content-Disposition",
                               "attachment; filename=\"coredump.bin\"");
            ESP_LOGI(TAG, "sending %u B of core dump from %s", (unsigned) size,
                     partition->label);
        }

        if (httpd_resp_send_chunk(req, buffer, want) != ESP_OK) {
            ESP_LOGW(TAG, "client stopped reading after %u of %u B", (unsigned) sent,
                     (unsigned) size);
            break;
        }
        sent += want;

        if (sent < size && esp_timer_get_time() > budget_ends) {
            ESP_LOGW(TAG, "abandoning the response after %u of %u B — %lld s budget spent",
                     (unsigned) sent, (unsigned) size, SEND_BUDGET_US / 1000000);
            break;
        }
    }
    free(buffer);

    if (sent == 0) {
        /* Nothing was committed, so the whole vocabulary is still available. */
        return refuse_dump_status(req, err == ESP_OK ? ESP_FAIL : err);
    }

    /*
     * Past the first chunk a failure cannot be a §4 error document: the 200 and
     * part of the body have already gone. Abandoning the response without the
     * terminating chunk is what tells the client its file is incomplete — the
     * alternative, closing the stream cleanly, would hand esp-coredump a
     * truncated dump and let it decide the panel is confused rather than the
     * transfer.
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
 *
 * Deliberately not part of registering the route. This is the diagnostic that
 * works when nothing else does, and a boot whose HTTP server did not start is
 * exactly the boot that needs it — so main calls it with the rest of the boot
 * report rather than the endpoint carrying it as a side effect.
 */
void slate_coredump_report(void)
{
    const esp_partition_t *partition = NULL;
    size_t size = 0;
    esp_err_t err = dump_status(&partition, &size);
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

    /*
     * calloc, and the two "unknown"s below, because esp_core_dump_get_summary()
     * returns ESP_OK whether or not it found anything to fill the fields with:
     * the task name and program counter arrive only if a loadable segment
     * matches the crashed task's TCB, and the image SHA only if the dump carries
     * the note that holds it. A panic whose task stack was too broken to dump —
     * which is a stack overflow, i.e. one of the crashes this line exists for —
     * leaves both untouched. On malloc'd memory that prints heap contents as a
     * task name and compares heap contents against the running image; zeroed, it
     * is an empty string this can test for and say so.
     *
     * Off the stack rather than on it because the summary carries a backtrace
     * array, and a boot report is the one caller that can simply say less when
     * there is no heap for it.
     */
    esp_core_dump_summary_t *summary = calloc(1, sizeof(*summary));
    if (summary == NULL || esp_core_dump_get_summary(summary) != ESP_OK) {
        ESP_LOGW(TAG, "core dump on flash: %u B, no summary available — fetch it from GET "
                      SLATE_API_BASE_PATH "/coredump",
                 (unsigned) size);
        free(summary);
        return;
    }

    char task[sizeof(summary->exc_task) + sizeof("\"\"")];
    if (summary->exc_task[0] != '\0') {
        snprintf(task, sizeof(task), "\"%.*s\"", (int) sizeof(summary->exc_task),
                 summary->exc_task);
    } else {
        strlcpy(task, "unknown", sizeof(task));
    }

    char image[APP_ELF_SHA256_SZ + sizeof("NOT this firmware ()")];
    if (summary->app_elf_sha256[0] != '\0') {
        bool same = strncmp((const char *) summary->app_elf_sha256,
                            esp_app_get_elf_sha256_str(), APP_ELF_SHA256_SZ - 1) == 0;
        snprintf(image, sizeof(image), "%.*s (%s)", APP_ELF_SHA256_SZ - 1,
                 (const char *) summary->app_elf_sha256,
                 same ? "this firmware" : "NOT this firmware");
    } else {
        strlcpy(image, "unknown", sizeof(image));
    }

    /* A warning rather than an info line: a panel that has crashed at some point
     * is a panel with something to explain, and this is the line #13 streams to
     * a desk. */
    ESP_LOGW(TAG, "core dump on flash: %u B, task %s at pc 0x%08" PRIx32
                  ", image %s — GET " SLATE_API_BASE_PATH "/coredump",
             (unsigned) size, task, summary->exc_pc, image);
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

    return slate_api_register_uri(&coredump, SLATE_API_AUTH_DEVICE_TOKEN);
}
