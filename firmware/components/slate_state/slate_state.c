/*
 * Slate — provider-neutral state store.
 *
 * See slate_state.h for the contract. The implementation notes here are the
 * ones that are about this file rather than about the design:
 *
 * ONE TABLE, REPLACED WHOLE. Everything the store holds lives in one
 * allocation sized by slate_state_bind(). There is no per-entry malloc, no
 * intrusive list and no growth path, which is what makes §6.4's "after 500
 * cycles the free LVGL heap returns to its starting value" checkable on this
 * side of the boundary too: one allocation in, one free out, and a rebuild that
 * leaked would show up as a single missing block rather than as a slow bleed.
 *
 * LOOKUP IS LINEAR. Twelve tiles per page (§3.2) and a few dozen resources
 * across a dashboard; a hash table would be more code to get wrong than a scan
 * over a contiguous array of fixed-size structs costs to run. If a configuration
 * ever makes this measurable, the fix is an index built inside bind(), not a
 * different data structure — the table's identity has to stay stable because
 * §5.4's publication path is what a script hits ten times a second.
 *
 * THE LOCK IS NEVER HELD ACROSS A CALLBACK. Providers subscribe outside it and
 * observers drain outside it. Both may call back into the store — an adapter
 * that publishes a cached value the moment it is subscribed is the obvious
 * one — and a lock held across somebody else's code is a deadlock waiting for
 * the first adapter that does something reasonable.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "slate_state.h"

static const char *TAG = "slate_state";

typedef struct {
    char id[SLATE_PROVIDER_ID_MAX + 1];
    slate_state_subscribe_fn subscribe;
    void *ctx;
    slate_provider_status_t status;
} provider_t;

/*
 * The fields through `state` form the published half exposed by
 * slate_resource_t; the rest is store bookkeeping. `mismatch` and
 * `mismatch_kind` outlive the publish that set them because §3.3's
 * incompatible-binding placeholder has to stay on screen after the refusal
 * has been answered, and `changed` is the coalescing the drain is built on.
 */
typedef struct {
    char provider[SLATE_PROVIDER_ID_MAX + 1];
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    slate_kind_t kind;
    slate_kind_t mismatch_kind;
    char name[SLATE_RESOURCE_NAME_MAX + 1];
    char area[SLATE_RESOURCE_AREA_MAX + 1];
    slate_capabilities_t capabilities;
    slate_state_value_t state;
    int64_t updated_us;
    bool available;
    bool mismatch;
    bool changed;
} entry_t;

static SemaphoreHandle_t s_lock;
static provider_t s_providers[SLATE_STATE_MAX_PROVIDERS];
static size_t s_provider_count;
static entry_t *s_entries;
static size_t s_count;
static slate_state_wake_fn s_wake;
static void *s_wake_ctx;
static slate_state_publish_observer_fn s_publish_observer;
static void *s_publish_observer_ctx;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* --- Vocabulary ---------------------------------------------------------- */

static const char *const KIND_NAMES[] = {
    [SLATE_KIND_LIGHT] = "light",
    [SLATE_KIND_COVER] = "cover",
    [SLATE_KIND_SENSOR] = "sensor",
    [SLATE_KIND_SCENE] = "scene",
};

static const char *const STATUS_NAMES[] = {
    [SLATE_PROVIDER_UNCONFIGURED] = "unconfigured",
    [SLATE_PROVIDER_CONNECTING] = "connecting",
    [SLATE_PROVIDER_ONLINE] = "online",
    [SLATE_PROVIDER_DEGRADED] = "degraded",
    [SLATE_PROVIDER_OFFLINE] = "offline",
    [SLATE_PROVIDER_ERROR] = "error",
};

static const char *const ACTION_NAMES[] = {
    [SLATE_ACTION_TOGGLE] = "toggle",
    [SLATE_ACTION_SET_POWER] = "set_power",
    [SLATE_ACTION_SET_BRIGHTNESS] = "set_brightness",
    [SLATE_ACTION_SET_COLOR_TEMPERATURE] = "set_color_temperature",
    [SLATE_ACTION_OPEN] = "open",
    [SLATE_ACTION_STOP] = "stop",
    [SLATE_ACTION_CLOSE] = "close",
    [SLATE_ACTION_SET_POSITION] = "set_position",
    [SLATE_ACTION_ACTIVATE] = "activate",
};

static const char *const MEASUREMENT_NAMES[] = {
    [SLATE_MEASUREMENT_NONE] = NULL,
    [SLATE_MEASUREMENT_TEMPERATURE] = "temperature",
    [SLATE_MEASUREMENT_HUMIDITY] = "humidity",
    [SLATE_MEASUREMENT_PRESSURE] = "pressure",
    [SLATE_MEASUREMENT_POWER] = "power",
};

static const char *const CATEGORY_NAMES[] = {
    [SLATE_CATEGORY_NONE] = NULL,
    [SLATE_CATEGORY_TEMPERATURE] = "temperature",
    [SLATE_CATEGORY_HUMIDITY] = "humidity",
    [SLATE_CATEGORY_PRESSURE] = "pressure",
    [SLATE_CATEGORY_POWER] = "power",
    [SLATE_CATEGORY_ILLUMINANCE] = "illuminance",
    [SLATE_CATEGORY_AIR_QUALITY] = "air_quality",
    [SLATE_CATEGORY_GAS] = "gas",
    [SLATE_CATEGORY_SOUND] = "sound",
    [SLATE_CATEGORY_SPEED] = "speed",
    [SLATE_CATEGORY_BATTERY] = "battery",
    [SLATE_CATEGORY_CONNECTIVITY] = "connectivity",
    [SLATE_CATEGORY_DOOR] = "door",
    [SLATE_CATEGORY_WINDOW] = "window",
    [SLATE_CATEGORY_GARAGE] = "garage",
    [SLATE_CATEGORY_MOTION] = "motion",
    [SLATE_CATEGORY_OCCUPANCY] = "occupancy",
    [SLATE_CATEGORY_MOISTURE] = "moisture",
    [SLATE_CATEGORY_SMOKE] = "smoke",
    [SLATE_CATEGORY_LOCK] = "lock",
    [SLATE_CATEGORY_PLUG] = "plug",
    [SLATE_CATEGORY_PROBLEM] = "problem",
    [SLATE_CATEGORY_RUNNING] = "running",
};

static const char *const PRESENTATION_NAMES[] = {
    [SLATE_PRESENT_OK] = "ok",
    [SLATE_PRESENT_STALE] = "stale",
    [SLATE_PRESENT_UNAVAILABLE] = "unavailable",
    [SLATE_PRESENT_MISSING] = "missing",
    [SLATE_PRESENT_MISSING_PROVIDER] = "missing_provider",
    [SLATE_PRESENT_INCOMPATIBLE] = "incompatible",
};

/*
 * Name tables are indexed by the enum, so a member added without a name would
 * read a NULL rather than fail to compile. One assertion per table is cheaper
 * than the log line that would otherwise be the first sign of it.
 */
_Static_assert(sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0]) == SLATE_KIND_SCENE + 1,
               "every kind needs a name");
_Static_assert(sizeof(STATUS_NAMES) / sizeof(STATUS_NAMES[0]) == SLATE_PROVIDER_ERROR + 1,
               "every provider status needs a name");
_Static_assert(sizeof(ACTION_NAMES) / sizeof(ACTION_NAMES[0]) == SLATE_ACTION_COUNT,
               "every action needs a name");
_Static_assert(sizeof(PRESENTATION_NAMES) / sizeof(PRESENTATION_NAMES[0]) ==
                   SLATE_PRESENT_INCOMPATIBLE + 1,
               "every presentation needs a name");
_Static_assert(SLATE_ACTION_COUNT <= 16, "the capability bitmask is 16 bits wide");

static bool name_lookup(const char *const *names, size_t count, const char *name, int *out)
{
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (names[i] != NULL && strcmp(names[i], name) == 0) {
            *out = (int) i;
            return true;
        }
    }
    return false;
}

const char *slate_kind_str(slate_kind_t kind)
{
    return kind <= SLATE_KIND_SCENE ? KIND_NAMES[kind] : "?";
}

bool slate_kind_from_str(const char *name, slate_kind_t *out)
{
    int index = 0;
    if (out == NULL || !name_lookup(KIND_NAMES, SLATE_KIND_SCENE + 1, name, &index)) {
        return false;
    }
    *out = (slate_kind_t) index;
    return true;
}

const char *slate_provider_status_str(slate_provider_status_t status)
{
    return status <= SLATE_PROVIDER_ERROR ? STATUS_NAMES[status] : "?";
}

const char *slate_action_str(slate_action_t action)
{
    return (unsigned) action < SLATE_ACTION_COUNT ? ACTION_NAMES[action] : "?";
}

bool slate_action_from_str(const char *name, slate_action_t *out)
{
    int index = 0;
    if (out == NULL || !name_lookup(ACTION_NAMES, SLATE_ACTION_COUNT, name, &index)) {
        return false;
    }
    *out = (slate_action_t) index;
    return true;
}

const char *slate_measurement_str(slate_measurement_t measurement)
{
    return measurement <= SLATE_MEASUREMENT_POWER ? MEASUREMENT_NAMES[measurement] : NULL;
}

bool slate_measurement_from_str(const char *name, slate_measurement_t *out)
{
    int index = 0;
    if (out == NULL ||
        !name_lookup(MEASUREMENT_NAMES, SLATE_MEASUREMENT_POWER + 1, name, &index)) {
        return false;
    }
    *out = (slate_measurement_t) index;
    return true;
}

const char *slate_category_str(slate_category_t category)
{
    return category < SLATE_CATEGORY_COUNT ? CATEGORY_NAMES[category] : NULL;
}

bool slate_category_from_str(const char *name, slate_category_t *out)
{
    int index = 0;
    if (out == NULL || !name_lookup(CATEGORY_NAMES, SLATE_CATEGORY_COUNT, name, &index)) {
        return false;
    }
    *out = (slate_category_t) index;
    return true;
}

slate_category_t slate_category_of_measurement(slate_measurement_t measurement)
{
    /* The four measurements are also four of the categories, and they are
     * spelled the same in §5.2 because they mean the same thing. Written as a
     * switch rather than a table so that a fifth measurement cannot be added
     * without the compiler asking what it is about. */
    switch (measurement) {
    case SLATE_MEASUREMENT_TEMPERATURE:
        return SLATE_CATEGORY_TEMPERATURE;
    case SLATE_MEASUREMENT_HUMIDITY:
        return SLATE_CATEGORY_HUMIDITY;
    case SLATE_MEASUREMENT_PRESSURE:
        return SLATE_CATEGORY_PRESSURE;
    case SLATE_MEASUREMENT_POWER:
        return SLATE_CATEGORY_POWER;
    case SLATE_MEASUREMENT_NONE:
        return SLATE_CATEGORY_NONE;
    }
    return SLATE_CATEGORY_NONE;
}

const char *slate_presentation_str(slate_presentation_t presentation)
{
    return presentation <= SLATE_PRESENT_INCOMPATIBLE ? PRESENTATION_NAMES[presentation] : "?";
}

bool slate_capabilities_have(const slate_capabilities_t *caps, slate_action_t action)
{
    return caps != NULL && (unsigned) action < SLATE_ACTION_COUNT &&
           (caps->actions & (uint16_t) (1u << action)) != 0;
}

/*
 * What each kind is allowed to advertise. §5.2 says "unknown state fields and
 * capabilities are ignored", and this is that rule applied to the one case
 * where ignoring is not the same as tolerating: a light advertising `activate`
 * would let #18 route an action §7.1 has no control for, and the resulting
 * failure would be reported against the tile rather than against the adapter
 * that made it up. The bits are dropped at the boundary, where the adapter can
 * still be blamed.
 */
static uint16_t kind_actions(slate_kind_t kind)
{
    switch (kind) {
    case SLATE_KIND_LIGHT:
        return (uint16_t) ((1u << SLATE_ACTION_TOGGLE) | (1u << SLATE_ACTION_SET_POWER) |
                           (1u << SLATE_ACTION_SET_BRIGHTNESS) |
                           (1u << SLATE_ACTION_SET_COLOR_TEMPERATURE));
    case SLATE_KIND_COVER:
        return (uint16_t) ((1u << SLATE_ACTION_TOGGLE) | (1u << SLATE_ACTION_OPEN) |
                           (1u << SLATE_ACTION_STOP) | (1u << SLATE_ACTION_CLOSE) |
                           (1u << SLATE_ACTION_SET_POSITION));
    case SLATE_KIND_SENSOR:
        return 0; /* §7.3: "No actions." */
    case SLATE_KIND_SCENE:
        return (uint16_t) (1u << SLATE_ACTION_ACTIVATE);
    }
    return 0;
}

/* --- Small helpers ------------------------------------------------------- */

/** Copy a presentation field, truncating. NULL means the resource supplied none. */
static void copy_field(char *dst, size_t size, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t len = strnlen(src, size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/** An identity is present and inside its limit. Never truncated — see the header. */
static bool identity_ok(const char *id, size_t max)
{
    return id != NULL && id[0] != '\0' && strnlen(id, max + 1) <= max;
}

/** A provider whose resources components may believe (§5.2's `degraded` rule). */
static bool status_is_serving(slate_provider_status_t status)
{
    return status == SLATE_PROVIDER_ONLINE || status == SLATE_PROVIDER_DEGRADED;
}

/** Caller holds the lock. */
static provider_t *find_provider(const char *id)
{
    for (size_t i = 0; i < s_provider_count; i++) {
        if (strcmp(s_providers[i].id, id) == 0) {
            return &s_providers[i];
        }
    }
    return NULL;
}

/** Caller holds the lock. */
static entry_t *find_entry(const char *provider, const char *resource)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].provider, provider) == 0 &&
            strcmp(s_entries[i].resource, resource) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

/*
 * §7.5's six cases, decided in one place. The order is the argument:
 *
 * A provider this firmware does not implement comes first, because nothing
 * below it can be true — there is no adapter to have published anything, and
 * §3.3 wants that configuration shown rather than refused.
 *
 * A confirmed value comes next, ahead of the mismatch flag, so a tile that has
 * something true to show keeps showing it while a script publishes the wrong
 * kind over it (§5.4: "None disturbs the last confirmed value"). The
 * incompatible placeholder is for the binding that has nothing else to say.
 *
 * Within a confirmed value, staleness outranks unavailability: a provider that
 * is offline is not in a position to know whether its resource still is, and
 * §7.5 dims both the same way regardless.
 *
 * Caller holds the lock.
 */
static slate_presentation_t presentation_of(const entry_t *entry)
{
    const provider_t *provider = find_provider(entry->provider);
    if (provider == NULL) {
        return SLATE_PRESENT_MISSING_PROVIDER;
    }
    if (entry->updated_us != 0) {
        if (!status_is_serving(provider->status)) {
            return SLATE_PRESENT_STALE;
        }
        return entry->available ? SLATE_PRESENT_OK : SLATE_PRESENT_UNAVAILABLE;
    }
    return entry->mismatch ? SLATE_PRESENT_INCOMPATIBLE : SLATE_PRESENT_MISSING;
}

/** Caller holds the lock. */
static void export_entry(const entry_t *entry, slate_resource_t *out)
{
    memcpy(out->provider, entry->provider, sizeof(out->provider));
    memcpy(out->resource, entry->resource, sizeof(out->resource));
    memcpy(out->name, entry->name, sizeof(out->name));
    memcpy(out->area, entry->area, sizeof(out->area));
    out->kind = entry->kind;
    out->mismatch_kind = entry->mismatch_kind;
    out->presentation = presentation_of(entry);
    out->capabilities = entry->capabilities;
    out->state = entry->state;
    out->updated_us = entry->updated_us;
}

static void wake_observer(void)
{
    LOCK();
    slate_state_wake_fn wake = s_wake;
    void *ctx = s_wake_ctx;
    UNLOCK();

    if (wake != NULL) {
        wake(ctx);
    }
}

/* --- Lifecycle ----------------------------------------------------------- */

esp_err_t slate_state_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "no memory for the store lock");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* --- Providers ----------------------------------------------------------- */

esp_err_t slate_state_provider_register(const slate_state_provider_t *provider)
{
    if (provider == NULL || !identity_ok(provider->id, SLATE_PROVIDER_ID_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    LOCK();
    esp_err_t err = ESP_OK;
    if (find_provider(provider->id) != NULL) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s_provider_count == SLATE_STATE_MAX_PROVIDERS) {
        err = ESP_ERR_NO_MEM;
    } else {
        provider_t *slot = &s_providers[s_provider_count++];
        copy_field(slot->id, sizeof(slot->id), provider->id);
        slot->subscribe = provider->subscribe;
        slot->ctx = provider->ctx;
        slot->status = SLATE_PROVIDER_UNCONFIGURED;
    }
    UNLOCK();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "provider \"%s\" registered", provider->id);
    }
    return err;
}

esp_err_t slate_state_provider_set_status(const char *id, slate_provider_status_t status)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (id == NULL || status > SLATE_PROVIDER_ERROR) {
        return ESP_ERR_INVALID_ARG;
    }

    LOCK();
    provider_t *provider = find_provider(id);
    bool changed = false;
    if (provider != NULL && provider->status != status) {
        bool was_serving = status_is_serving(provider->status);
        provider->status = status;

        /* Only a crossing of the serving boundary changes what is on screen, and
         * only for this provider's resources — §5.2: "an unavailable HA instance
         * must not dim tiles supplied by `direct`." */
        if (was_serving != status_is_serving(status)) {
            for (size_t i = 0; i < s_count; i++) {
                if (strcmp(s_entries[i].provider, id) == 0 && s_entries[i].updated_us != 0) {
                    s_entries[i].changed = true;
                    changed = true;
                }
            }
        }
    }
    esp_err_t err = provider != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
    UNLOCK();

    if (changed) {
        wake_observer();
    }
    return err;
}

slate_provider_status_t slate_state_provider_status(const char *id)
{
    if (id == NULL || s_lock == NULL) {
        return SLATE_PROVIDER_UNCONFIGURED;
    }
    LOCK();
    const provider_t *provider = find_provider(id);
    slate_provider_status_t status =
        provider != NULL ? provider->status : SLATE_PROVIDER_UNCONFIGURED;
    UNLOCK();
    return status;
}

size_t slate_state_provider_count(void)
{
    if (s_lock == NULL) {
        return 0;
    }
    LOCK();
    size_t count = s_provider_count;
    UNLOCK();
    return count;
}

esp_err_t slate_state_provider_at(size_t index, slate_state_provider_info_t *out)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    LOCK();
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (index < s_provider_count) {
        memcpy(out->id, s_providers[index].id, sizeof(out->id));
        out->status = s_providers[index].status;
        out->resource_count = 0;
        for (size_t i = 0; i < s_count; i++) {
            if (strcmp(s_entries[i].provider, out->id) == 0) {
                out->resource_count++;
            }
        }
        err = ESP_OK;
    }
    UNLOCK();
    return err;
}

/* --- Bindings ------------------------------------------------------------ */

/**
 * Hand each registered provider the resource ids it is now responsible for.
 *
 * Outside the lock, and on the binding task, so an adapter may publish from
 * inside its own subscribe. The provider table is copied first for the same
 * reason: a registration arriving mid-rebuild must not be iterated over
 * half-written, and the copy is six pointers.
 *
 * A failure is logged rather than propagated. §6.4 activates the tree and the
 * subscriptions together, and there is no meaning to a rebuild that half
 * happened: the screen already shows the new configuration, and a provider that
 * could not subscribe reports it as a status the tiles present as stale.
 */
static void notify_subscribers(void)
{
    provider_t providers[SLATE_STATE_MAX_PROVIDERS];
    size_t provider_count;

    LOCK();
    provider_count = s_provider_count;
    memcpy(providers, s_providers, sizeof(providers));
    size_t count = s_count;
    UNLOCK();

    const char **ids = NULL;
    if (count > 0) {
        /* A kilobyte at the ceiling, and transient, but it comes out of the same
         * PSRAM the table does for the reason §6.2 gives: internal RAM is the
         * scarce kind, and this allocation happens while the display and the
         * radio are holding what they hold. */
        ids = heap_caps_malloc_prefer(count * sizeof(*ids), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ids == NULL) {
            ESP_LOGE(TAG, "no memory to publish the subscription set; providers keep the old one");
            return;
        }
    }

    for (size_t p = 0; p < provider_count; p++) {
        if (providers[p].subscribe == NULL) {
            continue;
        }

        LOCK();
        size_t used = 0;
        for (size_t i = 0; i < s_count && used < count; i++) {
            if (strcmp(s_entries[i].provider, providers[p].id) == 0) {
                ids[used++] = s_entries[i].resource;
            }
        }
        UNLOCK();

        esp_err_t err = providers[p].subscribe(providers[p].ctx, ids, used);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "provider \"%s\" refused %u resource(s): %s", providers[p].id,
                     (unsigned) used, esp_err_to_name(err));
        }
    }

    free(ids);
}

esp_err_t slate_state_bind(const slate_binding_t *bindings, size_t count)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (count > 0 && bindings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validated before anything is allocated, so a rejected configuration costs
     * the caller nothing and leaves the active one exactly where it was. */
    size_t distinct = 0;
    for (size_t i = 0; i < count; i++) {
        if (!identity_ok(bindings[i].provider, SLATE_PROVIDER_ID_MAX) ||
            !identity_ok(bindings[i].resource, SLATE_RESOURCE_ID_MAX) ||
            bindings[i].kind > SLATE_KIND_SCENE) {
            ESP_LOGE(TAG, "binding %u is not a usable provider/resource pair", (unsigned) i);
            return ESP_ERR_INVALID_ARG;
        }

        /* Several tiles may bind one resource — a 1×1 light beside the same
         * light in a 2×2 — and the store holds one entry for them. Two tiles
         * that disagree about its kind are a different thing: at most one can be
         * right, one entry cannot describe both, and the configuration that says
         * it is the thing to refuse. #19 reports this against the tile.
         *
         * Counting the survivors here rather than while filling is what lets the
         * table be the size it will actually use, and it keeps every refusal
         * ahead of the allocation: a contradictory configuration now costs
         * neither a block nor the free that follows it.
         */
        bool duplicate = false;
        for (size_t j = 0; j < i; j++) {
            if (strcmp(bindings[i].provider, bindings[j].provider) != 0 ||
                strcmp(bindings[i].resource, bindings[j].resource) != 0) {
                continue;
            }
            if (bindings[i].kind != bindings[j].kind) {
                ESP_LOGE(TAG, "%s:%s is bound as both %s and %s", bindings[i].provider,
                         bindings[i].resource, slate_kind_str(bindings[j].kind),
                         slate_kind_str(bindings[i].kind));
                return ESP_ERR_INVALID_STATE;
            }
            duplicate = true;
            break;
        }
        if (!duplicate && ++distinct > SLATE_STATE_MAX_RESOURCES) {
            /* The cap counts what the table will hold, which §5.1 makes the
             * resources the configuration references rather than the bindings
             * that reference them. Comparing the caller's raw count instead
             * refused documents slate_config accepts — it counts distinct pairs
             * too — so a dashboard with several tiles on one light could pass
             * validate and then fail apply. #138.
             *
             * The old check bounded the walk as well, and this does not: a long
             * list naming few pairs never reaches the refusal and is scanned to
             * its end, quadratically, because the duplicate search above reads
             * every earlier binding rather than every earlier pair. What bounds
             * it now is the document: §3.1 stops at 64 KB and a binding costs
             * tens of bytes of JSON, so the ceiling is a few thousand of them
             * and a few million strcmp of a short id, once, on the task that is
             * rebuilding the tree anyway. If that ever shows up in an apply, the
             * fix is a better duplicate search and not a cap on the raw count:
             * a cap on the raw count is #138. */
            ESP_LOGE(TAG, "more than %u distinct resource(s) bound",
                     (unsigned) SLATE_STATE_MAX_RESOURCES);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    entry_t *fresh = NULL;
    if (distinct > 0) {
        /* §6.2 puts the store in PSRAM, where a saturated configuration's ~72 KB
         * is unremarkable; internal RAM is the fallback rather than the choice,
         * on the same reasoning slate_store_config_read() uses. */
        fresh = heap_caps_calloc_prefer(distinct, sizeof(*fresh), 2,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (fresh == NULL) {
            ESP_LOGE(TAG, "no memory for %u bound resource(s)", (unsigned) distinct);
            return ESP_ERR_NO_MEM;
        }
    }

    LOCK();

    size_t used = 0;
    bool carried_change = false;
    /* `used < distinct` cannot bind here — both passes ask the same question of
     * the same list — and it is written anyway, because the alternative if they
     * ever disagreed is a write past the end of the table. */
    for (size_t i = 0; i < count && used < distinct; i++) {
        bool duplicate = false;
        for (size_t j = 0; j < used; j++) {
            if (strcmp(fresh[j].provider, bindings[i].provider) == 0 &&
                strcmp(fresh[j].resource, bindings[i].resource) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        entry_t *entry = &fresh[used++];
        copy_field(entry->provider, sizeof(entry->provider), bindings[i].provider);
        copy_field(entry->resource, sizeof(entry->resource), bindings[i].resource);
        entry->kind = bindings[i].kind;
        entry->mismatch_kind = bindings[i].kind;

        /* Carry the last known value across the rebuild. Same pair and same
         * kind only: a tile whose type changed is asking for a different
         * contract, and yesterday's light state is not an answer to it. */
        const entry_t *previous = find_entry(entry->provider, entry->resource);
        if (previous != NULL && previous->kind == entry->kind) {
            memcpy(entry->name, previous->name, sizeof(entry->name));
            memcpy(entry->area, previous->area, sizeof(entry->area));
            entry->capabilities = previous->capabilities;
            entry->state = previous->state;
            entry->updated_us = previous->updated_us;
            entry->available = previous->available;
            entry->mismatch = previous->mismatch;
            entry->mismatch_kind = previous->mismatch_kind;

            /* An undelivered change survives too, and this is not bookkeeping
             * tidiness. §6.4 builds the replacement tree before activating it,
             * so the tree is drawn from values read a moment before this call; a
             * snapshot landing in that window has already set `changed` on the
             * table being replaced. Dropping the flag here would leave the new
             * tile showing the value the old table held, with nothing to correct
             * it until the resource happens to change again — indefinitely, for
             * a light somebody toggles twice a day. A redundant delivery costs a
             * tile update; a dropped one is wrong until the next one. */
            entry->changed = previous->changed;
            carried_change = carried_change || previous->changed;
        }
    }

    entry_t *old = s_entries;
    s_entries = fresh;
    s_count = used;
    UNLOCK();

    free(old);
    notify_subscribers();
    if (carried_change) {
        wake_observer();
    }
    ESP_LOGI(TAG, "bound %u resource(s) from %u binding(s)", (unsigned) used, (unsigned) count);
    return ESP_OK;
}

size_t slate_state_count(void)
{
    if (s_lock == NULL) {
        return 0;
    }
    LOCK();
    size_t count = s_count;
    UNLOCK();
    return count;
}

/* --- Publication --------------------------------------------------------- */

/**
 * Whether a snapshot's kind-specific half is one the components can render.
 *
 * §5.4's `invalid_state`, and the reason it is checked here rather than in the
 * endpoint that answers 400: an HA mapping bug produces the same nonsense as a
 * malformed `POST /direct/state`, and a percentage of 900 reaching a slider is
 * not a failure either adapter is better placed to catch than this is.
 */
static bool state_is_valid(const slate_snapshot_t *snapshot)
{
    switch (snapshot->kind) {
    case SLATE_KIND_LIGHT: {
        const slate_light_state_t *light = &snapshot->state.light;
        bool brightness_ok = light->brightness == SLATE_STATE_ABSENT ||
                             (light->brightness >= 0 && light->brightness <= 100);
        bool temperature_ok =
            light->color_temperature == SLATE_STATE_ABSENT || light->color_temperature > 0;
        return brightness_ok && temperature_ok;
    }
    case SLATE_KIND_COVER: {
        const slate_cover_state_t *cover = &snapshot->state.cover;
        bool position_ok = cover->position == SLATE_STATE_ABSENT ||
                           (cover->position >= 0 && cover->position <= 100);
        return position_ok && cover->motion <= SLATE_COVER_CLOSING;
    }
    case SLATE_KIND_SENSOR: {
        const slate_sensor_state_t *sensor = &snapshot->state.sensor;
        if (sensor->measurement > SLATE_MEASUREMENT_POWER ||
            sensor->category >= SLATE_CATEGORY_COUNT) {
            return false;
        }
        /* A textual sensor with nothing to say renders as an empty tile, which
         * §7.5 does not have a case for. NaN is the numeric spelling of the
         * same emptiness and formats as "nan" on a dashboard. */
        return sensor->numeric ? isfinite(sensor->value)
                               : strnlen(sensor->text, sizeof(sensor->text)) > 0 &&
                                     strnlen(sensor->text, sizeof(sensor->text)) <
                                         sizeof(sensor->text);
    }
    case SLATE_KIND_SCENE:
        return true; /* §5.2: "`scene`: stateless". */
    }
    return false;
}

/**
 * Drop what §5.2 says to ignore, and settle what an unstated range means.
 *
 * §5.2 spells one capability as `true` and the next as `{"min":0,"max":100}`,
 * so an advertised percentage with no range is the ordinary encoding rather
 * than an adapter's oversight, and every component would otherwise invent the
 * same fallback separately. A percentage's whole range is the obvious one and
 * it is filled in here, once. Colour temperature has no such natural scale, so
 * an unstated range stays unstated and §7.1's control decides what to do with
 * a capability whose limits it was not given.
 */
static slate_capabilities_t normalize_capabilities(const slate_snapshot_t *snapshot)
{
    slate_capabilities_t caps = snapshot->capabilities;
    caps.actions &= kind_actions(snapshot->kind);

    if (slate_capabilities_have(&caps, SLATE_ACTION_SET_BRIGHTNESS)) {
        if (caps.brightness_min == 0 && caps.brightness_max == 0) {
            caps.brightness_max = 100;
        }
    } else {
        caps.brightness_min = 0;
        caps.brightness_max = 0;
    }
    if (slate_capabilities_have(&caps, SLATE_ACTION_SET_POSITION)) {
        if (caps.position_min == 0 && caps.position_max == 0) {
            caps.position_max = 100;
        }
    } else {
        caps.position_min = 0;
        caps.position_max = 0;
    }
    if (!slate_capabilities_have(&caps, SLATE_ACTION_SET_COLOR_TEMPERATURE)) {
        caps.color_temperature_min = 0;
        caps.color_temperature_max = 0;
    }
    return caps;
}

/**
 * Whether the advertised ranges describe a control that can be drawn.
 *
 * Checked after masking, so a range belonging to a capability the kind cannot
 * have is not held against a snapshot that is otherwise fine. An inverted or
 * out-of-scale range is malformed rather than unknown, and §5.2's "unknown
 * capabilities are ignored" is not a licence to hand #18 a `set_brightness`
 * whose maximum is below its minimum — that is an action bus that refuses
 * every value a slider can produce, reported against the tile rather than
 * against the adapter that made it up.
 *
 * The percentage capabilities are held to §5.2's own scale, because a state
 * value outside 0..100 is refused a few lines above and a capability that
 * promised a wider one would be promising an action whose result cannot be
 * published back.
 */
static bool ranges_are_valid(const slate_capabilities_t *caps)
{
    if (slate_capabilities_have(caps, SLATE_ACTION_SET_BRIGHTNESS) &&
        (caps->brightness_min < 0 || caps->brightness_max > 100 ||
         caps->brightness_min > caps->brightness_max)) {
        return false;
    }
    if (slate_capabilities_have(caps, SLATE_ACTION_SET_POSITION) &&
        (caps->position_min < 0 || caps->position_max > 100 ||
         caps->position_min > caps->position_max)) {
        return false;
    }
    if (slate_capabilities_have(caps, SLATE_ACTION_SET_COLOR_TEMPERATURE) &&
        !(caps->color_temperature_min == 0 && caps->color_temperature_max == 0) &&
        (caps->color_temperature_min <= 0 ||
         caps->color_temperature_min > caps->color_temperature_max)) {
        return false;
    }
    return true;
}

esp_err_t slate_state_publish(const char *provider, const slate_snapshot_t *snapshot)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (provider == NULL || snapshot == NULL || !identity_ok(snapshot->resource, SLATE_RESOURCE_ID_MAX) ||
        snapshot->kind > SLATE_KIND_SCENE || !state_is_valid(snapshot)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Before the lock, because it depends on nothing the store holds. */
    const slate_capabilities_t capabilities = normalize_capabilities(snapshot);
    if (!ranges_are_valid(&capabilities)) {
        return ESP_ERR_INVALID_ARG;
    }

    LOCK();
    esp_err_t err = ESP_OK;
    entry_t *entry = find_entry(provider, snapshot->resource);
    /* Recorded inside the lock: a rebuild on the binding task may free the table
     * the moment this one releases it, and a pointer tested afterwards would be
     * asking a question about memory that is no longer ours. */
    bool touched = entry != NULL;
    slate_state_publish_observer_fn observer = NULL;
    void *observer_ctx = NULL;
    if (entry == NULL) {
        /* §5.4: publishing an id the configuration does not reference is what
         * `404 resource_not_bound` refuses, and it is the store's memory bound
         * rather than a permission check. */
        err = ESP_ERR_NOT_FOUND;
    } else if (entry->kind != snapshot->kind) {
        entry->mismatch = true;
        entry->mismatch_kind = snapshot->kind;
        entry->changed = true;
        err = ESP_ERR_INVALID_STATE;
    } else {
        copy_field(entry->name, sizeof(entry->name), snapshot->name);
        copy_field(entry->area, sizeof(entry->area), snapshot->area);
        entry->capabilities = capabilities;
        entry->available = snapshot->available;
        entry->mismatch = false;
        entry->mismatch_kind = entry->kind;
        entry->changed = true;
        entry->updated_us = esp_timer_get_time();

        /* The whole union, so a kind's unused members cannot carry a previous
         * snapshot's bytes into a component that reads them by accident. */
        memset(&entry->state, 0, sizeof(entry->state));
        switch (snapshot->kind) {
        case SLATE_KIND_LIGHT:
            entry->state.light = snapshot->state.light;
            break;
        case SLATE_KIND_COVER:
            entry->state.cover = snapshot->state.cover;
            break;
        case SLATE_KIND_SENSOR:
            entry->state.sensor = snapshot->state.sensor;
            entry->state.sensor.text[sizeof(entry->state.sensor.text) - 1] = '\0';
            entry->state.sensor.unit[sizeof(entry->state.sensor.unit) - 1] = '\0';
            /* §5.2: a measurement implies the matching category. Filled in here
             * for the same reason an advertised percentage gets 0..100 — every
             * reader would otherwise write this line for itself, and §7.3's
             * icon would depend on which of the two fields a provider happened
             * to set. A category the provider stated is never overwritten: it
             * is the more specific of the two. */
            if (entry->state.sensor.category == SLATE_CATEGORY_NONE) {
                entry->state.sensor.category =
                    slate_category_of_measurement(entry->state.sensor.measurement);
            }
            break;
        case SLATE_KIND_SCENE:
            break;
        }
        observer = s_publish_observer;
        observer_ctx = s_publish_observer_ctx;
    }
    UNLOCK();

    if (err == ESP_OK && observer != NULL) {
        observer(observer_ctx, provider, snapshot);
    }
    if (touched) {
        wake_observer();
    }
    return err;
}

void slate_state_set_publish_observer(slate_state_publish_observer_fn observer, void *ctx)
{
    if (s_lock == NULL) {
        return;
    }
    LOCK();
    s_publish_observer = observer;
    s_publish_observer_ctx = ctx;
    UNLOCK();
}

/* --- Reading ------------------------------------------------------------- */

esp_err_t slate_state_get(const char *provider, const char *resource, slate_resource_t *out)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (provider == NULL || resource == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    LOCK();
    const entry_t *entry = find_entry(provider, resource);
    if (entry != NULL) {
        export_entry(entry, out);
    }
    UNLOCK();
    return entry != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t slate_state_at(size_t index, slate_resource_t *out)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    LOCK();
    bool found = index < s_count;
    if (found) {
        export_entry(&s_entries[index], out);
    }
    UNLOCK();
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* --- Observation --------------------------------------------------------- */

void slate_state_set_wake(slate_state_wake_fn wake, void *ctx)
{
    if (s_lock == NULL) {
        return;
    }
    LOCK();
    s_wake = wake;
    s_wake_ctx = ctx;
    UNLOCK();
}

size_t slate_state_drain(slate_state_visit_fn fn, void *ctx)
{
    if (s_lock == NULL) {
        return 0;
    }

    size_t delivered = 0;
    for (size_t i = 0;; i++) {
        slate_resource_t resource;

        LOCK();
        if (i >= s_count) {
            UNLOCK();
            break;
        }
        bool changed = s_entries[i].changed;
        if (changed) {
            s_entries[i].changed = false;
            export_entry(&s_entries[i], &resource);
        }
        UNLOCK();

        if (!changed) {
            continue;
        }
        if (fn != NULL) {
            fn(&resource, ctx);
        }
        delivered++;
    }
    return delivered;
}

/* --- Self-test ------------------------------------------------------------ */

#ifdef SLATE_STATE_SELFTEST

static int s_checks;
static int s_failures;

/*
 * The count is reported as well as the failures, and that is not decoration.
 * §11.3's retained ring is 8 KB and this runs before the WebSocket server
 * exists, so the first checks are evicted by the last ones and read as though
 * they never ran. A total nobody can see all of is still a total that changes
 * when a check stops executing.
 */
#define CHECK(cond, what)                                     \
    do {                                                      \
        bool ok_ = (cond);                                    \
        s_checks++;                                           \
        if (!ok_) {                                           \
            s_failures++;                                     \
        }                                                     \
        ESP_LOGI(TAG, "selftest: %-46s %s", what,             \
                 ok_ ? "PASS" : "FAIL");                      \
    } while (0)

/* A provider that records what it was subscribed to and nothing else. §5.1's
 * second core operation is the only part of the boundary the store can exercise
 * without a transport, and this is the whole of that side of it. */
typedef struct {
    size_t calls;
    size_t count;
} fixture_t;

static esp_err_t fixture_subscribe(void *ctx, const char *const *resources, size_t count)
{
    fixture_t *fixture = ctx;
    fixture->calls++;
    fixture->count = count;
    for (size_t i = 0; i < count; i++) {
        if (resources[i] == NULL || resources[i][0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static size_t s_wakes;

static void fixture_wake(void *ctx)
{
    (void) ctx;
    s_wakes++;
}

typedef struct {
    size_t seen;
    slate_resource_t last;
} visit_t;

static void fixture_visit(const slate_resource_t *resource, void *ctx)
{
    visit_t *visit = ctx;
    visit->seen++;
    visit->last = *resource;
}

esp_err_t slate_state_selftest(void)
{
    static fixture_t alpha;
    static fixture_t beta;
    static slate_resource_t r;
    static visit_t visit;

    const slate_state_provider_t alpha_registration = {
        .id = "st-alpha", .subscribe = fixture_subscribe, .ctx = &alpha};
    const slate_state_provider_t beta_registration = {
        .id = "st-beta", .subscribe = fixture_subscribe, .ctx = &beta};
    const slate_state_provider_t over_long = {.id = "0123456789abcdefg"};

    /* One pair bound twice, three kinds, and a provider this build does not
     * have — §3.3's forward-compatibility case belongs in the ordinary set
     * rather than in a check of its own. */
    const slate_binding_t set[] = {
        {"st-alpha", "lamp", SLATE_KIND_LIGHT},
        {"st-alpha", "lamp", SLATE_KIND_LIGHT},
        {"st-alpha", "blind", SLATE_KIND_COVER},
        {"st-beta", "temp", SLATE_KIND_SENSOR},
        {"st-nowhere", "thing", SLATE_KIND_SCENE},
    };
    const size_t SET_LEN = sizeof(set) / sizeof(set[0]);

    slate_snapshot_t lamp = {
        .resource = "lamp",
        .kind = SLATE_KIND_LIGHT,
        .name = "Reading lamp",
        .area = "Study",
        .available = true,
        .capabilities = {.actions = (1u << SLATE_ACTION_TOGGLE) |
                                    (1u << SLATE_ACTION_SET_BRIGHTNESS) |
                                    (1u << SLATE_ACTION_ACTIVATE),
                         .brightness_min = 0,
                         .brightness_max = 100},
        .state.light = {.on = true, .brightness = 62, .color_temperature = SLATE_STATE_ABSENT},
    };
    const slate_snapshot_t temp = {
        .resource = "temp",
        .kind = SLATE_KIND_SENSOR,
        .name = "Study temperature",
        .available = true,
        .state.sensor = {.numeric = true,
                         .value = 21.4,
                         .unit = "°C",
                         .measurement = SLATE_MEASUREMENT_TEMPERATURE},
    };

    CHECK(slate_state_init() == ESP_OK, "init is idempotent");

    CHECK(slate_state_provider_register(&alpha_registration) == ESP_OK, "register a provider");
    CHECK(slate_state_provider_register(&beta_registration) == ESP_OK, "register a second one");
    CHECK(slate_state_provider_register(&alpha_registration) == ESP_ERR_INVALID_STATE,
          "the same id twice is refused");
    CHECK(slate_state_provider_register(&over_long) == ESP_ERR_INVALID_ARG,
          "an over-long provider id is refused");

    slate_state_set_wake(fixture_wake, NULL);

    CHECK(slate_state_bind(set, SET_LEN) == ESP_OK, "bind a mixed set");
    CHECK(slate_state_count() == 4, "one pair bound twice is one entry");
    CHECK(alpha.count == 2 && beta.count == 1, "each provider is handed its own subset");

    const slate_binding_t contradiction[] = {
        {"st-alpha", "lamp", SLATE_KIND_LIGHT},
        {"st-alpha", "lamp", SLATE_KIND_SENSOR},
    };
    CHECK(slate_state_bind(contradiction, 2) == ESP_ERR_INVALID_STATE,
          "one pair with two kinds is refused");
    const slate_binding_t empty_id[] = {{"st-alpha", "", SLATE_KIND_LIGHT}};
    CHECK(slate_state_bind(empty_id, 1) == ESP_ERR_INVALID_ARG, "an empty resource id is refused");
    CHECK(slate_state_count() == 4, "a refused rebuild leaves the active set alone");

    CHECK(slate_state_get("st-alpha", "lamp", &r) == ESP_OK && r.presentation == SLATE_PRESENT_MISSING,
          "a binding with no snapshot presents missing");
    CHECK(slate_state_get("st-nowhere", "thing", &r) == ESP_OK &&
              r.presentation == SLATE_PRESENT_MISSING_PROVIDER,
          "a provider this build lacks presents missing_provider");
    CHECK(slate_state_get("st-alpha", "absent", &r) == ESP_ERR_NOT_FOUND,
          "an unbound pair is not readable");

    CHECK(slate_state_provider_set_status("st-alpha", SLATE_PROVIDER_ONLINE) == ESP_OK,
          "a provider comes online");
    CHECK(slate_state_provider_set_status("st-beta", SLATE_PROVIDER_DEGRADED) == ESP_OK,
          "a provider reports degraded");
    CHECK(slate_state_provider_set_status("st-nowhere", SLATE_PROVIDER_ONLINE) == ESP_ERR_NOT_FOUND,
          "a status for an unregistered provider is refused");

    s_wakes = 0;
    CHECK(slate_state_publish("st-alpha", &lamp) == ESP_OK, "publish a light");
    CHECK(s_wakes == 1, "a publish wakes the observer");
    CHECK(slate_state_get("st-alpha", "lamp", &r) == ESP_OK && r.presentation == SLATE_PRESENT_OK &&
              r.state.light.on && r.state.light.brightness == 62 &&
              r.state.light.color_temperature == SLATE_STATE_ABSENT &&
              strcmp(r.name, "Reading lamp") == 0 && strcmp(r.area, "Study") == 0,
          "the snapshot is what comes back out");
    CHECK(slate_capabilities_have(&r.capabilities, SLATE_ACTION_TOGGLE) &&
              !slate_capabilities_have(&r.capabilities, SLATE_ACTION_ACTIVATE),
          "a capability the kind cannot have is dropped");

    slate_snapshot_t unbound = lamp;
    unbound.resource = "nowhere";
    CHECK(slate_state_publish("st-alpha", &unbound) == ESP_ERR_NOT_FOUND,
          "publishing an unbound resource is refused");
    CHECK(slate_state_publish("st-beta", &lamp) == ESP_ERR_NOT_FOUND,
          "one provider cannot write into another's resource");

    slate_snapshot_t wrong_kind = temp;
    wrong_kind.resource = "lamp";
    CHECK(slate_state_publish("st-alpha", &wrong_kind) == ESP_ERR_INVALID_STATE,
          "a snapshot of the wrong kind is refused");
    slate_snapshot_t nonsense = lamp;
    nonsense.state.light.brightness = 900;
    CHECK(slate_state_publish("st-alpha", &nonsense) == ESP_ERR_INVALID_ARG,
          "an out-of-range percentage is refused");
    slate_snapshot_t wordless = temp;
    wordless.state.sensor.numeric = false;
    wordless.state.sensor.text[0] = '\0';
    CHECK(slate_state_publish("st-beta", &wordless) == ESP_ERR_INVALID_ARG,
          "a textual sensor with no text is refused");
    slate_snapshot_t inverted = lamp;
    inverted.capabilities.brightness_min = 100;
    inverted.capabilities.brightness_max = 0;
    CHECK(slate_state_publish("st-alpha", &inverted) == ESP_ERR_INVALID_ARG,
          "an inverted capability range is refused");
    CHECK(slate_state_get("st-alpha", "lamp", &r) == ESP_OK && r.presentation == SLATE_PRESENT_OK &&
              r.state.light.brightness == 62,
          "no refusal disturbs the last confirmed value");

    slate_snapshot_t blind_as_light = lamp;
    blind_as_light.resource = "blind";
    CHECK(slate_state_publish("st-alpha", &blind_as_light) == ESP_ERR_INVALID_STATE,
          "a cover binding refuses a light");
    CHECK(slate_state_get("st-alpha", "blind", &r) == ESP_OK &&
              r.presentation == SLATE_PRESENT_INCOMPATIBLE &&
              r.kind == SLATE_KIND_COVER && r.mismatch_kind == SLATE_KIND_LIGHT,
          "an incompatible resource reports expected and received kinds");
    CHECK(slate_state_bind(set, SET_LEN) == ESP_OK &&
              slate_state_get("st-alpha", "blind", &r) == ESP_OK &&
              r.presentation == SLATE_PRESENT_INCOMPATIBLE &&
              r.mismatch_kind == SLATE_KIND_LIGHT,
          "an incompatible diagnostic survives a layout rebuild");
    const slate_snapshot_t blind = {
        .resource = "blind",
        .kind = SLATE_KIND_COVER,
        .available = true,
        .state.cover = {.position = 50, .motion = SLATE_COVER_IDLE},
    };
    CHECK(slate_state_publish("st-alpha", &blind) == ESP_OK &&
              slate_state_get("st-alpha", "blind", &r) == ESP_OK &&
              r.presentation == SLATE_PRESENT_OK &&
              r.mismatch_kind == SLATE_KIND_COVER,
          "a valid snapshot clears the incompatible diagnostic");

    CHECK(slate_state_publish("st-beta", &temp) == ESP_OK, "publish a sensor");
    CHECK(slate_state_get("st-beta", "temp", &r) == ESP_OK && r.presentation == SLATE_PRESENT_OK &&
              r.state.sensor.numeric && r.state.sensor.value == 21.4 &&
              strcmp(r.state.sensor.unit, "°C") == 0,
          "a degraded provider's resources stay fresh");
    CHECK(slate_state_get("st-beta", "temp", &r) == ESP_OK &&
              r.state.sensor.category == SLATE_CATEGORY_TEMPERATURE,
          "a stated measurement fills in the category it implies");

    /* The `binary_sensor` shape of §5.6: a word, no magnitude, and a category
     * that is the only thing §7.3 can choose an icon from. */
    slate_snapshot_t contact = temp;
    contact.state.sensor = (slate_sensor_state_t) {.category = SLATE_CATEGORY_DOOR};
    strlcpy(contact.state.sensor.text, "Open", sizeof(contact.state.sensor.text));
    CHECK(slate_state_publish("st-beta", &contact) == ESP_OK &&
              slate_state_get("st-beta", "temp", &r) == ESP_OK &&
              r.state.sensor.measurement == SLATE_MEASUREMENT_NONE &&
              r.state.sensor.category == SLATE_CATEGORY_DOOR,
          "a textual sensor carries a category without a measurement");

    slate_snapshot_t both = temp;
    both.state.sensor.category = SLATE_CATEGORY_BATTERY;
    CHECK(slate_state_publish("st-beta", &both) == ESP_OK &&
              slate_state_get("st-beta", "temp", &r) == ESP_OK &&
              r.state.sensor.category == SLATE_CATEGORY_BATTERY,
          "a stated category is not overwritten by the measurement's");

    slate_snapshot_t bad_category = temp;
    bad_category.state.sensor.category = (slate_category_t) SLATE_CATEGORY_COUNT;
    CHECK(slate_state_publish("st-beta", &bad_category) == ESP_ERR_INVALID_ARG &&
              slate_state_get("st-beta", "temp", &r) == ESP_OK &&
              r.state.sensor.category == SLATE_CATEGORY_BATTERY,
          "a category outside the vocabulary is refused without losing the value");
    CHECK(slate_state_publish("st-beta", &temp) == ESP_OK, "the sensor returns to its reading");

    slate_snapshot_t lamp_gone = lamp;
    lamp_gone.available = false;
    CHECK(slate_state_publish("st-alpha", &lamp_gone) == ESP_OK, "publish an unavailable light");
    CHECK(slate_state_get("st-alpha", "lamp", &r) == ESP_OK &&
              r.presentation == SLATE_PRESENT_UNAVAILABLE && r.state.light.brightness == 62,
          "unavailable keeps its last known values");
    CHECK(slate_state_publish("st-alpha", &lamp) == ESP_OK, "the light comes back");

    CHECK(slate_state_provider_set_status("st-alpha", SLATE_PROVIDER_OFFLINE) == ESP_OK,
          "a provider goes offline");
    CHECK(slate_state_get("st-alpha", "lamp", &r) == ESP_OK && r.presentation == SLATE_PRESENT_STALE &&
              r.state.light.brightness == 62,
          "offline stales its own resources without losing them");
    CHECK(slate_state_get("st-beta", "temp", &r) == ESP_OK && r.presentation == SLATE_PRESENT_OK,
          "one provider offline does not stale another's tiles");
    CHECK(slate_state_provider_set_status("st-alpha", SLATE_PROVIDER_ONLINE) == ESP_OK &&
              slate_state_get("st-alpha", "lamp", &r) == ESP_OK && r.presentation == SLATE_PRESENT_OK,
          "reconnecting restores freshness with no new snapshot");

    slate_state_drain(NULL, NULL);
    visit = (visit_t){0};
    CHECK(slate_state_drain(fixture_visit, &visit) == 0, "a drained store delivers nothing");
    slate_snapshot_t dimmer = lamp;
    dimmer.state.light.brightness = 40;
    CHECK(slate_state_publish("st-alpha", &lamp) == ESP_OK &&
              slate_state_publish("st-alpha", &dimmer) == ESP_OK,
          "publish twice between drains");
    CHECK(slate_state_drain(fixture_visit, &visit) == 1 && visit.seen == 1 &&
              visit.last.state.light.brightness == 40,
          "two updates coalesce into one current value");

    CHECK(slate_state_bind(set, SET_LEN) == ESP_OK, "rebuild the same set");
    CHECK(slate_state_get("st-alpha", "lamp", &r) == ESP_OK && r.presentation == SLATE_PRESENT_OK &&
              r.state.light.brightness == 40,
          "a surviving binding keeps its value across a rebuild");

    slate_snapshot_t unstated = lamp;
    unstated.capabilities.brightness_min = 0;
    unstated.capabilities.brightness_max = 0;
    CHECK(slate_state_publish("st-alpha", &unstated) == ESP_OK &&
              slate_state_get("st-alpha", "lamp", &r) == ESP_OK &&
              r.capabilities.brightness_min == 0 && r.capabilities.brightness_max == 100,
          "an unstated percentage range becomes the whole one");

    /* §6.4 draws the replacement tree from values read before the rebuild, so a
     * snapshot landing in that window is one the new tree has not seen. It has
     * to survive the swap, or the tile keeps the old table's value until the
     * resource happens to move again. */
    slate_snapshot_t late = lamp;
    late.state.light.brightness = 7;
    slate_state_drain(NULL, NULL);
    s_wakes = 0;
    CHECK(slate_state_publish("st-alpha", &late) == ESP_OK &&
              slate_state_bind(set, SET_LEN) == ESP_OK,
          "publish, then rebuild before draining");
    visit = (visit_t){0};
    CHECK(slate_state_drain(fixture_visit, &visit) == 1 && visit.last.state.light.brightness == 7,
          "an undelivered change survives the rebuild");
    CHECK(s_wakes == 2, "the rebuild wakes the observer for what it carried");

    slate_state_provider_info_t info;
    size_t found = 0;
    for (size_t i = 0; i < slate_state_provider_count(); i++) {
        if (slate_state_provider_at(i, &info) == ESP_OK && strcmp(info.id, "st-alpha") == 0) {
            found = info.resource_count;
        }
    }
    CHECK(found == 2, "a provider reports the resources bound to it");
    CHECK(slate_state_provider_at(slate_state_provider_count(), &info) == ESP_ERR_NOT_FOUND,
          "past the last provider reports not found");

    const slate_binding_t retyped[] = {{"st-alpha", "lamp", SLATE_KIND_SENSOR}};
    CHECK(slate_state_bind(retyped, 1) == ESP_OK, "rebind the same pair as another kind");
    CHECK(slate_state_count() == 1 && slate_state_get("st-beta", "temp", &r) == ESP_ERR_NOT_FOUND,
          "a rebuild drops what it no longer binds");
    CHECK(slate_state_get("st-alpha", "lamp", &r) == ESP_OK && r.presentation == SLATE_PRESENT_MISSING,
          "a binding whose kind changed does not keep its value");

    CHECK(slate_state_bind(NULL, 0) == ESP_OK, "release the whole set");
    CHECK(slate_state_count() == 0 && alpha.count == 0 && beta.count == 0,
          "removing the last binding unsubscribes every provider");
    CHECK(slate_state_publish("st-alpha", &lamp) == ESP_ERR_NOT_FOUND,
          "an empty store accepts nothing");

    /*
     * A collapsed duplicate must not be paid for. The set above holds five
     * bindings and four pairs; binding the four on their own has to cost the
     * same, which is a claim about the allocation rather than about
     * slate_state_count() and so is read off the heap. The store is empty here,
     * so both measurements start from the same place.
     */
    const slate_binding_t without_duplicate[] = {
        {"st-alpha", "lamp", SLATE_KIND_LIGHT},
        {"st-alpha", "blind", SLATE_KIND_COVER},
        {"st-beta", "temp", SLATE_KIND_SENSOR},
        {"st-nowhere", "thing", SLATE_KIND_SCENE},
    };
    size_t before_collapsed = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    slate_state_bind(set, SET_LEN);
    size_t collapsed_cost = before_collapsed - heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    slate_state_bind(NULL, 0);
    size_t before_plain = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    slate_state_bind(without_duplicate, 4);
    size_t plain_cost = before_plain - heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    slate_state_bind(NULL, 0);
    CHECK(collapsed_cost == plain_cost && plain_cost >= 4 * sizeof(entry_t),
          "a collapsed duplicate is not allocated for");

    /*
     * §4.1's promise about validate is that it predicts what the write will do,
     * and slate_config counts distinct pairs. So the edge that matters is a
     * saturated document with more bindings than pairs — twelve bar items and a
     * page of tiles all naming resources something else already binds — which
     * has to be accepted here or it would be accepted by validate and refused by
     * apply. The 257th *pair* is the refusal; the 257th binding is not. #138.
     */
    enum { OVER = 8 };
    static char saturated_ids[SLATE_STATE_MAX_RESOURCES][8];
    slate_binding_t *saturated =
        calloc(SLATE_STATE_MAX_RESOURCES + OVER, sizeof(*saturated));
    if (saturated == NULL) {
        CHECK(false, "memory for the saturation fixture");
    } else {
        for (size_t i = 0; i < SLATE_STATE_MAX_RESOURCES; i++) {
            snprintf(saturated_ids[i], sizeof(saturated_ids[i]), "r%u", (unsigned) i);
            saturated[i] = (slate_binding_t) {"st-alpha", saturated_ids[i], SLATE_KIND_LIGHT};
        }
        for (size_t i = 0; i < OVER; i++) {
            saturated[SLATE_STATE_MAX_RESOURCES + i] = saturated[i];
        }
        CHECK(slate_state_bind(saturated, SLATE_STATE_MAX_RESOURCES + OVER) == ESP_OK,
              "a saturated set with more bindings than pairs binds");
        CHECK(slate_state_count() == SLATE_STATE_MAX_RESOURCES,
              "and holds one entry per pair, not per binding");

        /* One pair past the cap, reached without touching the length: a
         * duplicate becomes a pair of its own, so the only thing that differs
         * from the set just accepted is the number of distinct pairs. */
        saturated[SLATE_STATE_MAX_RESOURCES] =
            (slate_binding_t) {"st-alpha", "over", SLATE_KIND_LIGHT};
        CHECK(slate_state_bind(saturated, SLATE_STATE_MAX_RESOURCES + OVER) ==
                  ESP_ERR_INVALID_SIZE,
              "one pair past the cap is refused");
        CHECK(slate_state_count() == SLATE_STATE_MAX_RESOURCES,
              "and the saturated set is still the active one");
        free(saturated);
    }
    slate_state_bind(NULL, 0);

    /*
     * The allocation half of "rebuilding the same configuration does not leak
     * store entries or subscriptions". S-1's method note applies here as much as
     * it does to the LVGL tree: the sets alternate rather than repeat, because a
     * rebuild of an identical set can be handed back the same block by the
     * allocator and hide a leak behind reuse. The LVGL-side 500-cycle criterion
     * remains #20's.
     */
    const slate_binding_t alternate[] = {
        {"st-beta", "temp", SLATE_KIND_SENSOR},
        {"st-alpha", "blind", SLATE_KIND_COVER},
    };
    const size_t CYCLES = 100;

    /* Three hundred rebuild lines are three hundred lines of §11.3's 8 KB
     * retained ring, which is where the checks above are read from. The
     * measurement is what matters here, not the commentary. */
    esp_log_level_set(TAG, ESP_LOG_WARN);
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    bool cycles_ok = true;
    for (size_t i = 0; i < CYCLES; i++) {
        cycles_ok = cycles_ok && slate_state_bind(set, SET_LEN) == ESP_OK &&
                    slate_state_publish("st-alpha", &lamp) == ESP_OK &&
                    slate_state_bind(alternate, 2) == ESP_OK && slate_state_bind(NULL, 0) == ESP_OK;
    }
    size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    CHECK(cycles_ok, "100 rebuild cycles complete");
    CHECK(internal_after == internal_before && psram_after == psram_before,
          "100 rebuild cycles return the heap to its start");
    ESP_LOGI(TAG, "selftest: over %u cycles internal %u -> %u B, psram %u -> %u B",
             (unsigned) CYCLES, (unsigned) internal_before, (unsigned) internal_after,
             (unsigned) psram_before, (unsigned) psram_after);
    ESP_LOGI(TAG, "selftest: one entry is %u B, a full table %u B", (unsigned) sizeof(entry_t),
             (unsigned) (sizeof(entry_t) * SLATE_STATE_MAX_RESOURCES));

    slate_state_set_wake(NULL, NULL);
    ESP_LOGI(TAG, "selftest: %d check(s), %d failure(s)", s_checks, s_failures);
    return s_failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif
