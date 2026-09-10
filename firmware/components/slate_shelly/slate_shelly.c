/*
 * Slate — the Shelly integration provider. See include/slate_shelly.h.
 *
 * One task owns everything with a socket in it, and it owns the device tables
 * outright: nothing else reads or writes them, so there is no lock to take
 * across an HTTP call and no way for a rebuild to end up waiting on one.
 *
 * That is not a stylistic preference. §5.1 calls `subscribe()` on the binding
 * task, which is the LVGL task (§6.4), and both tasks run at priority 4 — so a
 * mutex this poller held across a sweep of unreachable relays would freeze the
 * screen and the touch panel for as long as the timeouts lasted. `slate_ha`
 * avoids the same trap by doing only short bookkeeping in its `subscribe()` and
 * handing the network work to its own task; this does the equivalent, with the
 * new tables built on the caller's stack and swapped in through a lock that is
 * never held for more than a few instructions.
 *
 * The action bus's dispatch callback does not make the HTTP call either — it
 * queues the command and returns, which is what §5.3 means by "returning
 * ESP_OK only acknowledges that the adapter took responsibility for reporting a
 * later result".
 */

#include "slate_shelly.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "slate_action.h"
#include "slate_state.h"
#include "slate_wifi.h"

static const char *TAG = "slate_shelly";

#define TASK_STACK       6144
#define TASK_PRIORITY    4
#define POLL_INTERVAL_MS 5000

/*
 * Two timeouts, because they answer to different deadlines. A sweep is only
 * racing the next sweep, but a command is racing §5.3's three-second revert:
 * the tile gives up at 3 s, and an adapter that took longer than that to fail
 * would have its `slate_action_result` discarded as late (`slate_action.c`) and
 * leave the relay switching after the UI already said it had not.
 *
 * The bound this actually buys, stated honestly: a command can still be delayed
 * by one in-flight sweep request before its own begins. With both at 2 s, a tap
 * on a *reachable* relay lands inside 3 s even if another device is timing out —
 * the sweep is also abandoned as soon as a command arrives, so it is one request
 * of delay and not one per device. Two unreachable devices in a row can exceed
 * the deadline, and in that case the action has genuinely failed and reverting
 * the tile is the correct outcome rather than a wrong one.
 */
#define POLL_HTTP_TIMEOUT_MS    2000
#define COMMAND_HTTP_TIMEOUT_MS 2000

/*
 * 1031 B is the largest real payload measured on a Plus 2PM answering
 * `Shelly.GetStatus` with both channels metered. 4 KB is that with room for a
 * four-channel device, and it is a hard ceiling rather than a growing buffer:
 * a body that does not fit is a device this adapter does not understand, and
 * truncating JSON produces a parse failure instead of a wrong reading.
 */
#define BODY_MAX 4096

/*
 * A host long enough to fit in a binding parses here. `slate_state_bind()`
 * accepts a resource id of SLATE_RESOURCE_ID_MAX, so any smaller ceiling would
 * reject an id the store had already accepted, and the only trace would be one
 * log line and a tile that never fills in.
 */
#define HOST_MAX          SLATE_RESOURCE_ID_MAX
#define COMMAND_QUEUE_LEN 8

/* --- The binding set, parsed ---------------------------------------------- */

typedef enum {
    ROLE_SWITCH = 0,   /**< the relay itself, published as a `light` */
    ROLE_POWER,        /**< instantaneous draw, W */
    ROLE_VOLTAGE,      /**< mains, V */
    ROLE_TEMPERATURE,  /**< the channel's own, °C */
} role_t;

typedef struct {
    char host[HOST_MAX + 1];
    uint8_t generation; /**< 0 until `GET /shelly` has answered, then 1 or 2 */
    bool reachable;
    /* Whether it has *ever* answered, which is a different question and decides
     * a different status: a relay that answered and went quiet is a loss, while
     * one that has never answered is usually an address nobody can reach. */
    bool ever_reachable;
} device_t;

/*
 * `ever_read` is load-bearing rather than bookkeeping. §5.2's snapshot has no
 * "no value" state — a sensor must carry a finite number or a non-empty word,
 * and `slate_state.c` refuses anything else — so a resource that has never been
 * read has nothing publishable to say. Publishing the zeroed struct would be
 * refused with `invalid_state` on every sweep, five seconds apart, for as long
 * as the binding existed. Not publishing at all leaves §3.3's placeholder
 * naming `provider:resource` on the tile, which is the accurate thing: this
 * binding has never produced a value.
 *
 * Once one has arrived, `last` is what an unavailable publication carries, so
 * §5.2's "an unavailable resource keeps its last values and renders stale"
 * holds — a complete snapshot replaces the previous one, and sending a zeroed
 * state with `available: false` would be claiming the light is off rather than
 * unknown.
 */
typedef struct {
    char id[SLATE_RESOURCE_ID_MAX + 1];
    uint16_t device;
    role_t role;
    uint8_t index;
    bool ever_read;
    slate_state_value_t last;
} entry_t;

typedef enum {
    CMD_SWEEP = 0, /**< a rebuild asking for a sweep now; carries nothing */
    CMD_ACTION,
} command_kind_t;

typedef struct {
    command_kind_t kind;
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    uint32_t action_id;
    bool toggle;
    bool power;
} command_t;

/* Owned by the poller task alone. No lock guards these, because nothing else
 * touches them. */
static device_t *s_devices;
static entry_t *s_entries;
static size_t s_device_count;
static size_t s_entry_count;
static char *s_body;

/* The handover. `s_bind_lock` is held for pointer moves and nothing else — in
 * particular never across an HTTP call, which is the whole point of it. */
static SemaphoreHandle_t s_bind_lock;
static StaticSemaphore_t s_bind_lock_storage;
static device_t *s_pending_devices;
static entry_t *s_pending_entries;
static size_t s_pending_device_count;
static size_t s_pending_entry_count;
static bool s_pending_valid;

/* Where the next sweep resumes, and whether every device has had one turn. Both
 * belong to the poller and both reset when a new binding set is adopted. */
static size_t s_cursor;
static bool s_swept;

static QueueHandle_t s_commands;
static TaskHandle_t s_task;
static volatile bool s_network_up;
static volatile bool s_has_bindings;
static bool s_initialized;

/* --- Resource ids --------------------------------------------------------- */

/*
 * `<host>/<role>:<index>`. The host half is passed to the HTTP client verbatim
 * and is not validated as an address here: an IP, an mDNS name and a typo are
 * indistinguishable to this parser, and the one that is a typo announces itself
 * as a device that never answers — which is a state the adapter has to render
 * correctly anyway.
 */
static bool parse_resource(const char *id, char *host, role_t *role, uint8_t *index)
{
    const char *slash = strchr(id, '/');
    if (slash == NULL || slash == id) {
        return false;
    }
    size_t host_len = (size_t)(slash - id);
    if (host_len > HOST_MAX) {
        return false;
    }
    memcpy(host, id, host_len);
    host[host_len] = '\0';

    const char *colon = strchr(slash + 1, ':');
    if (colon == NULL || colon == slash + 1) {
        return false;
    }
    size_t role_len = (size_t)(colon - slash - 1);
    static const struct {
        const char *name;
        role_t role;
    } names[] = {
        {"switch", ROLE_SWITCH},
        {"power", ROLE_POWER},
        {"voltage", ROLE_VOLTAGE},
        {"temperature", ROLE_TEMPERATURE},
    };
    bool matched = false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i].name) == role_len && strncmp(slash + 1, names[i].name, role_len) == 0) {
            *role = names[i].role;
            matched = true;
            break;
        }
    }
    if (!matched) {
        return false;
    }

    const char *digits = colon + 1;
    if (*digits < '0' || *digits > '9' || digits[1] != '\0') {
        return false; /* one digit: no Shelly in this family has ten channels */
    }
    *index = (uint8_t)(*digits - '0');
    return true;
}

/* --- HTTP ----------------------------------------------------------------- */

/** @brief GET one JSON document into the shared body buffer. Caller owns *out. */
static esp_err_t fetch_json(const char *host, const char *path, int timeout_ms, cJSON **out)
{
    char url[HOST_MAX + 64];
    int written = snprintf(url, sizeof(url), "http://%s%s", host, path);
    if (written <= 0 || (size_t)written >= sizeof(url)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);

    int length = esp_http_client_read_response(client, s_body, BODY_MAX - 1);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (length < 0 || status != 200) {
        return ESP_FAIL;
    }
    s_body[length] = '\0';

    cJSON *parsed = cJSON_Parse(s_body);
    if (parsed == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out = parsed;
    return ESP_OK;
}

/*
 * `GET /shelly` is the one route both generations answer, and `gen` appears
 * only on the newer one — a Plus 2PM reports `"gen": 2`, an SHSW-1 reports its
 * `type` and no such field. That is the whole discrimination.
 *
 * It is repeated whenever a device has gone unreachable, because "the same
 * address" and "the same device" are not the same claim: a relay replaced with
 * a newer model keeps the binding and changes the dialect, and a generation
 * cached for the lifetime of a configuration would answer for the old one until
 * somebody happened to republish the dashboard.
 */
static void probe_generation(device_t *device)
{
    cJSON *doc = NULL;
    if (fetch_json(device->host, "/shelly", POLL_HTTP_TIMEOUT_MS, &doc) != ESP_OK) {
        return;
    }
    const cJSON *gen = cJSON_GetObjectItemCaseSensitive(doc, "gen");
    device->generation = cJSON_IsNumber(gen) && gen->valueint >= 2 ? 2 : 1;
    ESP_LOGI(TAG, "%s is generation %u", device->host, device->generation);
    cJSON_Delete(doc);
}

/* --- Reading one device into snapshots ------------------------------------ */

static void publish_entry(entry_t *entry, bool available)
{
    if (!entry->ever_read) {
        return; /* nothing §5.2 would accept, and nothing true to say */
    }
    slate_snapshot_t snapshot = {
        .resource = entry->id,
        .kind = entry->role == ROLE_SWITCH ? SLATE_KIND_LIGHT : SLATE_KIND_SENSOR,
        .available = available,
        .state = entry->last,
    };
    if (entry->role == ROLE_SWITCH) {
        snapshot.capabilities.actions =
            (1u << SLATE_ACTION_TOGGLE) | (1u << SLATE_ACTION_SET_POWER);
        snapshot.state.light.brightness = SLATE_STATE_ABSENT;
        snapshot.state.light.color_temperature = SLATE_STATE_ABSENT;
    }
    esp_err_t err = slate_state_publish(SLATE_SHELLY_PROVIDER_ID, &snapshot);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "publish %s: %s", entry->id, esp_err_to_name(err));
    }
}

static void fill_sensor(entry_t *entry, double value, const char *unit,
                        slate_measurement_t measurement, slate_category_t category)
{
    slate_sensor_state_t *sensor = &entry->last.sensor;
    memset(sensor, 0, sizeof(*sensor));
    sensor->numeric = true;
    sensor->value = value;
    snprintf(sensor->unit, sizeof(sensor->unit), "%s", unit);
    sensor->measurement = measurement;
    sensor->category = category;
}

/** @brief Map one Gen2 `switch:N` object onto whichever role asked for it. */
static bool read_gen2(entry_t *entry, const cJSON *doc)
{
    char key[16];
    snprintf(key, sizeof(key), "switch:%u", entry->index);
    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(doc, key);
    if (!cJSON_IsObject(channel)) {
        return false;
    }
    switch (entry->role) {
    case ROLE_SWITCH: {
        const cJSON *output = cJSON_GetObjectItemCaseSensitive(channel, "output");
        if (!cJSON_IsBool(output)) {
            return false;
        }
        entry->last.light.on = cJSON_IsTrue(output);
        return true;
    }
    case ROLE_POWER: {
        const cJSON *power = cJSON_GetObjectItemCaseSensitive(channel, "apower");
        if (!cJSON_IsNumber(power)) {
            return false;
        }
        fill_sensor(entry, power->valuedouble, "W", SLATE_MEASUREMENT_POWER,
                    SLATE_CATEGORY_POWER);
        return true;
    }
    case ROLE_VOLTAGE: {
        const cJSON *volts = cJSON_GetObjectItemCaseSensitive(channel, "voltage");
        if (!cJSON_IsNumber(volts)) {
            return false;
        }
        fill_sensor(entry, volts->valuedouble, "V", SLATE_MEASUREMENT_NONE,
                    SLATE_CATEGORY_POWER);
        return true;
    }
    case ROLE_TEMPERATURE: {
        const cJSON *block = cJSON_GetObjectItemCaseSensitive(channel, "temperature");
        const cJSON *celsius = cJSON_GetObjectItemCaseSensitive(block, "tC");
        if (!cJSON_IsNumber(celsius)) {
            return false;
        }
        fill_sensor(entry, celsius->valuedouble, "°C", SLATE_MEASUREMENT_TEMPERATURE,
                    SLATE_CATEGORY_TEMPERATURE);
        return true;
    }
    }
    return false;
}

/** @brief The same for Gen1's flat `relays[]` / `meters[]` arrays. */
static bool read_gen1(entry_t *entry, const cJSON *doc)
{
    switch (entry->role) {
    case ROLE_SWITCH: {
        const cJSON *relays = cJSON_GetObjectItemCaseSensitive(doc, "relays");
        const cJSON *relay = cJSON_GetArrayItem(relays, entry->index);
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(relay, "ison");
        if (!cJSON_IsBool(on)) {
            return false;
        }
        entry->last.light.on = cJSON_IsTrue(on);
        return true;
    }
    case ROLE_POWER: {
        const cJSON *meters = cJSON_GetObjectItemCaseSensitive(doc, "meters");
        const cJSON *meter = cJSON_GetArrayItem(meters, entry->index);
        const cJSON *power = cJSON_GetObjectItemCaseSensitive(meter, "power");
        if (!cJSON_IsNumber(power)) {
            return false;
        }
        fill_sensor(entry, power->valuedouble, "W", SLATE_MEASUREMENT_POWER,
                    SLATE_CATEGORY_POWER);
        return true;
    }
    /* A Gen1 relay measures neither of these, so these bindings never produce a
     * value and their tiles keep §3.3's placeholder. */
    case ROLE_VOLTAGE:
    case ROLE_TEMPERATURE:
        return false;
    }
    return false;
}

static void poll_device(size_t device_index)
{
    device_t *device = &s_devices[device_index];
    if (device->generation == 0) {
        probe_generation(device);
        if (device->generation == 0) {
            goto unreachable;
        }
    }

    cJSON *doc = NULL;
    const char *path = device->generation >= 2 ? "/rpc/Shelly.GetStatus" : "/status";
    if (fetch_json(device->host, path, POLL_HTTP_TIMEOUT_MS, &doc) != ESP_OK) {
        goto unreachable;
    }
    device->reachable = true;
    device->ever_reachable = true;

    for (size_t i = 0; i < s_entry_count; i++) {
        entry_t *entry = &s_entries[i];
        if (entry->device != device_index) {
            continue;
        }
        bool read = device->generation >= 2 ? read_gen2(entry, doc) : read_gen1(entry, doc);
        if (read) {
            entry->ever_read = true;
        }
        /* A field this device does not carry — a `voltage` binding on a Shelly
         * 1 — stales exactly like a field that vanished from an otherwise
         * healthy answer, because from the tile's side they are the same fact:
         * this resource has no current value. */
        publish_entry(entry, read);
    }
    cJSON_Delete(doc);
    return;

unreachable:
    if (device->reachable) {
        ESP_LOGW(TAG, "%s stopped answering", device->host);
    }
    device->reachable = false;
    device->generation = 0; /* re-probe: the next answer may be a different device */
    for (size_t i = 0; i < s_entry_count; i++) {
        if (s_entries[i].device == device_index) {
            publish_entry(&s_entries[i], false);
        }
    }
}

/* --- Commands ------------------------------------------------------------- */

/*
 * Gen2 has `Switch.Toggle`, which is one request and cannot race a reading the
 * way a read-then-invert can. Gen1's `/relay/N?turn=toggle` is the same idea in
 * the older dialect, so neither generation needs the current state to flip it.
 */
static void run_command(const command_t *command)
{
    char host[HOST_MAX + 1];
    role_t role;
    uint8_t index;
    if (!parse_resource(command->resource, host, &role, &index) || role != ROLE_SWITCH) {
        slate_action_result(SLATE_SHELLY_PROVIDER_ID, command->action_id, false,
                            "unsupported_action");
        return;
    }

    size_t device_index = SIZE_MAX;
    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].host, host) == 0) {
            device_index = i;
            break;
        }
    }
    if (device_index == SIZE_MAX) {
        slate_action_result(SLATE_SHELLY_PROVIDER_ID, command->action_id, false, "not_bound");
        return;
    }
    if (s_devices[device_index].generation == 0) {
        probe_generation(&s_devices[device_index]);
        if (s_devices[device_index].generation == 0) {
            slate_action_result(SLATE_SHELLY_PROVIDER_ID, command->action_id, false,
                                "unreachable");
            return;
        }
    }

    char path[64];
    if (s_devices[device_index].generation >= 2) {
        if (command->toggle) {
            snprintf(path, sizeof(path), "/rpc/Switch.Toggle?id=%u", index);
        } else {
            snprintf(path, sizeof(path), "/rpc/Switch.Set?id=%u&on=%s", index,
                     command->power ? "true" : "false");
        }
    } else {
        snprintf(path, sizeof(path), "/relay/%u?turn=%s", index,
                 command->toggle ? "toggle" : (command->power ? "on" : "off"));
    }

    cJSON *doc = NULL;
    esp_err_t err = fetch_json(host, path, COMMAND_HTTP_TIMEOUT_MS, &doc);
    if (doc != NULL) {
        cJSON_Delete(doc);
    }
    if (err != ESP_OK) {
        slate_action_result(SLATE_SHELLY_PROVIDER_ID, command->action_id, false, "unreachable");
        return;
    }

    /* §5.3: the acknowledgement says the request was taken; the confirmation is
     * the next snapshot. Re-reading the device now rather than waiting out the
     * sweep is what keeps a tap from sitting pending for five seconds. */
    slate_action_result(SLATE_SHELLY_PROVIDER_ID, command->action_id, true, NULL);
    poll_device(device_index);
}

/* --- The one task --------------------------------------------------------- */

/*
 * A station and a binding set are not evidence that this provider is serving
 * anything. Reporting `online` on the strength of those two alone made the
 * worst configuration — every address wrong, nothing ever answered — look
 * exactly like a working one, and `GET /resources` could not correct the
 * impression, because a resource with no reading is one this adapter does not
 * publish at all. §5.2's vocabulary already distinguishes these:
 *
 *   connecting   bound, network up, no device has had its first turn yet
 *   online       every device answered the last time it was asked
 *   degraded     some did — §5.2's "can still serve part of its contract"
 *   offline      none do now, but some have; a loss, and it stales the tiles
 *   error        none ever has, which is a person's problem to go and look at
 */
static void update_status(void)
{
    slate_provider_status_t status;
    if (s_entry_count == 0) {
        status = SLATE_PROVIDER_UNCONFIGURED;
    } else if (!s_network_up) {
        status = SLATE_PROVIDER_OFFLINE;
    } else if (!s_swept) {
        status = SLATE_PROVIDER_CONNECTING;
    } else {
        size_t reachable = 0;
        size_t ever = 0;
        for (size_t i = 0; i < s_device_count; i++) {
            reachable += s_devices[i].reachable ? 1 : 0;
            ever += s_devices[i].ever_reachable ? 1 : 0;
        }
        if (reachable == s_device_count) {
            status = SLATE_PROVIDER_ONLINE;
        } else if (reachable > 0) {
            status = SLATE_PROVIDER_DEGRADED;
        } else if (ever > 0) {
            status = SLATE_PROVIDER_OFFLINE;
        } else {
            status = SLATE_PROVIDER_ERROR;
        }
    }
    slate_state_provider_set_status(SLATE_SHELLY_PROVIDER_ID, status);
}

/** @brief Take ownership of a binding set `subscribe()` left, if there is one. */
static bool adopt_pending(void)
{
    xSemaphoreTake(s_bind_lock, portMAX_DELAY);
    bool pending = s_pending_valid;
    device_t *devices = s_pending_devices;
    entry_t *entries = s_pending_entries;
    size_t device_count = s_pending_device_count;
    size_t entry_count = s_pending_entry_count;
    s_pending_valid = false;
    s_pending_devices = NULL;
    s_pending_entries = NULL;
    s_pending_device_count = 0;
    s_pending_entry_count = 0;
    xSemaphoreGive(s_bind_lock);

    if (!pending) {
        return false;
    }

    /*
     * Carry across what this adapter already knows about hosts and resources
     * the rebuild did not change. The tables arrive zeroed, and adopting them
     * as-is threw that away — which was not merely wasteful, because
     * `slate_state_bind()` deliberately carries `available` and the last value
     * across a rebuild too ("Carry the last known value across the rebuild").
     * A resource whose `ever_read` had just been reset publishes nothing when
     * its device stops answering, so the store kept a carried `available: true`
     * and the tile showed a live reading for an unplugged relay, permanently.
     *
     * The other two are cheaper but real: a rediscovered generation costs an
     * extra `GET /shelly` per host on every republish, and a reset `s_swept`
     * drops the provider to `connecting` for a whole sweep even when no Shelly
     * binding moved.
     */
    bool new_device = false;
    for (size_t d = 0; d < device_count; d++) {
        const device_t *previous = NULL;
        for (size_t o = 0; o < s_device_count; o++) {
            if (strcmp(s_devices[o].host, devices[d].host) == 0) {
                previous = &s_devices[o];
                break;
            }
        }
        if (previous == NULL) {
            new_device = true;
            continue;
        }
        devices[d].generation = previous->generation;
        devices[d].reachable = previous->reachable;
        devices[d].ever_reachable = previous->ever_reachable;
    }
    for (size_t e = 0; e < entry_count; e++) {
        for (size_t o = 0; o < s_entry_count; o++) {
            if (strcmp(s_entries[o].id, entries[e].id) == 0) {
                entries[e].ever_read = s_entries[o].ever_read;
                entries[e].last = s_entries[o].last;
                break;
            }
        }
    }

    free(s_devices);
    free(s_entries);
    s_devices = devices;
    s_entries = entries;
    s_device_count = device_count;
    s_entry_count = entry_count;
    s_has_bindings = entry_count > 0;
    s_cursor = 0;
    if (new_device) {
        /* Only a host nobody has asked yet makes the status a question again. */
        s_swept = false;
    }
    ESP_LOGI(TAG, "%u resources on %u devices", (unsigned)s_entry_count,
             (unsigned)s_device_count);
    update_status();
    return true;
}

/*
 * The cursor is what makes abandoning a sweep safe. Restarting at the first
 * device every time meant the last one was only ever read when nobody happened
 * to be tapping — somebody adjusting six lights every couple of seconds would
 * refresh device 0 on every pass and device 5 almost never, with the tiles at
 * the end of the list frozen until the tapping stopped. Resuming where the
 * previous pass stopped turns "read every device" into a promise the code
 * keeps, rather than one a comment made.
 */
static void sweep(void)
{
    if (s_device_count == 0) {
        return;
    }
    for (size_t n = 0; n < s_device_count; n++) {
        /* A tap is waiting and §5.3 gives it three seconds. Finishing the sweep
         * first would spend them on devices nobody is looking at. */
        if (uxQueueMessagesWaiting(s_commands) > 0) {
            break;
        }
        poll_device(s_cursor % s_device_count);
        s_cursor++;
    }
    if (s_cursor >= s_device_count) {
        s_swept = true; /* every device has had at least one turn */
    }
}

static void poller_task(void *arg)
{
    (void)arg;
    int64_t next_sweep_us = 0;
    for (;;) {
        if (adopt_pending()) {
            next_sweep_us = 0; /* a fresh dashboard should not wait out an interval */
        }

        int64_t remaining_us = next_sweep_us - esp_timer_get_time();
        TickType_t wait = remaining_us <= 0 ? 0 : pdMS_TO_TICKS(remaining_us / 1000);

        command_t command;
        if (xQueueReceive(s_commands, &command, wait) == pdTRUE) {
            if (command.kind == CMD_ACTION) {
                if (s_network_up && s_entry_count > 0) {
                    run_command(&command);
                } else {
                    slate_action_result(SLATE_SHELLY_PROVIDER_ID, command.action_id, false,
                                        "offline");
                }
            }
            continue; /* drain the queue before spending time on a sweep */
        }

        if (s_entry_count > 0 && s_network_up) {
            sweep();
        }
        /* After the sweep, not before it: the status is a statement about what
         * the devices just said, and reporting it first would always describe
         * the previous pass. */
        update_status();
        /* Measured from the end of the sweep, not from the last wake, so a busy
         * few seconds of tapping cannot starve the sweep or make it run back to
         * back once the tapping stops. */
        next_sweep_us = esp_timer_get_time() + (int64_t)POLL_INTERVAL_MS * 1000;
    }
}

/* --- Provider callbacks --------------------------------------------------- */

/*
 * §5.1's second operation, and this adapter's entire configuration.
 *
 * This runs on the LVGL task (§6.4). Everything expensive about it — parsing
 * and allocation — happens before the lock, and the lock itself covers five
 * pointer moves. The tables are rebuilt whole rather than diffed: a rebuild is
 * rare, the sets are small, and a diff would have to decide what a host losing
 * its last resource means about a probe already in flight.
 */
static esp_err_t subscribe(void *ctx, const char *const *resources, size_t count)
{
    (void)ctx;
    device_t *devices = NULL;
    entry_t *entries = NULL;
    size_t device_count = 0;
    size_t entry_count = 0;

    if (count > 0) {
        entries = heap_caps_calloc(count, sizeof(entry_t), MALLOC_CAP_SPIRAM);
        devices = heap_caps_calloc(count, sizeof(device_t), MALLOC_CAP_SPIRAM);
        if (entries == NULL || devices == NULL) {
            free(entries);
            free(devices);
            return ESP_ERR_NO_MEM;
        }
    }

    for (size_t i = 0; i < count; i++) {
        char host[HOST_MAX + 1];
        role_t role;
        uint8_t index;
        if (!parse_resource(resources[i], host, &role, &index)) {
            /* Not an error the rebuild can act on (§5.1). The binding renders
             * as a resource that never arrived, which is what it is. */
            ESP_LOGW(TAG, "ignoring unparseable resource id %s", resources[i]);
            continue;
        }
        size_t device = SIZE_MAX;
        for (size_t d = 0; d < device_count; d++) {
            if (strcmp(devices[d].host, host) == 0) {
                device = d;
                break;
            }
        }
        if (device == SIZE_MAX) {
            device = device_count++;
            snprintf(devices[device].host, sizeof(devices[device].host), "%s", host);
        }
        entry_t *entry = &entries[entry_count++];
        snprintf(entry->id, sizeof(entry->id), "%s", resources[i]);
        entry->device = (uint16_t)device;
        entry->role = role;
        entry->index = index;
    }

    xSemaphoreTake(s_bind_lock, portMAX_DELAY);
    /* A set the poller never got to is replaced rather than queued: only the
     * newest configuration is the configuration. */
    free(s_pending_devices);
    free(s_pending_entries);
    s_pending_devices = devices;
    s_pending_entries = entries;
    s_pending_device_count = device_count;
    s_pending_entry_count = entry_count;
    s_pending_valid = true;
    xSemaphoreGive(s_bind_lock);

    /* Best effort, and correctness does not depend on it: the poller adopts the
     * set at the top of its next pass regardless. This only decides whether
     * that happens now or up to one interval from now. */
    if (s_commands != NULL) {
        const command_t poke = {.kind = CMD_SWEEP};
        xQueueSend(s_commands, &poke, 0);
    }
    return ESP_OK;
}

static esp_err_t dispatch(void *ctx, uint32_t id, const slate_action_request_t *request)
{
    (void)ctx;
    /* The provider registers in init() and the queue is created in start(), so
     * a panel whose poller failed to start has a clickable tile and no consumer
     * behind it. Refusing here turns that into §5.3's immediate revert instead
     * of a FreeRTOS assertion on the first tap. */
    if (s_commands == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (request->action != SLATE_ACTION_TOGGLE && request->action != SLATE_ACTION_SET_POWER) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->action == SLATE_ACTION_SET_POWER &&
        request->value_type != SLATE_ACTION_VALUE_BOOL) {
        return ESP_ERR_INVALID_ARG;
    }

    command_t command = {
        .kind = CMD_ACTION,
        .action_id = id,
        .toggle = request->action == SLATE_ACTION_TOGGLE,
        .power = request->value_type == SLATE_ACTION_VALUE_BOOL && request->value.boolean,
    };
    snprintf(command.resource, sizeof(command.resource), "%s", request->resource);

    /* Never block the bus: the queue is short on purpose, and a full one is a
     * device already several taps behind rather than something to wait for. */
    if (xQueueSend(s_commands, &command, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == SLATE_WIFI_EVENT_CONNECTED) {
        s_network_up = true;
    } else if (id == SLATE_WIFI_EVENT_DISCONNECTED) {
        s_network_up = false;
        slate_action_provider_unavailable(SLATE_SHELLY_PROVIDER_ID, "offline");
    } else {
        return;
    }
    /* A loss stales this provider's resources at once rather than at the end of
     * the current interval; the recovery is left to the task, which is the one
     * that knows whether anything is bound to come back online for. */
    if (!s_network_up && s_has_bindings) {
        slate_state_provider_set_status(SLATE_SHELLY_PROVIDER_ID, SLATE_PROVIDER_OFFLINE);
    }
}

/* --- Lifecycle ------------------------------------------------------------ */

esp_err_t slate_shelly_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_bind_lock = xSemaphoreCreateMutexStatic(&s_bind_lock_storage);
    if (s_bind_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const slate_state_provider_t provider = {
        .id = SLATE_SHELLY_PROVIDER_ID,
        .subscribe = subscribe,
    };
    esp_err_t err = slate_state_provider_register(&provider);
    if (err != ESP_OK) {
        return err;
    }
    s_initialized = true;
    slate_state_provider_set_status(SLATE_SHELLY_PROVIDER_ID, SLATE_PROVIDER_UNCONFIGURED);

    const slate_action_provider_t action_provider = {
        .id = SLATE_SHELLY_PROVIDER_ID,
        .dispatch = dispatch,
    };
    esp_err_t action_err = slate_action_provider_register(&action_provider);
    if (action_err != ESP_OK) {
        ESP_LOGE(TAG, "semantic action dispatch unavailable: %s", esp_err_to_name(action_err));
    }
    return action_err;
}

esp_err_t slate_shelly_start(void)
{
    if (!s_initialized || s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_body = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM);
    if (s_body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_commands = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(command_t));
    if (s_commands == NULL) {
        free(s_body);
        s_body = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_event_handler_instance_register(SLATE_WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event, NULL, NULL);
    if (err != ESP_OK) {
        vQueueDelete(s_commands);
        s_commands = NULL;
        free(s_body);
        s_body = NULL;
        return err;
    }

    slate_wifi_status_t wifi;
    slate_wifi_status(&wifi);
    s_network_up = wifi.connected;

    if (xTaskCreate(poller_task, "slate_shelly", TASK_STACK, NULL, TASK_PRIORITY, &s_task) !=
        pdPASS) {
        /* Unwind everything, and `s_commands` above all: a queue left behind
         * with no task to drain it would let dispatch() accept taps and answer
         * none of them, which §5.3 makes worse than refusing them outright. */
        esp_event_handler_unregister(SLATE_WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event);
        vQueueDelete(s_commands);
        s_commands = NULL;
        free(s_body);
        s_body = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#ifdef SLATE_SHELLY_SELFTEST

/*
 * The three pure functions in this component, against fixture data. They are
 * where a Shelly's own vocabulary becomes §5.2's, so a mistake here is a wrong
 * reading on a wall rather than a crash, and nothing else in the build would
 * notice it.
 */
esp_err_t slate_shelly_selftest(void)
{
    int failures = 0;
#define CHECK(condition, name)                                                \
    do {                                                                      \
        bool passed_ = (condition);                                           \
        failures += !passed_;                                                 \
        ESP_LOGI(TAG, "selftest: %-48s %s", name, passed_ ? "PASS" : "FAIL"); \
    } while (0)

    /* The fixture values round-trip exactly through strtod today, so `==` would
     * pass — but the first fixture value without an exact binary representation
     * would fail for a reason that has nothing to do with this adapter. */
#define NEAR(actual, expected) (fabs((actual) - (expected)) < 1e-9)

    char host[HOST_MAX + 1];
    role_t role;
    uint8_t index;

    CHECK(parse_resource("192.0.2.11/switch:0", host, &role, &index) &&
              strcmp(host, "192.0.2.11") == 0 && role == ROLE_SWITCH && index == 0,
          "an address, a switch and channel zero");
    CHECK(parse_resource("192.0.2.11/switch:1", host, &role, &index) && index == 1,
          "the second channel of a two-channel device");
    CHECK(parse_resource("shelly1-abcdef123456.local/power:0", host, &role, &index) &&
              strcmp(host, "shelly1-abcdef123456.local") == 0 && role == ROLE_POWER,
          "an mDNS name and a power reading");
    CHECK(parse_resource("h/voltage:0", host, &role, &index) && role == ROLE_VOLTAGE,
          "voltage");
    CHECK(parse_resource("h/temperature:9", host, &role, &index) &&
              role == ROLE_TEMPERATURE && index == 9,
          "temperature, and the highest channel a digit holds");

    CHECK(!parse_resource("192.0.2.11", host, &role, &index), "no role at all");
    CHECK(!parse_resource("192.0.2.11/switch", host, &role, &index), "no channel");
    CHECK(!parse_resource("192.0.2.11/relay:0", host, &role, &index),
          "a role this adapter does not have");
    CHECK(!parse_resource("/switch:0", host, &role, &index), "an empty host");
    CHECK(!parse_resource("192.0.2.11/:0", host, &role, &index), "an empty role");
    CHECK(!parse_resource("192.0.2.11/switch:10", host, &role, &index),
          "a two-digit channel, which no device in this family has");
    CHECK(!parse_resource("192.0.2.11/switch:x", host, &role, &index), "a non-numeric channel");

    /* A host that fits a binding must parse here, or the store would accept an
     * id this adapter then silently drops. */
    char longest[SLATE_RESOURCE_ID_MAX + 1];
    size_t host_len = SLATE_RESOURCE_ID_MAX - strlen("/switch:0");
    memset(longest, 'h', host_len);
    snprintf(longest + host_len, sizeof(longest) - host_len, "/switch:0");
    CHECK(parse_resource(longest, host, &role, &index) && strlen(host) == host_len,
          "the longest host a 63-byte resource id can carry");

    static const char GEN2[] =
        "{\"switch:0\":{\"output\":true,\"apower\":3.2,\"voltage\":242.9,"
        "\"temperature\":{\"tC\":41.6}},\"switch:1\":{\"output\":false,\"apower\":0}}";
    cJSON *gen2 = cJSON_Parse(GEN2);
    CHECK(gen2 != NULL, "the Gen2 fixture parses");

    entry_t entry = {.device = 0, .index = 0};
    entry.role = ROLE_SWITCH;
    CHECK(read_gen2(&entry, gen2) && entry.last.light.on, "Gen2 switch:0 reads on");
    entry.index = 1;
    CHECK(read_gen2(&entry, gen2) && !entry.last.light.on, "Gen2 switch:1 reads off");
    entry.index = 0;
    entry.role = ROLE_POWER;
    CHECK(read_gen2(&entry, gen2) && entry.last.sensor.numeric &&
              NEAR(entry.last.sensor.value, 3.2) &&
              entry.last.sensor.measurement == SLATE_MEASUREMENT_POWER,
          "Gen2 power carries watts and its measurement");
    entry.role = ROLE_VOLTAGE;
    CHECK(read_gen2(&entry, gen2) && NEAR(entry.last.sensor.value, 242.9) &&
              entry.last.sensor.category == SLATE_CATEGORY_POWER,
          "Gen2 voltage carries volts and a category, not a measurement");
    entry.role = ROLE_TEMPERATURE;
    CHECK(read_gen2(&entry, gen2) && NEAR(entry.last.sensor.value, 41.6) &&
              entry.last.sensor.measurement == SLATE_MEASUREMENT_TEMPERATURE,
          "Gen2 temperature is the channel's own tC");
    entry.index = 3;
    entry.role = ROLE_SWITCH;
    CHECK(!read_gen2(&entry, gen2), "a channel the device does not have");
    cJSON_Delete(gen2);

    static const char GEN1[] =
        "{\"relays\":[{\"ison\":true}],\"meters\":[{\"power\":0.00}]}";
    cJSON *gen1 = cJSON_Parse(GEN1);
    CHECK(gen1 != NULL, "the Gen1 fixture parses");

    entry.index = 0;
    entry.role = ROLE_SWITCH;
    CHECK(read_gen1(&entry, gen1) && entry.last.light.on, "Gen1 relay reads on");
    entry.role = ROLE_POWER;
    CHECK(read_gen1(&entry, gen1) && entry.last.sensor.numeric &&
              NEAR(entry.last.sensor.value, 0.0),
          "Gen1 meter reads zero watts as a value, not as absent");
    entry.role = ROLE_VOLTAGE;
    CHECK(!read_gen1(&entry, gen1), "Gen1 measures no voltage");
    entry.role = ROLE_TEMPERATURE;
    CHECK(!read_gen1(&entry, gen1), "Gen1 measures no temperature");
    entry.index = 1;
    entry.role = ROLE_SWITCH;
    CHECK(!read_gen1(&entry, gen1), "a second relay a one-channel device does not have");
    cJSON_Delete(gen1);

    /*
     * The invariant `publish_entry()`'s early return rests on, asked of the
     * store rather than of the zeroed struct. `state_is_valid()` runs before
     * the binding lookup, so the refusal does not depend on anything being
     * bound — and this fails if somebody removes the `ever_read` guard, which
     * is the regression worth catching.
     */
    entry_t never = {0};
    const slate_snapshot_t empty = {.resource = "never-read",
                                    .kind = SLATE_KIND_SENSOR,
                                    .available = false,
                                    .state = never.last};
    CHECK(slate_state_publish(SLATE_SHELLY_PROVIDER_ID, &empty) == ESP_ERR_INVALID_ARG,
          "the store refuses a never-read sensor, so publish_entry skips it");

    /*
     * The handover, which is the newest code here and the one that fixes the
     * worst bug. It needs no network and no device: `subscribe()` runs on the
     * caller's task by design, and at this point in startup `s_commands` is
     * still NULL, so the wake-up poke is a no-op rather than a queue write.
     */
    static const char *const THREE[] = {"192.0.2.11/switch:0", "192.0.2.11/power:0",
                                        "192.0.2.12/switch:0"};
    CHECK(subscribe(NULL, THREE, 3) == ESP_OK && adopt_pending() && s_entry_count == 3 &&
              s_device_count == 2,
          "three resources on two hosts, deduplicated by host");
    /* Guarded rather than trusting the check above: CHECK does not short-circuit
     * between cases, so an allocation failure there would crash here instead of
     * reporting a failure. */
    CHECK(s_entries != NULL && s_entry_count == 3 && s_entries[0].device == 0 &&
              s_entries[1].device == 0 && s_entries[2].device == 1,
          "each entry points at the device its host created");
    CHECK(!adopt_pending(), "a second adopt with nothing pending is a no-op");

    CHECK(subscribe(NULL, THREE, 1) == ESP_OK && subscribe(NULL, THREE, 2) == ESP_OK &&
              adopt_pending() && s_entry_count == 2 && s_device_count == 1,
          "a set the poller never adopted is replaced, not queued behind it");

    static const char *const JUNK[] = {"nonsense", "192.0.2.11/switch:0"};
    CHECK(subscribe(NULL, JUNK, 2) == ESP_OK && adopt_pending() && s_entry_count == 1,
          "an unparseable id is dropped and the rest of the set survives");

    /*
     * The carry-over, which is the one the store cannot correct for us:
     * `slate_state_bind()` keeps `available` across a rebuild, so an entry whose
     * `ever_read` was reset publishes nothing when its device goes quiet and the
     * tile keeps a live reading for an unplugged relay.
     */
    CHECK(subscribe(NULL, THREE, 3) == ESP_OK && adopt_pending() && s_devices != NULL &&
              s_entries != NULL && s_device_count == 2,
          "a set to learn something about");
    /* Guarded like the assertions around it: an allocation that failed above
     * leaves these NULL, and a self-test that segfaults on its way to reporting
     * a failure reports nothing at all. */
    if (s_devices != NULL && s_entries != NULL) {
        s_devices[0].generation = 2;
        s_devices[0].reachable = true;
        s_devices[0].ever_reachable = true;
        s_entries[0].ever_read = true;
        s_entries[0].last.light.on = true;
        s_swept = true;
    }
    CHECK(subscribe(NULL, THREE, 3) == ESP_OK && adopt_pending() && s_devices != NULL &&
              s_entries != NULL && s_devices[0].generation == 2 && s_devices[0].reachable &&
              s_devices[0].ever_reachable && s_entries[0].ever_read &&
              s_entries[0].last.light.on,
          "republishing the same set keeps what was learned about its hosts");
    CHECK(s_swept, "and does not drop the provider back to connecting");

    static const char *const ELSEWHERE[] = {"192.0.2.13/switch:0"};
    CHECK(subscribe(NULL, ELSEWHERE, 1) == ESP_OK && adopt_pending() && !s_swept,
          "a host nobody has asked yet does make the status a question again");

    CHECK(subscribe(NULL, NULL, 0) == ESP_OK && adopt_pending() && s_entry_count == 0 &&
              s_device_count == 0 && s_devices == NULL && s_entries == NULL,
          "count zero releases the tables, per §5.1's unsubscribe");

#undef NEAR
#undef CHECK
    ESP_LOGI(TAG, "selftest: %d failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif /* SLATE_SHELLY_SELFTEST */
