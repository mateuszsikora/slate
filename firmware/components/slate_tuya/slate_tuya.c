/*
 * Slate — the Tuya integration provider. See include/slate_tuya.h for what
 * this is and what it refuses to be.
 *
 * One task owns everything with a socket in it, and it owns the device tables
 * outright: nothing else reads or writes them, so there is no lock to take
 * across an HTTPS round trip and no way for a rebuild to end up waiting on
 * the cloud. That is the Shelly structure with one addition the cloud makes
 * necessary: the live client can be created and destroyed by a credentials
 * change at any moment, so the poller reconciles its client against the store
 * at the top of every pass (§ "Reconciliation" below) and no other task ever
 * touches it.
 */

#include "slate_tuya.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "slate_action.h"
#include "slate_api.h"
#include "slate_state.h"
#include "slate_store.h"
#include "slate_tuya_client.h"
#include "slate_tuya_map.h"
#include "slate_wifi.h"

static const char *TAG = "slate_tuya";

/*
 * TLS costs more stack than Shelly's plain HTTP, and the credentials test runs
 * on its own task for the same reason — a parked HTTP request must not hold
 * the API task for the length of a cloud handshake. slate_update's HTTPS task
 * uses the same figure.
 */
#define TASK_STACK        8192
#define TASK_PRIORITY     4
#define POLL_INTERVAL_MS  5000
#define COMMAND_QUEUE_LEN 8
#define CATALOG_QUEUE_LEN 1

/*
 * The route body is four short fields; 384 bytes is every one of them at its
 * store limit plus the JSON around them.
 */
#define ROUTE_BODY_MAX 384

/*
 * How long the poller waits before re-asking the cloud a question it just got
 * a useless answer to: a device list that named no bound device (a binding to
 * a device the app deleted), or a functions spec that classified as
 * UNSUPPORTED (a category the map module has no vocabulary for, which a
 * firmware update might fix). Without this, each of those costs a cloud call
 * every sweep for as long as the binding exists.
 */
#define RETRY_BACKOFF_MS 60000

/* --- Types ---------------------------------------------------------------- */

/*
 * The poller's device table. `info_known` is the device-list half (name,
 * category) and `dps_known` the functions-spec half; they fail separately —
 * a device can leave the app's home while its functions spec stays cached —
 * so they are refetched separately too.
 */
typedef struct {
    char id[SLATE_RESOURCE_ID_MAX + 1]; /* the base Tuya device id */
    char name[SLATE_RESOURCE_NAME_MAX + 1];
    char category[16];
    slate_tuya_dps_t dps;
    bool dps_known;
    bool info_known;
    bool reachable;
    bool ever_reachable;
    int64_t info_retry_us;   /* next permitted device-list question */
    int64_t dps_retry_us;    /* next permitted functions question */
} device_t;

/*
 * One bound resource. A dual temp/humidity sensor occupies two entries — the
 * bare device id for temperature, "<id>/humidity" for the second reading —
 * both pointing at the same device_t, because Slate binds exactly one
 * reading per resource and the map module selects the reading by resource id.
 */
typedef struct {
    char id[SLATE_RESOURCE_ID_MAX + 1]; /* the full resource id, suffix and all */
    uint16_t device;
    bool humidity;
    bool ever_read;
    slate_capabilities_t capabilities;
    slate_state_value_t last;
} entry_t;

typedef enum {
    CMD_SWEEP = 0,    /* a rebuild asking for a sweep now; carries nothing */
    CMD_ACTION,
    CMD_WAKE,         /* credentials changed; the loop's reconciliation acts */
} command_kind_t;

typedef struct {
    command_kind_t kind;
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    uint32_t action_id;
    slate_action_t action;
    slate_action_value_type_t value_type;
    bool boolean;
    int32_t number;
    uint32_t credentials_epoch;
    int64_t deadline_us;
} command_t;

/* A parked POST /tuya, from the HTTP task to the configure task. */
typedef struct {
    httpd_req_t *request;
    uint32_t credentials_epoch;
    char region[SLATE_TUYA_REGION_MAX_LEN + 1];
    char access_id[SLATE_TUYA_ACCESS_ID_MAX_LEN + 1];
    char secret[SLATE_TUYA_SECRET_MAX_LEN + 1];
    char uid[SLATE_TUYA_UID_MAX_LEN + 1];
} configure_job_t;

typedef struct {
    uint32_t credentials_epoch;
} catalog_job_t;

/* --- State ---------------------------------------------------------------- */

/* Owned by the poller task alone. No lock guards these. */
static device_t *s_devices;
static entry_t *s_entries;
static size_t s_device_count;
static size_t s_entry_count;
static slate_tuya_client_handle_t s_client;
static char s_uid[SLATE_TUYA_UID_MAX_LEN + 1]; /* the client's, copied at build */
static bool s_client_up_to_date;
static size_t s_cursor;
static bool s_swept;
static bool s_offline_hint; /* a network-classified failure this pass */
static _Atomic int s_error_hint_kind; /* auth/quota failure this pass */
static int64_t s_list_fetch_us;

/* The handover. Held for pointer moves and nothing else. */
static SemaphoreHandle_t s_bind_lock;
static StaticSemaphore_t s_bind_lock_storage;
static device_t *s_pending_devices;
static entry_t *s_pending_entries;
static size_t s_pending_device_count;
static size_t s_pending_entry_count;
static bool s_pending_valid;

static QueueHandle_t s_commands;
static QueueHandle_t s_config_jobs;
static QueueHandle_t s_catalog_jobs;
static SemaphoreHandle_t s_mutation_lock;
static StaticSemaphore_t s_mutation_lock_storage;
static TaskHandle_t s_task;
static TaskHandle_t s_catalog_task;
static volatile bool s_network_up;
/*
 * Bumped by the routes on any credentials change and read by the poller,
 * which compares it against the epoch of the client it holds. Atomic because
 * the configure task and a DELETE from the HTTP task can both increment;
 * a torn read would either miss a change for one pass or rebuild twice, and
 * stdatomic costs nothing here.
 */
static _Atomic uint32_t s_credentials_epoch;
enum { ACTIVE_POLLER, ACTIVE_CATALOG, ACTIVE_CONFIG, ACTIVE_COUNT };
static _Atomic uint32_t s_active_epoch[ACTIVE_COUNT];
#define EPOCH_INACTIVE UINT32_MAX
static uint32_t s_client_epoch; /* the epoch s_client was built from */
static bool s_initialized;

/*
 * The picker's catalog cache, guarded by its own lock because it is read from
 * the HTTP task while nothing else here is. A fresh cache is served without a
 * cloud call. A dedicated catalog task owns its throwaway client, so the HTTP
 * task only copies a bounded snapshot and requests refresh work.
 */
static struct {
    SemaphoreHandle_t lock;
    slate_resource_t *items;
    size_t count;
    int64_t fetched_us;
    uint32_t credentials_epoch;
    uint32_t refresh_epoch;
    slate_api_catalog_state_t state;
    bool refresh_pending;
} s_catalog;
#define CATALOG_FRESH_MS 60000
#define CATALOG_RESOURCE_CAP SLATE_STATE_MAX_RESOURCES

static void request_catalog_refresh(void);
static void classify_failure(slate_tuya_error_kind_t kind);

static bool epoch_work_begin(size_t slot, uint32_t epoch)
{
    xSemaphoreTake(s_mutation_lock, portMAX_DELAY);
    bool current = epoch == atomic_load(&s_credentials_epoch);
    if (current) {
        atomic_store(&s_active_epoch[slot], epoch);
    }
    xSemaphoreGive(s_mutation_lock);
    return current;
}

static void epoch_work_end(size_t slot)
{
    atomic_store(&s_active_epoch[slot], EPOCH_INACTIVE);
}

static uint32_t mutation_begin(void)
{
    xSemaphoreTake(s_mutation_lock, portMAX_DELAY);
    uint32_t epoch = atomic_fetch_add(&s_credentials_epoch, 1) + 1;
    xSemaphoreGive(s_mutation_lock);
    return epoch;
}

static void mutation_barrier(uint32_t epoch)
{
    for (;;) {
        bool prior = false;
        for (size_t i = 0; i < ACTIVE_COUNT; i++) {
            uint32_t active = atomic_load(&s_active_epoch[i]);
            prior = prior || (active != EPOCH_INACTIVE && active < epoch);
        }
        if (!prior) {
            return;
        }
        vTaskDelay(1);
    }
}

static bool epoch_is_current(uint32_t epoch)
{
    return epoch == atomic_load(&s_credentials_epoch) && epoch == s_client_epoch &&
           slate_store_tuya_is_set();
}

/* --- Resource ids --------------------------------------------------------- */

/*
 * A resource id is a bare Tuya device id, or one with the humidity suffix the
 * map module also knows. The suffix is therefore the only syntax this parser
 * implements, and it delegates the spelling to the constant so the two
 * modules cannot drift.
 */
static bool decode_resource_id(const char *resource, char *device_id,
                               size_t device_id_size, bool *humidity)
{
    if (resource == NULL || device_id == NULL || device_id_size == 0 || humidity == NULL) {
        return false;
    }
    size_t len = strnlen(resource, SLATE_RESOURCE_ID_MAX + 1);
    if (len == 0 || len > SLATE_RESOURCE_ID_MAX) {
        return false;
    }
    size_t suffix_len = strlen(SLATE_TUYA_HUMIDITY_SUFFIX);
    size_t base_len = len;
    *humidity = false;
    if (len > suffix_len &&
        strcmp(resource + len - suffix_len, SLATE_TUYA_HUMIDITY_SUFFIX) == 0) {
        base_len -= suffix_len;
        *humidity = true;
    } else if (memchr(resource, '/', len) != NULL) {
        return false;
    }
    if (base_len == 0 || base_len >= device_id_size) {
        return false;
    }
    memcpy(device_id, resource, base_len);
    device_id[base_len] = '\0';
    return true;
}

static bool encode_resource_id(const char *device_id, bool humidity,
                               char *resource, size_t resource_size)
{
    if (device_id == NULL || resource == NULL || resource_size == 0) {
        return false;
    }
    size_t base_len = strnlen(device_id, SLATE_RESOURCE_ID_MAX + 1);
    size_t suffix_len = humidity ? strlen(SLATE_TUYA_HUMIDITY_SUFFIX) : 0;
    if (base_len == 0 || base_len > SLATE_RESOURCE_ID_MAX ||
        memchr(device_id, '/', base_len) != NULL ||
        base_len + suffix_len > SLATE_RESOURCE_ID_MAX ||
        base_len + suffix_len >= resource_size) {
        return false;
    }
    memcpy(resource, device_id, base_len);
    if (humidity) {
        memcpy(resource + base_len, SLATE_TUYA_HUMIDITY_SUFFIX, suffix_len);
    }
    resource[base_len + suffix_len] = '\0';
    return true;
}

/* --- The client, owned by the poller --------------------------------------- */

/*
 * Reconciliation: the routes may store or clear credentials at any moment,
 * and they announce it by bumping s_credentials_epoch. The poller is the one
 * task allowed to hold the live client, so it is the one that rebuilds it —
 * no route ever swaps a handle another task is calling into, and a configure
 * job's validation always ran on a throwaway client of its own.
 */
static void reconcile_client(void)
{
    bool want = slate_store_tuya_is_set();
    uint32_t epoch = atomic_load(&s_credentials_epoch);
    if (!want && s_client != NULL) {
        slate_tuya_client_destroy(s_client);
        s_client = NULL;
        s_client_up_to_date = false;
        ESP_LOGI(TAG, "cloud client released: credentials cleared");
    }
    if (want && (!s_client_up_to_date || s_client == NULL ||
                 epoch != s_client_epoch)) {
        char region[SLATE_TUYA_REGION_MAX_LEN + 1] = {0};
        char access_id[SLATE_TUYA_ACCESS_ID_MAX_LEN + 1] = {0};
        char secret[SLATE_TUYA_SECRET_MAX_LEN + 1] = {0};
        char uid[SLATE_TUYA_UID_MAX_LEN + 1] = {0};
        if (slate_store_tuya_get(region, sizeof(region), access_id, sizeof(access_id),
                                 secret, sizeof(secret), uid, sizeof(uid)) == ESP_OK) {
            slate_tuya_client_handle_t fresh = NULL;
            if (slate_tuya_client_create(region, access_id, secret, uid, &fresh) ==
                ESP_OK) {
                if (s_client != NULL) {
                    slate_tuya_client_destroy(s_client);
                }
                s_client = fresh;
                snprintf(s_uid, sizeof(s_uid), "%s", uid);
                s_client_up_to_date = true;
                s_client_epoch = epoch;
                s_swept = false; /* the new client has proven nothing yet */
                s_list_fetch_us = 0;
                slate_state_provider_set_status(SLATE_TUYA_PROVIDER_ID,
                                                SLATE_PROVIDER_CONNECTING);
                ESP_LOGI(TAG, "cloud client built for region %s", region);
                /* Pre-warm the picker's catalog on its own TLS-capable task. */
                if (s_network_up) {
                    request_catalog_refresh();
                }
            } else {
                ESP_LOGE(TAG, "cloud client build failed — continuing");
            }
            explicit_bzero(secret, sizeof(secret));
        }
    }
}

/* --- Reading devices ------------------------------------------------------- */

/** @brief GET one client path as the `result` subtree; caller deletes *out. */
static esp_err_t client_result(const char *path, cJSON **out,
                               slate_tuya_error_kind_t *err_kind)
{
    return slate_tuya_client_request(s_client, HTTP_METHOD_GET, path, NULL, out,
                                     err_kind);
}

/*
 * The device list is the only place the cloud names a device's category, and
 * map_functions needs one — so the poller keeps its own copy, refreshed when
 * a bound device is unknown to it and no more often than the backoff allows.
 * One response fills every unknown device in the table, so a fresh dashboard
 * costs one list call however many devices it binds.
 */
static void refresh_device_list(void)
{
    uint32_t epoch = s_client_epoch;
    if (!epoch_is_current(epoch)) {
        return;
    }
    char path[SLATE_TUYA_UID_MAX_LEN + 32];
    snprintf(path, sizeof(path), "/v1.0/users/%s/devices", s_uid);
    cJSON *result = NULL;
    slate_tuya_error_kind_t kind = SLATE_TUYA_ERR_NONE;
    esp_err_t err = client_result(path, &result, &kind);
    if (!epoch_is_current(epoch)) {
        cJSON_Delete(result);
        return;
    }
    s_list_fetch_us = esp_timer_get_time();
    if (err != ESP_OK) {
        classify_failure(kind);
        ESP_LOGW(TAG, "device list: %s (%d)", esp_err_to_name(err), (int)kind);
        return;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, result)
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *category = cJSON_GetObjectItemCaseSensitive(item, "category");
        if (!cJSON_IsString(id) || id->valuestring[0] == '\0' ||
            !cJSON_IsString(category)) {
            continue;
        }
        for (size_t d = 0; d < s_device_count; d++) {
            if (strcmp(s_devices[d].id, id->valuestring) == 0) {
                snprintf(s_devices[d].category, sizeof(s_devices[d].category), "%s",
                         category->valuestring);
                if (cJSON_IsString(name) && name->valuestring[0] != '\0') {
                    snprintf(s_devices[d].name, sizeof(s_devices[d].name), "%s",
                             name->valuestring);
                }
                s_devices[d].info_known = true;
                break;
            }
        }
    }
    cJSON_Delete(result);
}

/*
 * The specification is what turns a device's status and function schemas into
 * §5.2's reportable readings and actionable vocabulary, and
 * it is fetched once per device per binding lifetime — unless the category
 * classified as UNSUPPORTED, in which case the backoff decides when to ask
 * again, because a firmware update may add the vocabulary that device was
 * waiting for.
 */
static void refresh_specification(device_t *device)
{
    uint32_t epoch = s_client_epoch;
    if (!epoch_is_current(epoch)) {
        return;
    }
    char path[SLATE_RESOURCE_ID_MAX + 32];
    snprintf(path, sizeof(path), "/v1.1/devices/%s/specifications", device->id);
    cJSON *result = NULL;
    slate_tuya_error_kind_t kind = SLATE_TUYA_ERR_NONE;
    esp_err_t err = client_result(path, &result, &kind);
    if (!epoch_is_current(epoch)) {
        cJSON_Delete(result);
        return;
    }
    device->dps_retry_us = esp_timer_get_time() + (int64_t)RETRY_BACKOFF_MS * 1000;
    if (err != ESP_OK) {
        classify_failure(kind);
        ESP_LOGW(TAG, "specification for %.22s: %s (%d)", device->id,
                 esp_err_to_name(err), (int)kind);
        return;
    }
    const cJSON *functions = cJSON_GetObjectItemCaseSensitive(result, "functions");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(result, "status");
    slate_tuya_dps_t dps;
    if (slate_tuya_map_specification(device->category, functions, status, &dps) == ESP_OK) {
        device->dps = dps;
        device->dps_known = true;
    } else {
        /* Leave dps_known as it was: a device that classified before keeps its
         * old layout, and one that never did simply stays unpublishable until
         * a spec arrives that maps. */
        ESP_LOGI(TAG, "%.22s (%s) has no mapping yet", device->id, device->category);
    }
    cJSON_Delete(result);
}

static slate_kind_t class_kind(slate_tuya_class_t cls)
{
    switch (cls) {
    case SLATE_TUYA_CLASS_LIGHT:          return SLATE_KIND_LIGHT;
    case SLATE_TUYA_CLASS_COVER:          return SLATE_KIND_COVER;
    case SLATE_TUYA_CLASS_SENSOR_TEMP_HUM:
    case SLATE_TUYA_CLASS_SENSOR_POWER:   return SLATE_KIND_SENSOR;
    default:                              return SLATE_KIND_LIGHT; /* callers gate on cls */
    }
}

/*
 * `ever_read` is load-bearing, not bookkeeping: §5.2's snapshot has no "no
 * value" state, so a resource the cloud has never answered for has nothing
 * publishable to say and §3.3's placeholder — which names provider:resource —
 * is the accurate thing to show. Once a value has arrived, `last` is what an
 * unavailable publication carries, so a cloud outage stales the tile rather
 * than claiming the light went off.
 */
static void publish_entry(entry_t *entry, slate_kind_t kind, bool available,
                          const char *name)
{
    if (!entry->ever_read) {
        return;
    }
    slate_snapshot_t snapshot = {
        .resource = entry->id,
        .kind = kind,
        .name = name[0] != '\0' ? name : NULL,
        .available = available,
        .capabilities = entry->capabilities,
        .state = entry->last,
    };
    esp_err_t err = slate_state_publish(SLATE_TUYA_PROVIDER_ID, &snapshot);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "publish %s: %s", entry->id, esp_err_to_name(err));
    }
}

static void classify_failure(slate_tuya_error_kind_t kind)
{
    if (kind == SLATE_TUYA_ERR_NETWORK) {
        s_offline_hint = true;
        slate_state_provider_set_status(SLATE_TUYA_PROVIDER_ID, SLATE_PROVIDER_OFFLINE);
    } else if (kind == SLATE_TUYA_ERR_AUTH || kind == SLATE_TUYA_ERR_QUOTA) {
        /* These need a person: a rejected credential or an expired cloud
         * subscription looks identical to a dead dashboard from the sofa, and
         * the log line is the one place the difference is named. */
        atomic_store(&s_error_hint_kind, kind);
        slate_state_provider_set_status_reason(
            SLATE_TUYA_PROVIDER_ID, SLATE_PROVIDER_ERROR,
            kind == SLATE_TUYA_ERR_AUTH ? "auth" : "quota");
        ESP_LOGE(TAG, "cloud rejected a call (kind %d) — credentials or subscription",
                 (int)kind);
    } else if (kind == SLATE_TUYA_ERR_API) {
        atomic_store(&s_error_hint_kind, kind);
        slate_state_provider_set_status(SLATE_TUYA_PROVIDER_ID, SLATE_PROVIDER_ERROR);
    }
}

static void poll_device(size_t device_index)
{
    uint32_t epoch = s_client_epoch;
    if (!epoch_is_current(epoch)) {
        return;
    }
    device_t *device = &s_devices[device_index];
    int64_t now = esp_timer_get_time();

    if (!device->info_known && now >= device->info_retry_us &&
        now - s_list_fetch_us >= (int64_t)RETRY_BACKOFF_MS * 1000) {
        refresh_device_list();
        device->info_retry_us = now + (int64_t)RETRY_BACKOFF_MS * 1000;
    }
    if (device->info_known &&
        (!device->dps_known || device->dps.cls == SLATE_TUYA_CLASS_UNSUPPORTED) &&
        now >= device->dps_retry_us) {
        refresh_specification(device);
    }
    if (!device->info_known || !device->dps_known ||
        device->dps.cls == SLATE_TUYA_CLASS_UNSUPPORTED) {
        device->reachable = false;
        return; /* nothing publishable, and nothing true to say yet */
    }

    char path[SLATE_RESOURCE_ID_MAX + 32];
    snprintf(path, sizeof(path), "/v1.0/devices/%s/status", device->id);
    cJSON *result = NULL;
    slate_tuya_error_kind_t kind = SLATE_TUYA_ERR_NONE;
    if (client_result(path, &result, &kind) != ESP_OK) {
        classify_failure(kind);
        if (device->reachable) {
            ESP_LOGW(TAG, "%.22s stopped answering", device->id);
        }
        device->reachable = false;
        for (size_t i = 0; i < s_entry_count; i++) {
            if (s_entries[i].device == device_index) {
                publish_entry(&s_entries[i], class_kind(device->dps.cls), false,
                              device->name);
            }
        }
        return;
    }
    if (!epoch_is_current(epoch)) {
        cJSON_Delete(result);
        return;
    }
    device->reachable = true;
    device->ever_reachable = true;

    slate_kind_t kind_of_device = class_kind(device->dps.cls);
    for (size_t i = 0; i < s_entry_count; i++) {
        entry_t *entry = &s_entries[i];
        if (entry->device != device_index) {
            continue;
        }
        slate_snapshot_t snapshot;
        if (slate_tuya_map_status(&device->dps, result, entry->id, device->name,
                                  &snapshot) == ESP_OK) {
            entry->ever_read = true;
            entry->capabilities = snapshot.capabilities;
            entry->last = snapshot.state;
            publish_entry(entry, kind_of_device, true, device->name);
        } else {
            /* A reading this device does not carry stales exactly like one
             * that vanished from an otherwise healthy answer: from the tile's
             * side they are the same fact. */
            publish_entry(entry, kind_of_device, false, device->name);
        }
    }
    cJSON_Delete(result);
}

/* --- Actions --------------------------------------------------------------- */

static void run_action(const command_t *command)
{
    if (!epoch_is_current(command->credentials_epoch) ||
        esp_timer_get_time() >= command->deadline_us) {
        slate_action_result(SLATE_TUYA_PROVIDER_ID, command->action_id, false,
                            "stale_configuration");
        return;
    }
    char base[SLATE_RESOURCE_ID_MAX + 1];
    bool humidity = false;
    if (!decode_resource_id(command->resource, base, sizeof(base), &humidity) || humidity) {
        slate_action_result(SLATE_TUYA_PROVIDER_ID, command->action_id, false,
                            "unsupported_action");
        return;
    }

    size_t device_index = SIZE_MAX;
    entry_t *entry = NULL;
    for (size_t i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].id, command->resource) == 0) {
            entry = &s_entries[i];
            device_index = entry->device;
            break;
        }
    }
    if (entry == NULL || device_index >= s_device_count) {
        slate_action_result(SLATE_TUYA_PROVIDER_ID, command->action_id, false,
                            "not_bound");
        return;
    }
    device_t *device = &s_devices[device_index];

    /* A command needs the device's DP layout, which the sweep normally has
     * already fetched; a tap on a tile that never swept (the panel booted into
     * a cloud outage, say) pays for it here rather than failing. */
    int64_t now = esp_timer_get_time();
    if (!device->info_known && now >= device->info_retry_us &&
        now - s_list_fetch_us >= (int64_t)RETRY_BACKOFF_MS * 1000) {
        refresh_device_list();
        device->info_retry_us = now + (int64_t)RETRY_BACKOFF_MS * 1000;
    }
    if (device->info_known &&
        (!device->dps_known || device->dps.cls == SLATE_TUYA_CLASS_UNSUPPORTED) &&
        now >= device->dps_retry_us) {
        refresh_specification(device);
    }
    if (!device->dps_known || device->dps.cls == SLATE_TUYA_CLASS_UNSUPPORTED) {
        slate_action_result(SLATE_TUYA_PROVIDER_ID, command->action_id, false,
                            "unsupported_action");
        return;
    }

    bool has_bool = false;
    bool bool_value = false;
    int32_t number = 0;
    switch (command->action) {
    case SLATE_ACTION_SET_POWER:
        has_bool = command->value_type == SLATE_ACTION_VALUE_BOOL;
        bool_value = command->boolean;
        break;
    case SLATE_ACTION_TOGGLE:
        /* Tuya has no toggle command; the target comes from the last published
         * state, which is the same thing a person looking at the tile knows. */
        if (device->dps.cls != SLATE_TUYA_CLASS_LIGHT || !entry->ever_read) {
            slate_action_result(SLATE_TUYA_PROVIDER_ID, command->action_id, false,
                                "no_state");
            return;
        }
        has_bool = true;
        bool_value = !entry->last.light.on;
        break;
    case SLATE_ACTION_SET_BRIGHTNESS:
    case SLATE_ACTION_SET_COLOR_TEMPERATURE:
    case SLATE_ACTION_SET_POSITION:
        number = command->number;
        break;
    case SLATE_ACTION_OPEN:
    case SLATE_ACTION_STOP:
    case SLATE_ACTION_CLOSE:
        break; /* the map module names the cover control value itself */
    default:
        slate_action_result(SLATE_TUYA_PROVIDER_ID, command->action_id, false,
                            "unsupported_action");
        return;
    }

    cJSON *body = NULL;
    if (slate_tuya_map_command(&device->dps, command->action, has_bool, bool_value,
                               number, &body) != ESP_OK) {
        slate_action_result(SLATE_TUYA_PROVIDER_ID, command->action_id, false,
                            "unsupported_action");
        return;
    }
    char *body_text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (body_text == NULL) {
        slate_action_result(SLATE_TUYA_PROVIDER_ID, command->action_id, false,
                            "out_of_memory");
        return;
    }

    char path[SLATE_RESOURCE_ID_MAX + 32];
    snprintf(path, sizeof(path), "/v1.0/devices/%s/commands", device->id);
    slate_tuya_error_kind_t kind = SLATE_TUYA_ERR_NONE;
    esp_err_t err = slate_tuya_client_request(s_client, HTTP_METHOD_POST, path,
                                              body_text, NULL, &kind);
    cJSON_free(body_text);
    if (err != ESP_OK) {
        classify_failure(kind);
        slate_action_result(SLATE_TUYA_PROVIDER_ID, command->action_id, false,
                            kind == SLATE_TUYA_ERR_NETWORK ? "unreachable"
                                                           : "cloud_error");
        return;
    }
    if (!epoch_is_current(command->credentials_epoch)) {
        slate_action_result(SLATE_TUYA_PROVIDER_ID, command->action_id, false,
                            "stale_configuration");
        return;
    }

    /* §5.3: the acknowledgement says the cloud took the command; the
     * confirmation is the next snapshot. Re-reading the device now rather
     * than waiting out the sweep is what keeps a tap from sitting pending. */
    slate_action_result(SLATE_TUYA_PROVIDER_ID, command->action_id, true, NULL);
    poll_device(device_index);
}

/* --- The one task ---------------------------------------------------------- */

static void update_status(void)
{
    slate_provider_status_t status;
    if (s_client == NULL && !slate_store_tuya_is_set()) {
        status = SLATE_PROVIDER_UNCONFIGURED;
    } else if (!s_network_up) {
        status = SLATE_PROVIDER_OFFLINE;
    } else if (atomic_load(&s_error_hint_kind) != SLATE_TUYA_ERR_NONE) {
        status = SLATE_PROVIDER_ERROR;
    } else if (s_device_count == 0) {
        /* Configured and nothing bound: there is nothing to poll and nothing
         * to be wrong. The cloud was reachable when the credentials were
         * tested, and saying `connecting` here would be describing a sweep
         * that is never going to happen. */
        status = SLATE_PROVIDER_ONLINE;
    } else if (!s_swept) {
        status = SLATE_PROVIDER_CONNECTING;
    } else if (s_offline_hint) {
        status = SLATE_PROVIDER_OFFLINE;
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
    slate_tuya_error_kind_t reason = atomic_load(&s_error_hint_kind);
    slate_state_provider_set_status_reason(
        SLATE_TUYA_PROVIDER_ID, status,
        reason == SLATE_TUYA_ERR_AUTH ? "auth" :
        reason == SLATE_TUYA_ERR_QUOTA ? "quota" : NULL);
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

    /* Carry across what this adapter already knows about devices the rebuild
     * did not change — the same reasoning as the Shelly handover, with the
     * DP layout and the device-list knowledge added to what a host's
     * generation was: `slate_state_bind()` carries the last value across a
     * rebuild, so an entry whose `ever_read` reset would publish nothing when
     * its device went quiet and the tile would keep a live reading forever. */
    bool new_device = false;
    for (size_t d = 0; d < device_count; d++) {
        const device_t *previous = NULL;
        for (size_t o = 0; o < s_device_count; o++) {
            if (strcmp(s_devices[o].id, devices[d].id) == 0) {
                previous = &s_devices[o];
                break;
            }
        }
        if (previous == NULL) {
            new_device = true;
            continue;
        }
        devices[d] = *previous; /* name, category, dps, reachability, retries */
    }
    for (size_t e = 0; e < entry_count; e++) {
        for (size_t o = 0; o < s_entry_count; o++) {
            if (strcmp(s_entries[o].id, entries[e].id) == 0) {
                entries[e].ever_read = s_entries[o].ever_read;
                entries[e].capabilities = s_entries[o].capabilities;
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
    s_cursor = 0;
    if (new_device) {
        s_swept = false;
    }
    ESP_LOGI(TAG, "%u resources on %u devices", (unsigned)s_entry_count,
             (unsigned)s_device_count);
    update_status();
    return true;
}

static void sweep(void)
{
    s_offline_hint = false;
    atomic_store(&s_error_hint_kind, SLATE_TUYA_ERR_NONE);
    if (s_device_count == 0) {
        return;
    }
    for (size_t n = 0; n < s_device_count; n++) {
        /* A tap is waiting and §5.3 gives it three seconds; finishing the
         * sweep first would spend them on devices nobody is looking at. */
        if (uxQueueMessagesWaiting(s_commands) > 0) {
            break;
        }
        poll_device(s_cursor % s_device_count);
        s_cursor++;
    }
    if (s_cursor >= s_device_count) {
        s_swept = true;
    }
}

static void poller_task(void *arg)
{
    (void)arg;
    int64_t next_sweep_us = 0;
    for (;;) {
        /* Credentials may have changed while this task was elsewhere; the
         * epoch check is cheap and the poke that wakes the queue early is
         * only an accelerator. */
        uint32_t loop_epoch = atomic_load(&s_credentials_epoch);
        if (epoch_work_begin(ACTIVE_POLLER, loop_epoch)) {
            reconcile_client();
            epoch_work_end(ACTIVE_POLLER);
        }

        if (adopt_pending()) {
            next_sweep_us = 0;
        }

        int64_t remaining_us = next_sweep_us - esp_timer_get_time();
        TickType_t wait = remaining_us <= 0 ? 0 : pdMS_TO_TICKS(remaining_us / 1000);

        command_t command;
        if (xQueueReceive(s_commands, &command, wait) == pdTRUE) {
            if (command.kind == CMD_ACTION) {
                if (s_network_up && s_client != NULL && s_entry_count > 0 &&
                    epoch_is_current(command.credentials_epoch) &&
                    esp_timer_get_time() < command.deadline_us) {
                    if (epoch_work_begin(ACTIVE_POLLER, command.credentials_epoch)) {
                        run_action(&command);
                        epoch_work_end(ACTIVE_POLLER);
                    }
                } else {
                    if (command.credentials_epoch == atomic_load(&s_credentials_epoch)) {
                        slate_action_result(SLATE_TUYA_PROVIDER_ID, command.action_id,
                                            false, "offline");
                    }
                }
            }
            continue; /* drain the queue before spending time on a sweep */
        }

        if (s_entry_count > 0 && s_network_up && s_client != NULL) {
            uint32_t epoch = s_client_epoch;
            if (epoch_work_begin(ACTIVE_POLLER, epoch)) {
                sweep();
                update_status();
                epoch_work_end(ACTIVE_POLLER);
            }
        } else {
            update_status();
        }
        next_sweep_us = esp_timer_get_time() + (int64_t)POLL_INTERVAL_MS * 1000;
    }
}

/* --- The picker's catalog --------------------------------------------------- */

/*
 * The catalog builds its own client on a cold fetch. It could borrow the
 * poller's — the client takes its own mutex — but borrowing couples a picker
 * open to the sweep in flight and to every credentials change below, and the
 * one extra TLS handshake a minute is the cheaper of those two designs.
 */
static esp_err_t catalog_add(slate_resource_t *items, size_t *count,
                             const char *device_id, bool humidity,
                             slate_kind_t kind, const char *name,
                             const slate_tuya_dps_t *dps)
{
    if (*count >= CATALOG_RESOURCE_CAP) {
        return ESP_ERR_INVALID_SIZE;
    }
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    if (!encode_resource_id(device_id, humidity, resource, sizeof(resource))) {
        return ESP_ERR_INVALID_ARG;
    }
    slate_resource_t *out = &items[(*count)++];
    memset(out, 0, sizeof(*out));
    snprintf(out->provider, sizeof(out->provider), "%s", SLATE_TUYA_PROVIDER_ID);
    snprintf(out->resource, sizeof(out->resource), "%s", resource);
    out->kind = kind;
    out->presentation = SLATE_PRESENT_OK;
    if (name != NULL && name[0] != '\0') {
        snprintf(out->name, sizeof(out->name), "%s", name);
    }
    switch (kind) {
    case SLATE_KIND_LIGHT:
        out->state.light.brightness = SLATE_STATE_ABSENT;
        out->state.light.color_temperature = SLATE_STATE_ABSENT;
        if (dps->can_switch) {
            out->capabilities.actions = (uint16_t)((1u << SLATE_ACTION_TOGGLE) |
                                                   (1u << SLATE_ACTION_SET_POWER));
        }
        if (dps->has_bright) {
            out->capabilities.brightness_max = 100;
            if (dps->can_bright) {
                out->capabilities.actions |= (uint16_t)(1u << SLATE_ACTION_SET_BRIGHTNESS);
            }
        }
        if (dps->has_temp) {
            out->capabilities.color_temperature_min = 2700;
            out->capabilities.color_temperature_max = 6500;
            if (dps->can_temp) {
                out->capabilities.actions |=
                    (uint16_t)(1u << SLATE_ACTION_SET_COLOR_TEMPERATURE);
            }
        }
        break;
    case SLATE_KIND_COVER:
        out->state.cover.position = SLATE_STATE_ABSENT;
        if (dps->has_control) {
            out->capabilities.actions = (uint16_t)((1u << SLATE_ACTION_OPEN) |
                                                   (1u << SLATE_ACTION_STOP) |
                                                   (1u << SLATE_ACTION_CLOSE));
        }
        if (dps->has_percent_control) {
            out->capabilities.actions |= (uint16_t)(1u << SLATE_ACTION_SET_POSITION);
            out->capabilities.position_max = 100;
        }
        break;
    case SLATE_KIND_SENSOR:
        out->state.sensor.numeric = true;
        if (dps->cls == SLATE_TUYA_CLASS_SENSOR_POWER) {
            out->state.sensor.measurement = SLATE_MEASUREMENT_POWER;
            strcpy(out->state.sensor.unit, "W");
        } else if (humidity) {
            out->state.sensor.measurement = SLATE_MEASUREMENT_HUMIDITY;
            strcpy(out->state.sensor.unit, "%");
        } else {
            out->state.sensor.measurement = SLATE_MEASUREMENT_TEMPERATURE;
            strcpy(out->state.sensor.unit, "°C");
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t fetch_catalog(uint32_t epoch, slate_resource_t **out_items,
                               size_t *out_count, slate_tuya_error_kind_t *out_kind)
{
    *out_items = NULL;
    *out_count = 0;
    *out_kind = SLATE_TUYA_ERR_NONE;
    if (epoch != atomic_load(&s_credentials_epoch)) {
        return ESP_ERR_INVALID_STATE;
    }
    char region[SLATE_TUYA_REGION_MAX_LEN + 1] = {0};
    char access_id[SLATE_TUYA_ACCESS_ID_MAX_LEN + 1] = {0};
    char secret[SLATE_TUYA_SECRET_MAX_LEN + 1] = {0};
    char uid[SLATE_TUYA_UID_MAX_LEN + 1] = {0};
    esp_err_t err = slate_store_tuya_get(region, sizeof(region), access_id,
                                         sizeof(access_id), secret, sizeof(secret),
                                         uid, sizeof(uid));
    slate_tuya_client_handle_t client = NULL;
    cJSON *devices = NULL;
    slate_resource_t *items = NULL;
    size_t count = 0;
    if (err == ESP_OK) {
        err = slate_tuya_client_create(region, access_id, secret, uid, &client);
    }
    explicit_bzero(secret, sizeof(secret));
    if (err == ESP_OK) {
        char path[SLATE_TUYA_UID_MAX_LEN + 32];
        snprintf(path, sizeof(path), "/v1.0/users/%s/devices", uid);
        err = slate_tuya_client_request(client, HTTP_METHOD_GET, path, NULL, &devices,
                                        out_kind);
    }
    if (err == ESP_OK && !cJSON_IsArray(devices)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        items = heap_caps_calloc(CATALOG_RESOURCE_CAP, sizeof(*items), MALLOC_CAP_SPIRAM);
        if (items == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        if (err != ESP_OK || epoch != atomic_load(&s_credentials_epoch)) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *category = cJSON_GetObjectItemCaseSensitive(item, "category");
        const cJSON *display = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!cJSON_IsString(id) || id->valuestring[0] == '\0' ||
            !cJSON_IsString(category)) {
            continue;
        }
        slate_tuya_class_t cls = slate_tuya_classify(category->valuestring);
        if (cls == SLATE_TUYA_CLASS_UNSUPPORTED) {
            continue;
        }
        char cloud_id[SLATE_RESOURCE_ID_MAX + 1];
        bool synthetic = false;
        if (!decode_resource_id(id->valuestring, cloud_id, sizeof(cloud_id), &synthetic) ||
            synthetic) {
            ESP_LOGW(TAG, "catalog omitted malformed device id");
            continue;
        }
        char path[SLATE_RESOURCE_ID_MAX + 40];
        snprintf(path, sizeof(path), "/v1.1/devices/%s/specifications", cloud_id);
        cJSON *specification = NULL;
        err = slate_tuya_client_request(client, HTTP_METHOD_GET, path, NULL,
                                        &specification, out_kind);
        if (err != ESP_OK) {
            cJSON_Delete(specification);
            break;
        }
        const cJSON *functions =
            cJSON_GetObjectItemCaseSensitive(specification, "functions");
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(specification, "status");
        slate_tuya_dps_t dps;
        esp_err_t mapped =
            slate_tuya_map_specification(category->valuestring, functions, status, &dps);
        cJSON_Delete(specification);
        if (mapped == ESP_ERR_NOT_SUPPORTED) {
            continue;
        }
        if (mapped != ESP_OK) {
            err = mapped;
            break;
        }
        const char *shown =
            cJSON_IsString(display) && display->valuestring[0] != '\0'
                ? display->valuestring
                : id->valuestring;
        if (dps.cls == SLATE_TUYA_CLASS_LIGHT && dps.has_switch) {
            err = catalog_add(items, &count, cloud_id, false, SLATE_KIND_LIGHT, shown, &dps);
        } else if (dps.cls == SLATE_TUYA_CLASS_COVER &&
                   (dps.has_control || dps.has_percent_state)) {
            err = catalog_add(items, &count, cloud_id, false, SLATE_KIND_COVER, shown, &dps);
        } else if (dps.cls == SLATE_TUYA_CLASS_SENSOR_POWER && dps.has_power) {
            err = catalog_add(items, &count, cloud_id, false, SLATE_KIND_SENSOR, shown, &dps);
        } else if (dps.cls == SLATE_TUYA_CLASS_SENSOR_TEMP_HUM) {
            if (dps.has_temperature) {
                err = catalog_add(items, &count, cloud_id, false, SLATE_KIND_SENSOR,
                                  shown, &dps);
            }
            if (err == ESP_OK && dps.has_humidity) {
                char humidity_name[SLATE_RESOURCE_NAME_MAX + 1];
                snprintf(humidity_name, sizeof(humidity_name), "%s humidity", shown);
                err = catalog_add(items, &count, cloud_id, true, SLATE_KIND_SENSOR,
                                  humidity_name, &dps);
                if (err == ESP_ERR_INVALID_ARG) {
                    ESP_LOGW(TAG, "catalog omitted humidity id that exceeds the resource bound");
                    err = ESP_OK;
                }
            }
        }
        taskYIELD();
    }

    cJSON_Delete(devices);
    if (client != NULL) {
        slate_tuya_client_destroy(client);
    }
    if (err != ESP_OK) {
        free(items);
        return err;
    }
    *out_items = items;
    *out_count = count;
    return ESP_OK;
}

static void catalog_task(void *arg)
{
    (void)arg;
    catalog_job_t job;
    for (;;) {
        if (xQueueReceive(s_catalog_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!epoch_work_begin(ACTIVE_CATALOG, job.credentials_epoch)) {
            continue;
        }
        slate_resource_t *items = NULL;
        size_t count = 0;
        slate_tuya_error_kind_t kind = SLATE_TUYA_ERR_NONE;
        esp_err_t err = fetch_catalog(job.credentials_epoch, &items, &count, &kind);
        xSemaphoreTake(s_catalog.lock, portMAX_DELAY);
        bool current = job.credentials_epoch == atomic_load(&s_credentials_epoch) &&
                       job.credentials_epoch == s_catalog.credentials_epoch;
        if (current && err == ESP_OK) {
            free(s_catalog.items);
            s_catalog.items = items;
            items = NULL;
            s_catalog.count = count;
            s_catalog.fetched_us = esp_timer_get_time();
            s_catalog.state = SLATE_API_CATALOG_READY;
            slate_state_provider_set_status(SLATE_TUYA_PROVIDER_ID,
                                            SLATE_PROVIDER_ONLINE);
        } else if (current) {
            s_catalog.state = SLATE_API_CATALOG_ERROR;
            classify_failure(kind == SLATE_TUYA_ERR_NONE ? SLATE_TUYA_ERR_API : kind);
            if (kind != SLATE_TUYA_ERR_NONE) {
                ESP_LOGW(TAG, "catalog refresh failed (kind %d)", (int)kind);
            }
        }
        bool retry_current = false;
        if (s_catalog.refresh_epoch == job.credentials_epoch) {
            s_catalog.refresh_pending = false;
        }
        if (!current && slate_store_tuya_is_set() && !s_catalog.refresh_pending) {
            retry_current = true;
        }
        xSemaphoreGive(s_catalog.lock);
        free(items);
        if (retry_current) {
            request_catalog_refresh();
        }
        epoch_work_end(ACTIVE_CATALOG);
    }
}

static void request_catalog_refresh(void)
{
    if (s_catalog.lock == NULL || s_catalog_jobs == NULL) {
        return;
    }
    catalog_job_t job = {.credentials_epoch = atomic_load(&s_credentials_epoch)};
    xSemaphoreTake(s_catalog.lock, portMAX_DELAY);
    if (s_catalog.refresh_pending || job.credentials_epoch != s_catalog.credentials_epoch) {
        xSemaphoreGive(s_catalog.lock);
        return;
    }
    s_catalog.refresh_pending = true;
    s_catalog.refresh_epoch = job.credentials_epoch;
    s_catalog.state = SLATE_API_CATALOG_LOADING;
    xSemaphoreGive(s_catalog.lock);
    if (xQueueSend(s_catalog_jobs, &job, 0) != pdTRUE) {
        xSemaphoreTake(s_catalog.lock, portMAX_DELAY);
        s_catalog.refresh_pending = false;
        s_catalog.state = SLATE_API_CATALOG_ERROR;
        xSemaphoreGive(s_catalog.lock);
    }
}

static esp_err_t catalog_append(void *ctx, cJSON *array,
                                slate_api_catalog_state_t *state)
{
    (void)ctx;
    if (!slate_store_tuya_is_set()) {
        return ESP_ERR_INVALID_STATE;
    }
    bool refresh = false;
    xSemaphoreTake(s_catalog.lock, portMAX_DELAY);
    if (s_catalog.state == SLATE_API_CATALOG_EMPTY ||
        s_catalog.state == SLATE_API_CATALOG_ERROR ||
        (s_catalog.state == SLATE_API_CATALOG_READY &&
         esp_timer_get_time() - s_catalog.fetched_us >=
             (int64_t)CATALOG_FRESH_MS * 1000)) {
        refresh = !s_catalog.refresh_pending;
    }
    if (refresh && s_catalog.state != SLATE_API_CATALOG_ERROR) {
        s_catalog.state = SLATE_API_CATALOG_LOADING;
    }
    esp_err_t err = ESP_OK;
    for (size_t i = 0; err == ESP_OK && i < s_catalog.count; i++) {
        err = slate_api_resource_append(array, &s_catalog.items[i]);
    }
    *state = s_catalog.state;
    xSemaphoreGive(s_catalog.lock);
    if (refresh) {
        request_catalog_refresh();
    }
    return err;
}

/* --- Device API ------------------------------------------------------------- */

static const char *read_body(httpd_req_t *req, char *buffer, size_t size)
{
    if (req->content_len == 0) {
        return "empty_body";
    }
    if (req->content_len >= size) {
        return "too_large";
    }
    size_t received = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (chunk <= 0) {
            return "truncated";
        }
        received += (size_t)chunk;
    }
    buffer[received] = '\0';
    return NULL;
}

static void wipe_job(configure_job_t *job)
{
    explicit_bzero(job, sizeof(*job));
}

/*
 * The region list this route accepts and the endpoint table the client keeps
 * are the same six data centres; a seventh added to one is a bug in the other
 * until this list learns it.
 */
static bool region_known(const char *region)
{
    static const char *const REGIONS[] = {"us", "eu", "cn", "in", "ueaz", "weaz"};
    for (size_t i = 0; i < sizeof(REGIONS) / sizeof(REGIONS[0]); i++) {
        if (strcmp(REGIONS[i], region) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t configuration_handler(httpd_req_t *req)
{
    /* The store reads the set whole or not at all, so the two fields this
     * route may show are read alongside the two it must not: into stack
     * scratch that is wiped on the way out, secret included. */
    char region[SLATE_TUYA_REGION_MAX_LEN + 1] = {0};
    char access_id[SLATE_TUYA_ACCESS_ID_MAX_LEN + 1] = {0};
    char secret[SLATE_TUYA_SECRET_MAX_LEN + 1] = {0};
    char uid[SLATE_TUYA_UID_MAX_LEN + 1] = {0};
    bool configured = slate_store_tuya_is_set() &&
                      slate_store_tuya_get(region, sizeof(region), access_id,
                                           sizeof(access_id), secret, sizeof(secret),
                                           uid, sizeof(uid)) == ESP_OK;

    cJSON *root = cJSON_CreateObject();
    bool ok = root != NULL &&
              cJSON_AddBoolToObject(root, "configured", configured) != NULL &&
              cJSON_AddItemToObject(
                  root, "region",
                  configured ? cJSON_CreateString(region) : cJSON_CreateNull()) &&
              cJSON_AddItemToObject(
                  root, "uid",
                  configured ? cJSON_CreateString(uid) : cJSON_CreateNull());
    explicit_bzero(access_id, sizeof(access_id));
    explicit_bzero(secret, sizeof(secret));
    if (!ok) {
        cJSON_Delete(root);
        return slate_api_send_json(req, NULL);
    }
    return slate_api_send_json(req, root);
}

static esp_err_t configure_handler(httpd_req_t *req)
{
    uint32_t intent_epoch = mutation_begin();
    char body[ROUTE_BODY_MAX];
    const char *problem = read_body(req, body, sizeof(body));
    if (problem != NULL) {
        explicit_bzero(body, sizeof(body));
        return slate_api_refuse(req,
                                strcmp(problem, "too_large") == 0
                                    ? "413 Payload Too Large"
                                    : "400 Bad Request",
                                problem);
    }

    cJSON *root = cJSON_Parse(body);
    explicit_bzero(body, sizeof(body));
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return slate_api_refuse(req, "400 Bad Request", "invalid_json");
    }

    const cJSON *region = cJSON_GetObjectItemCaseSensitive(root, "region");
    const cJSON *access_id = cJSON_GetObjectItemCaseSensitive(root, "access_id");
    const cJSON *secret = cJSON_GetObjectItemCaseSensitive(root, "secret");
    const cJSON *uid = cJSON_GetObjectItemCaseSensitive(root, "uid");
    const char *error = NULL;
    if (!cJSON_IsString(region) || region->valuestring[0] == '\0') {
        error = "region_required";
    } else if (!region_known(region->valuestring)) {
        error = "region_unknown";
    } else if (!cJSON_IsString(access_id) || access_id->valuestring[0] == '\0') {
        error = "access_id_required";
    } else if (strlen(access_id->valuestring) > SLATE_TUYA_ACCESS_ID_MAX_LEN) {
        error = "access_id_too_long";
    } else if (!cJSON_IsString(secret) || secret->valuestring[0] == '\0') {
        error = "secret_required";
    } else if (strlen(secret->valuestring) > SLATE_TUYA_SECRET_MAX_LEN) {
        error = "secret_too_long";
    } else if (!cJSON_IsString(uid) || uid->valuestring[0] == '\0') {
        error = "uid_required";
    } else if (strlen(uid->valuestring) > SLATE_TUYA_UID_MAX_LEN) {
        error = "uid_too_long";
    }

    configure_job_t *job = NULL;
    if (error == NULL) {
        job = calloc(1, sizeof(*job));
        if (job != NULL) {
            job->credentials_epoch = intent_epoch;
            snprintf(job->region, sizeof(job->region), "%s", region->valuestring);
            snprintf(job->access_id, sizeof(job->access_id), "%s", access_id->valuestring);
            snprintf(job->secret, sizeof(job->secret), "%s", secret->valuestring);
            snprintf(job->uid, sizeof(job->uid), "%s", uid->valuestring);
        }
    }

    if (cJSON_IsString(access_id) && access_id->valuestring != NULL) {
        explicit_bzero(access_id->valuestring, strlen(access_id->valuestring));
    }
    if (cJSON_IsString(secret) && secret->valuestring != NULL) {
        explicit_bzero(secret->valuestring, strlen(secret->valuestring));
    }
    cJSON_Delete(root);
    if (error != NULL) {
        free(job);
        return slate_api_refuse(req, "400 Bad Request", error);
    }
    if (job == NULL) {
        return slate_api_send_json(req, NULL);
    }

    /* The cloud test takes seconds; the HTTP task is parked rather than
     * held, exactly as the Home Assistant credential test does. */
    esp_err_t err = httpd_req_async_handler_begin(req, &job->request);
    if (err != ESP_OK) {
        wipe_job(job);
        free(job);
        return slate_api_send_json(req, NULL);
    }
    if (xQueueSend(s_config_jobs, &job, 0) != pdTRUE) {
        slate_api_refuse(job->request, "503 Service Unavailable", "tuya_busy");
        httpd_req_async_handler_complete(job->request);
        wipe_job(job);
        free(job);
    }
    return ESP_OK;
}

/*
 * A dedicated task, because the credential test is a full TLS handshake to a
 * data centre and the HTTP task has other requests. On success the store is
 * written and the epoch bumped; the poller notices on its next pass, and the
 * CMD_WAKE poke only decides whether that is now or in a few seconds.
 */
static void configure_task(void *arg)
{
    (void)arg;
    configure_job_t *job;
    for (;;) {
        if (xQueueReceive(s_config_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!epoch_work_begin(ACTIVE_CONFIG, job->credentials_epoch)) {
            slate_api_refuse(job->request, "409 Conflict", "stale_configuration");
            httpd_req_async_handler_complete(job->request);
            wipe_job(job);
            free(job);
            continue;
        }
        slate_tuya_error_kind_t kind = SLATE_TUYA_ERR_NONE;
        esp_err_t tested =
            slate_tuya_client_test(job->region, job->access_id, job->secret,
                                   job->uid, &kind);
        esp_err_t stored = ESP_OK;
        xSemaphoreTake(s_mutation_lock, portMAX_DELAY);
        bool current = job->credentials_epoch == atomic_load(&s_credentials_epoch);
        if (tested == ESP_OK && current) {
            stored = slate_store_tuya_set(job->region, job->access_id, job->secret,
                                          job->uid);
        }
        xSemaphoreGive(s_mutation_lock);

        if (!current) {
            slate_api_refuse(job->request, "409 Conflict", "stale_configuration");
        } else if (tested != ESP_OK && kind == SLATE_TUYA_ERR_AUTH) {
            slate_api_refuse(job->request, "422 Unprocessable Content",
                             "tuya_auth_failed");
        } else if (tested != ESP_OK && kind == SLATE_TUYA_ERR_QUOTA) {
            slate_api_refuse(job->request, "422 Unprocessable Content", "tuya_quota");
        } else if (tested != ESP_OK) {
            slate_api_refuse(job->request, "502 Bad Gateway", "tuya_unreachable");
        } else if (stored != ESP_OK) {
            slate_api_refuse(job->request, "500 Internal Server Error", "store_failed");
        } else {
            slate_state_provider_set_status(SLATE_TUYA_PROVIDER_ID,
                                            SLATE_PROVIDER_CONNECTING);
            if (s_commands != NULL) {
                const command_t wake = {.kind = CMD_WAKE};
                xQueueSend(s_commands, &wake, 0);
            }
            /* Drop any cached catalog from the previous project: those names
             * and categories belong to credentials that just stopped being
             * the ones in the store. */
            xSemaphoreTake(s_catalog.lock, portMAX_DELAY);
            free(s_catalog.items);
            s_catalog.items = NULL;
            s_catalog.count = 0;
            s_catalog.fetched_us = 0;
            s_catalog.credentials_epoch = atomic_load(&s_credentials_epoch);
            s_catalog.state = SLATE_API_CATALOG_EMPTY;
            s_catalog.refresh_pending = false;
            xSemaphoreGive(s_catalog.lock);
            httpd_resp_set_status(job->request, "204 No Content");
            httpd_resp_send(job->request, NULL, 0);
        }

        httpd_req_async_handler_complete(job->request);
        epoch_work_end(ACTIVE_CONFIG);
        wipe_job(job);
        free(job);
    }
}

static esp_err_t disconnect_handler(httpd_req_t *req)
{
    uint32_t intent_epoch = mutation_begin();
    slate_action_provider_unavailable(SLATE_TUYA_PROVIDER_ID, "unconfigured");
    if (s_commands != NULL) {
        xQueueReset(s_commands);
    }
    if (s_catalog_jobs != NULL) {
        xQueueReset(s_catalog_jobs);
    }
    mutation_barrier(intent_epoch);
    xSemaphoreTake(s_mutation_lock, portMAX_DELAY);
    esp_err_t err = intent_epoch == atomic_load(&s_credentials_epoch)
                        ? slate_store_tuya_clear()
                        : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_mutation_lock);
    if (err == ESP_ERR_INVALID_STATE) {
        return slate_api_refuse(req, "409 Conflict", "stale_configuration");
    }
    if (err != ESP_OK) {
        return slate_api_refuse(req, "500 Internal Server Error", "store_failed");
    }
    slate_state_provider_set_status(SLATE_TUYA_PROVIDER_ID,
                                    SLATE_PROVIDER_UNCONFIGURED);
    slate_action_provider_unavailable(SLATE_TUYA_PROVIDER_ID, "unconfigured");
    if (s_commands != NULL) {
        const command_t wake = {.kind = CMD_WAKE};
        xQueueSend(s_commands, &wake, 0);
    }
    xSemaphoreTake(s_catalog.lock, portMAX_DELAY);
    free(s_catalog.items);
    s_catalog.items = NULL;
    s_catalog.count = 0;
    s_catalog.fetched_us = 0;
    s_catalog.credentials_epoch = atomic_load(&s_credentials_epoch);
    s_catalog.state = SLATE_API_CATALOG_EMPTY;
    s_catalog.refresh_pending = false;
    xSemaphoreGive(s_catalog.lock);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* --- Provider callbacks ------------------------------------------------------ */

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
        char base[SLATE_RESOURCE_ID_MAX + 1];
        bool humidity = false;
        if (!decode_resource_id(resources[i], base, sizeof(base), &humidity)) {
            /* Not an error the rebuild can act on (§5.1); the binding renders
             * as a resource that never arrived, which is what it is. */
            ESP_LOGW(TAG, "ignoring unparseable resource id %s", resources[i]);
            continue;
        }
        size_t device = SIZE_MAX;
        for (size_t d = 0; d < device_count; d++) {
            if (strcmp(devices[d].id, base) == 0) {
                device = d;
                break;
            }
        }
        if (device == SIZE_MAX) {
            if (strlen(base) > SLATE_RESOURCE_ID_MAX) {
                continue; /* the store already refused it; belt and braces */
            }
            device = device_count++;
            snprintf(devices[device].id, sizeof(devices[device].id), "%s", base);
        }
        entry_t *entry = &entries[entry_count++];
        snprintf(entry->id, sizeof(entry->id), "%s", resources[i]);
        entry->device = (uint16_t)device;
        entry->humidity = humidity;
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

    /* Best effort: the poller adopts the set at the top of its next pass
     * regardless; this only decides whether that happens now. */
    if (s_commands != NULL) {
        const command_t poke = {.kind = CMD_SWEEP};
        xQueueSend(s_commands, &poke, 0);
    }
    return ESP_OK;
}

static bool action_supported(slate_action_t action)
{
    switch (action) {
    case SLATE_ACTION_TOGGLE:
    case SLATE_ACTION_SET_POWER:
    case SLATE_ACTION_SET_BRIGHTNESS:
    case SLATE_ACTION_SET_COLOR_TEMPERATURE:
    case SLATE_ACTION_OPEN:
    case SLATE_ACTION_STOP:
    case SLATE_ACTION_CLOSE:
    case SLATE_ACTION_SET_POSITION:
        return true;
    default:
        return false;
    }
}

static esp_err_t dispatch(void *ctx, uint32_t id, const slate_action_request_t *request)
{
    (void)ctx;
    /* The provider registers in init() and the queue is created in start(),
     * so a panel whose poller failed to start has a clickable tile and no
     * consumer behind it; refusing here is §5.3's immediate revert. */
    if (s_commands == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!slate_store_tuya_is_set() || !s_network_up ||
        slate_state_provider_status(SLATE_TUYA_PROVIDER_ID) == SLATE_PROVIDER_UNCONFIGURED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!action_supported(request->action)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->action == SLATE_ACTION_SET_POWER &&
        request->value_type != SLATE_ACTION_VALUE_BOOL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((request->action == SLATE_ACTION_SET_BRIGHTNESS ||
         request->action == SLATE_ACTION_SET_COLOR_TEMPERATURE ||
         request->action == SLATE_ACTION_SET_POSITION) &&
        request->value_type != SLATE_ACTION_VALUE_NUMBER) {
        return ESP_ERR_INVALID_ARG;
    }

    command_t command = {
        .kind = CMD_ACTION,
        .action_id = id,
        .action = request->action,
        .value_type = request->value_type,
        .boolean = request->value_type == SLATE_ACTION_VALUE_BOOL && request->value.boolean,
        .number = request->value_type == SLATE_ACTION_VALUE_NUMBER ? request->value.number : 0,
        .credentials_epoch = atomic_load(&s_credentials_epoch),
        .deadline_us = esp_timer_get_time() + 2800000,
    };
    snprintf(command.resource, sizeof(command.resource), "%s", request->resource);

    /* Never block the bus: a full queue is a device already several taps
     * behind rather than something to wait for. */
    /* Put a tap ahead of periodic sweep work. The action bus owns the visible
     * three-second timeout; the copied deadline prevents late queue work from
     * reaching the cloud or acknowledging a stale action. */
    if (xQueueSendToFront(s_commands, &command, 0) != pdTRUE) {
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
        if (slate_store_tuya_is_set()) {
            slate_state_provider_set_status(SLATE_TUYA_PROVIDER_ID,
                                            SLATE_PROVIDER_CONNECTING);
        }
    } else if (id == SLATE_WIFI_EVENT_DISCONNECTED) {
        s_network_up = false;
        slate_action_provider_unavailable(SLATE_TUYA_PROVIDER_ID, "offline");
    } else {
        return;
    }
    /* A loss stales this provider's resources at once rather than at the end
     * of the current interval; the recovery is the task's to report. */
    if (!s_network_up && slate_store_tuya_is_set()) {
        slate_state_provider_set_status(SLATE_TUYA_PROVIDER_ID, SLATE_PROVIDER_OFFLINE);
    }
}

/* --- Lifecycle ---------------------------------------------------------------- */

esp_err_t slate_tuya_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_bind_lock = xSemaphoreCreateMutexStatic(&s_bind_lock_storage);
    if (s_bind_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_mutation_lock = xSemaphoreCreateMutexStatic(&s_mutation_lock_storage);
    if (s_mutation_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < ACTIVE_COUNT; i++) {
        atomic_store(&s_active_epoch[i], EPOCH_INACTIVE);
    }
    s_catalog.lock = xSemaphoreCreateMutex();
    if (s_catalog.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_catalog.credentials_epoch = atomic_load(&s_credentials_epoch);
    s_catalog.state = SLATE_API_CATALOG_EMPTY;

    s_catalog_jobs = xQueueCreate(CATALOG_QUEUE_LEN, sizeof(catalog_job_t));
    if (s_catalog_jobs == NULL ||
        xTaskCreate(catalog_task, "slate_tuya_cat", TASK_STACK, NULL, TASK_PRIORITY,
                    &s_catalog_task) != pdPASS) {
        if (s_catalog_jobs != NULL) {
            vQueueDelete(s_catalog_jobs);
            s_catalog_jobs = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    s_config_jobs = xQueueCreate(2, sizeof(configure_job_t *));
    TaskHandle_t configure_handle = NULL;
    if (s_config_jobs == NULL ||
        xTaskCreate(configure_task, "slate_tuya_cfg", TASK_STACK, NULL,
                    TASK_PRIORITY, &configure_handle) != pdPASS) {
        if (s_config_jobs != NULL) {
            vQueueDelete(s_config_jobs);
            s_config_jobs = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    const slate_state_provider_t provider = {
        .id = SLATE_TUYA_PROVIDER_ID,
        .subscribe = subscribe,
    };
    esp_err_t err = slate_state_provider_register(&provider);
    if (err != ESP_OK) {
        return err;
    }
    slate_state_provider_set_status(SLATE_TUYA_PROVIDER_ID, SLATE_PROVIDER_UNCONFIGURED);

    const slate_action_provider_t action_provider = {
        .id = SLATE_TUYA_PROVIDER_ID,
        .dispatch = dispatch,
    };
    esp_err_t action_err = slate_action_provider_register(&action_provider);
    if (action_err != ESP_OK) {
        return action_err;
    }

    /* The catalog goes in front of /resources rather than into a route of its
     * own: the editor's picker already knows how to ask a provider for its
     * devices, and a second catalog protocol is a second thing to drift. */
    err = slate_api_resources_register(SLATE_TUYA_PROVIDER_ID, catalog_append, NULL);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t configure = {
        .uri = SLATE_API_BASE_PATH "/tuya",
        .method = HTTP_POST,
        .handler = configure_handler,
    };
    const httpd_uri_t configuration = {
        .uri = SLATE_API_BASE_PATH "/tuya",
        .method = HTTP_GET,
        .handler = configuration_handler,
    };
    const httpd_uri_t disconnect = {
        .uri = SLATE_API_BASE_PATH "/tuya",
        .method = HTTP_DELETE,
        .handler = disconnect_handler,
    };
    esp_err_t route_err = slate_api_register_uri(&configure, SLATE_API_AUTH_DEVICE_TOKEN);
    if (route_err == ESP_OK) {
        route_err = slate_api_register_uri(&configuration, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (route_err == ESP_OK) {
        route_err = slate_api_register_uri(&disconnect, SLATE_API_AUTH_DEVICE_TOKEN);
    }
    if (route_err != ESP_OK) {
        return route_err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "provider ready: POST " SLATE_API_BASE_PATH "/tuya");
    return ESP_OK;
}

esp_err_t slate_tuya_start(void)
{
    if (!s_initialized || s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_commands = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(command_t));
    if (s_commands == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_event_handler_instance_register(SLATE_WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event, NULL, NULL);
    if (err != ESP_OK) {
        vQueueDelete(s_commands);
        s_commands = NULL;
        return err;
    }

    slate_wifi_status_t wifi;
    slate_wifi_status(&wifi);
    s_network_up = wifi.connected;

    if (xTaskCreate(poller_task, "slate_tuya", TASK_STACK, NULL, TASK_PRIORITY,
                    &s_task) != pdPASS) {
        /* Unwind everything, and `s_commands` above all: a queue left behind
         * none of them, which §5.3 makes worse than refusing them outright. */
        esp_event_handler_unregister(SLATE_WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event);
        vQueueDelete(s_commands);
        s_commands = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#ifdef SLATE_TUYA_SELFTEST

/*
 * What only this component can verify without a cloud: the resource-id split
 * (the map module selects the reading; this component decides which resource
 * asked for it), the seam fixtures through the live store registration, and
 * the status the store holds. The DP vocabulary itself is slate_tuya_map's
 * host test.
 */
esp_err_t slate_tuya_selftest(void)
{
    int failures = 0;
#define CHECK(condition, name)                                                \
    do {                                                                      \
        bool passed_ = (condition);                                           \
        failures += !passed_;                                                 \
        ESP_LOGI(TAG, "selftest: %-48s %s", name, passed_ ? "PASS" : "FAIL"); \
    } while (0)

    CHECK(slate_tuya_client_selftest() == ESP_OK,
          "the Tuya signing and envelope fixtures pass");
    CHECK(slate_tuya_map_selftest() == ESP_OK,
          "the Tuya status/function mapping fixtures pass");

    char base[SLATE_RESOURCE_ID_MAX + 1];
    bool humidity = true;
    CHECK(decode_resource_id("bf2c1e00ab0f12face", base, sizeof(base), &humidity) &&
              strcmp(base, "bf2c1e00ab0f12face") == 0 && !humidity,
          "a bare device id is not a humidity resource");
    CHECK(decode_resource_id("bf2c1e00ab0f12face" SLATE_TUYA_HUMIDITY_SUFFIX,
                             base, sizeof(base), &humidity) && humidity &&
              strcmp(base, "bf2c1e00ab0f12face") == 0,
          "the humidity suffix is removed from the cloud id");
    CHECK(!decode_resource_id("", base, sizeof(base), &humidity),
          "an empty id is refused");
    CHECK(!decode_resource_id("device/temperature", base, sizeof(base), &humidity),
          "an unknown suffix is refused");
    char longest_device[SLATE_RESOURCE_ID_MAX + 1];
    memset(longest_device, 'a', sizeof(longest_device) - 1);
    longest_device[sizeof(longest_device) - 1] = '\0';
    char encoded[SLATE_RESOURCE_ID_MAX + 1];
    CHECK(encode_resource_id(longest_device, false, encoded, sizeof(encoded)) &&
              strcmp(longest_device, encoded) == 0,
          "a maximum-length cloud id round-trips exactly");
    CHECK(!encode_resource_id(longest_device, true, encoded, sizeof(encoded)),
          "an over-limit humidity id is refused without truncation");

    CHECK(slate_tuya_classify("dj") == SLATE_TUYA_CLASS_LIGHT &&
              slate_tuya_classify("cl") == SLATE_TUYA_CLASS_COVER &&
              slate_tuya_classify("wsdcg") == SLATE_TUYA_CLASS_SENSOR_TEMP_HUM &&
              slate_tuya_classify("nope") == SLATE_TUYA_CLASS_UNSUPPORTED,
          "the category table this provider's catalog depends on");

    /* One snapshot end to end: functions spec, status, and the store's
     * refusal of a resource nothing bound — the same invariant publish_entry
     * rests on. */
    static const char FUNCTIONS[] =
        "[{\"code\":\"switch_led\",\"type\":\"Boolean\",\"values\":\"{}\"},"
        "{\"code\":\"bright_value_v2\",\"type\":\"Integer\","
        "\"values\":\"{\\\"min\\\":10,\\\"max\\\":1000,\\\"scale\\\":0,\\\"step\\\":10}\"}]";
    cJSON *functions = cJSON_Parse(FUNCTIONS);
    slate_tuya_dps_t dps;
    CHECK(functions != NULL &&
              slate_tuya_map_functions("dj", functions, &dps) == ESP_OK &&
              dps.cls == SLATE_TUYA_CLASS_LIGHT && dps.has_switch && dps.has_bright,
          "a light functions spec maps");
    static const char STATUS[] =
        "[{\"code\":\"switch_led\",\"value\":true},"
        "{\"code\":\"bright_value_v2\",\"value\":500}]";
    cJSON *status = cJSON_Parse(STATUS);
    slate_snapshot_t snapshot = {0};
    CHECK(status != NULL &&
              slate_tuya_map_status(&dps, status, "bf2c1e00ab0f12face", "Desk",
                                    &snapshot) == ESP_OK &&
              snapshot.kind == SLATE_KIND_LIGHT && snapshot.state.light.on &&
              snapshot.state.light.brightness == 50,
          "a light status maps with its brightness scaled");
    CHECK(slate_state_publish(SLATE_TUYA_PROVIDER_ID, &snapshot) == ESP_ERR_NOT_FOUND,
          "the store refuses a resource nothing bound");
    cJSON_Delete(functions);
    cJSON_Delete(status);

    /* The handover: dedupe by base device, humidity as a second entry, and
     * the carry-over of what a republish should not forget. */
    static const char *const PAIR[] = {"bf2c1e00ab0f12face",
                                       "bf2c1e00ab0f12face" SLATE_TUYA_HUMIDITY_SUFFIX,
                                       "bf9d33c2d00440aabbccdd"};
    CHECK(subscribe(NULL, PAIR, 3) == ESP_OK && adopt_pending() &&
              s_entry_count == 3 && s_device_count == 2 &&
              s_entries[0].device == 0 && s_entries[1].device == 0 &&
              s_entries[1].humidity && s_entries[2].device == 1,
          "three resources, two devices: the humidity pair shares a device");
    CHECK(!adopt_pending(), "a second adopt with nothing pending is a no-op");

    s_devices[0].reachable = true;
    s_devices[0].ever_reachable = true;
    s_entries[0].ever_read = true;
    s_entries[0].last.light.on = true;
    s_swept = true;
    CHECK(subscribe(NULL, PAIR, 3) == ESP_OK && adopt_pending() &&
              s_devices[0].reachable && s_entries[0].ever_read &&
              s_entries[0].last.light.on,
          "republishing keeps what was learned about its devices");
    CHECK(s_swept, "and does not drop the provider back to connecting");

    static const char *const JUNK[] = {"", "bf2c1e00ab0f12face"};
    CHECK(subscribe(NULL, JUNK, 2) == ESP_OK && adopt_pending() && s_entry_count == 1,
          "an unparseable id is dropped and the rest of the set survives");

    CHECK(subscribe(NULL, NULL, 0) == ESP_OK && adopt_pending() && s_entry_count == 0 &&
              s_device_count == 0 && s_devices == NULL && s_entries == NULL,
          "count zero releases the tables, per §5.1's unsubscribe");

#undef CHECK
    ESP_LOGI(TAG, "selftest: %d failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif /* SLATE_TUYA_SELFTEST */
