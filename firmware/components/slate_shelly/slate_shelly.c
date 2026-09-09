/*
 * Slate — the Shelly integration provider. See include/slate_shelly.h.
 *
 * One task owns everything with a socket in it. §6.1 keeps LVGL on a single
 * task and has every provider post normalized work to it through the store, so
 * a poller that blocks for 267 ms on a relay that is thinking about it cannot
 * hold up a frame. The action bus's dispatch callback therefore does not make
 * the HTTP call either — it queues the command and returns, which is exactly
 * what §5.3 means by "returning ESP_OK only acknowledges that the adapter took
 * responsibility for reporting a later result".
 */

#include "slate_shelly.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "slate_action.h"
#include "slate_state.h"
#include "slate_wifi.h"

static const char *TAG = "slate_shelly";

#define TASK_STACK        6144
#define TASK_PRIORITY     4
#define POLL_INTERVAL_MS  5000
#define HTTP_TIMEOUT_MS   4000

/*
 * 1031 B is the largest real payload measured on a Plus 2PM answering
 * `Shelly.GetStatus` with both channels metered. 4 KB is that with room for a
 * four-channel device, and it is a hard ceiling rather than a growing buffer:
 * a body that does not fit is a device this adapter does not understand, and
 * truncating JSON produces a parse failure instead of a wrong reading.
 */
#define BODY_MAX          4096
#define HOST_MAX          47
#define COMMAND_QUEUE_LEN 8

/* --- The binding set, parsed ---------------------------------------------- */

typedef enum {
    ROLE_SWITCH = 0,   /**< the relay itself, published as a `light` */
    ROLE_POWER,        /**< instantaneous draw, W */
    ROLE_VOLTAGE,      /**< mains, V */
    ROLE_TEMPERATURE,  /**< the device's own, °C */
} role_t;

typedef struct {
    char host[HOST_MAX + 1];
    uint8_t generation; /**< 0 until `GET /shelly` has answered, then 1 or 2 */
    bool reachable;
} device_t;

/*
 * `last` is why an unreachable device does not blank its own tiles. §5.2: "an
 * unavailable resource keeps its last values and renders stale", and a snapshot
 * replaces the previous one in full — so publishing `available: false` with a
 * zeroed state would be publishing that the light is off, which is a different
 * and untrue claim. The adapter carries the last reading forward instead.
 */
typedef struct {
    char id[SLATE_RESOURCE_ID_MAX + 1];
    uint16_t device;
    role_t role;
    uint8_t index;
    slate_state_value_t last;
} entry_t;

/*
 * One queue carries both things the task can be asked to do. An empty
 * `resource` is a rebuild asking for a sweep now rather than at the end of the
 * current interval — a dashboard that was just pushed should not show dashes
 * for five seconds to save a second queue.
 */
typedef struct {
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    uint32_t action_id;
    bool toggle;
    bool power;
} command_t;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_storage;
static device_t *s_devices;
static entry_t *s_entries;
static size_t s_device_count;
static size_t s_entry_count;
static char *s_body;
static QueueHandle_t s_commands;
static TaskHandle_t s_task;
static volatile bool s_network_up;
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
static esp_err_t fetch_json(const char *host, const char *path, cJSON **out)
{
    char url[HOST_MAX + 64];
    int written = snprintf(url, sizeof(url), "http://%s%s", host, path);
    if (written <= 0 || (size_t)written >= sizeof(url)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
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
 * `type` and no such field. That is the whole discrimination, and it is done
 * once per host rather than per sweep.
 */
static void probe_generation(device_t *device)
{
    cJSON *doc = NULL;
    if (fetch_json(device->host, "/shelly", &doc) != ESP_OK) {
        device->reachable = false;
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
    /* A Gen1 Shelly 1 measures neither of these. A role it cannot answer is a
     * binding this adapter reports as unavailable rather than as zero. */
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
    if (fetch_json(device->host, path, &doc) != ESP_OK) {
        goto unreachable;
    }
    device->reachable = true;

    for (size_t i = 0; i < s_entry_count; i++) {
        entry_t *entry = &s_entries[i];
        if (entry->device != device_index) {
            continue;
        }
        bool read = device->generation >= 2 ? read_gen2(entry, doc) : read_gen1(entry, doc);
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

    uint8_t generation = 0;
    size_t device_index = SIZE_MAX;
    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].host, host) == 0) {
            generation = s_devices[i].generation;
            device_index = i;
            break;
        }
    }
    if (device_index == SIZE_MAX) {
        slate_action_result(SLATE_SHELLY_PROVIDER_ID, command->action_id, false, "not_bound");
        return;
    }
    if (generation == 0) {
        probe_generation(&s_devices[device_index]);
        generation = s_devices[device_index].generation;
        if (generation == 0) {
            slate_action_result(SLATE_SHELLY_PROVIDER_ID, command->action_id, false,
                                "unreachable");
            return;
        }
    }

    char path[64];
    if (generation >= 2) {
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
    esp_err_t err = fetch_json(host, path, &doc);
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

static void update_status(void)
{
    slate_provider_status_t status;
    if (s_entry_count == 0) {
        status = SLATE_PROVIDER_UNCONFIGURED;
    } else if (!s_network_up) {
        status = SLATE_PROVIDER_OFFLINE;
    } else {
        status = SLATE_PROVIDER_ONLINE;
    }
    slate_state_provider_set_status(SLATE_SHELLY_PROVIDER_ID, status);
}

static void poller_task(void *arg)
{
    (void)arg;
    for (;;) {
        command_t command;
        TickType_t wait = pdMS_TO_TICKS(POLL_INTERVAL_MS);
        bool have_command = xQueueReceive(s_commands, &command, wait) == pdTRUE;

        bool is_action = have_command && command.resource[0] != '\0';

        xSemaphoreTake(s_lock, portMAX_DELAY);
        update_status();
        if (s_entry_count == 0 || !s_network_up) {
            if (is_action) {
                slate_action_result(SLATE_SHELLY_PROVIDER_ID, command.action_id, false, "offline");
            }
        } else if (is_action) {
            run_command(&command);
        } else {
            for (size_t i = 0; i < s_device_count; i++) {
                poll_device(i);
            }
        }
        xSemaphoreGive(s_lock);
    }
}

/* --- Provider callbacks --------------------------------------------------- */

/*
 * §5.1's second operation, and this adapter's entire configuration. The tables
 * are rebuilt whole rather than diffed: a rebuild is rare, the sets are small,
 * and a diff would have to decide what a host losing its last resource means
 * about a probe already in flight.
 */
static esp_err_t subscribe(void *ctx, const char *const *resources, size_t count)
{
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    free(s_entries);
    free(s_devices);
    s_entries = NULL;
    s_devices = NULL;
    s_entry_count = 0;
    s_device_count = 0;

    if (count > 0) {
        s_entries = heap_caps_calloc(count, sizeof(entry_t), MALLOC_CAP_SPIRAM);
        s_devices = heap_caps_calloc(count, sizeof(device_t), MALLOC_CAP_SPIRAM);
        if (s_entries == NULL || s_devices == NULL) {
            free(s_entries);
            free(s_devices);
            s_entries = NULL;
            s_devices = NULL;
            update_status();
            xSemaphoreGive(s_lock);
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
        for (size_t d = 0; d < s_device_count; d++) {
            if (strcmp(s_devices[d].host, host) == 0) {
                device = d;
                break;
            }
        }
        if (device == SIZE_MAX) {
            device = s_device_count++;
            snprintf(s_devices[device].host, sizeof(s_devices[device].host), "%s", host);
        }
        entry_t *entry = &s_entries[s_entry_count++];
        snprintf(entry->id, sizeof(entry->id), "%s", resources[i]);
        entry->device = (uint16_t)device;
        entry->role = role;
        entry->index = index;
    }

    ESP_LOGI(TAG, "%u resources on %u devices", (unsigned)s_entry_count,
             (unsigned)s_device_count);
    update_status();
    xSemaphoreGive(s_lock);

    if (s_commands != NULL) {
        const command_t sweep = {0};
        xQueueSend(s_commands, &sweep, 0);
    }
    return ESP_OK;
}

static esp_err_t dispatch(void *ctx, uint32_t id, const slate_action_request_t *request)
{
    (void)ctx;
    if (request->action != SLATE_ACTION_TOGGLE && request->action != SLATE_ACTION_SET_POWER) {
        return ESP_ERR_INVALID_ARG;
    }
    command_t command = {
        .action_id = id,
        .toggle = request->action == SLATE_ACTION_TOGGLE,
        .power = request->value_type == SLATE_ACTION_VALUE_BOOL && request->value.boolean,
    };
    if (request->action == SLATE_ACTION_SET_POWER &&
        request->value_type != SLATE_ACTION_VALUE_BOOL) {
        return ESP_ERR_INVALID_ARG;
    }
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
    if (!s_network_up && s_entry_count > 0) {
        slate_state_provider_set_status(SLATE_SHELLY_PROVIDER_ID, SLATE_PROVIDER_OFFLINE);
    }
}

/* --- Lifecycle ------------------------------------------------------------ */

esp_err_t slate_shelly_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (s_lock == NULL) {
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
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
