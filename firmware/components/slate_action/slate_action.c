/*
 * Slate — provider-neutral semantic action bus.
 *
 * The confirmed value remains in slate_state. This component owns only the
 * short-lived optimistic overlay and its correlation with adapter results and
 * real snapshots. That split makes revert cheap and exact: dropping an overlay
 * reveals the last confirmed value without copying it back through a provider
 * API or pretending an optimistic value was ever a snapshot.
 */

#include "slate_action.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "slate_action";

#define ACTION_TIMEOUT_US (3 * 1000 * 1000LL)
#define ERROR_VISIBLE_US  (1500 * 1000LL)
#define TIMER_PERIOD_US   (100 * 1000ULL)

typedef struct {
    char id[SLATE_PROVIDER_ID_MAX + 1];
    slate_action_dispatch_fn dispatch;
    void *ctx;
} provider_t;

typedef struct pending {
    struct pending *next;
    char provider[SLATE_PROVIDER_ID_MAX + 1];
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    slate_kind_t kind;
    slate_action_t action;
    slate_state_value_t optimistic;
    uint32_t id;
    int64_t deadline_us;
    slate_action_phase_t phase;
} pending_t;

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_timer;
static provider_t s_providers[SLATE_STATE_MAX_PROVIDERS];
static size_t s_provider_count;
static pending_t *s_pending;
static uint32_t s_next_id;
static slate_action_wake_fn s_wake;
static void *s_wake_ctx;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static bool identity_ok(const char *id, size_t max)
{
    return id != NULL && id[0] != '\0' && strnlen(id, max + 1) <= max;
}

static provider_t *find_provider(const char *id)
{
    for (size_t i = 0; i < s_provider_count; i++) {
        if (strcmp(s_providers[i].id, id) == 0) {
            return &s_providers[i];
        }
    }
    return NULL;
}

static pending_t *find_pair(const char *provider, const char *resource)
{
    for (pending_t *item = s_pending; item != NULL; item = item->next) {
        if (strcmp(item->provider, provider) == 0 && strcmp(item->resource, resource) == 0) {
            return item;
        }
    }
    return NULL;
}

static pending_t *find_id(const char *provider, uint32_t id)
{
    for (pending_t *item = s_pending; item != NULL; item = item->next) {
        if (item->id == id && strcmp(item->provider, provider) == 0) {
            return item;
        }
    }
    return NULL;
}

static void remove_locked(pending_t *target)
{
    pending_t **link = &s_pending;
    while (*link != NULL) {
        if (*link == target) {
            *link = target->next;
            free(target);
            return;
        }
        link = &(*link)->next;
    }
}

static pending_t *allocate_pending(void)
{
    pending_t *item = heap_caps_calloc(1, sizeof(*item), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return item != NULL ? item : calloc(1, sizeof(*item));
}

static void wake_observer(void)
{
    slate_action_wake_fn wake;
    void *ctx;

    LOCK();
    wake = s_wake;
    ctx = s_wake_ctx;
    UNLOCK();
    if (wake != NULL) {
        wake(ctx);
    }
}

static bool no_value(const slate_action_request_t *request)
{
    return request->value_type == SLATE_ACTION_VALUE_NONE;
}

static esp_err_t expected_state(const slate_resource_t *resource,
                                const slate_action_request_t *request,
                                slate_state_value_t *out)
{
    *out = resource->state;

    switch (request->action) {
    case SLATE_ACTION_TOGGLE:
        if (!no_value(request)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (resource->kind == SLATE_KIND_LIGHT) {
            out->light.on = !out->light.on;
        } else if (resource->kind == SLATE_KIND_COVER) {
            if (out->cover.motion != SLATE_COVER_IDLE) {
                out->cover.motion = SLATE_COVER_IDLE;
            } else {
                out->cover.motion = out->cover.position > 0 ? SLATE_COVER_CLOSING
                                                           : SLATE_COVER_OPENING;
            }
        } else {
            return ESP_ERR_NOT_SUPPORTED;
        }
        return ESP_OK;

    case SLATE_ACTION_SET_POWER:
        if (resource->kind != SLATE_KIND_LIGHT ||
            request->value_type != SLATE_ACTION_VALUE_BOOL) {
            return ESP_ERR_INVALID_ARG;
        }
        out->light.on = request->value.boolean;
        return ESP_OK;

    case SLATE_ACTION_SET_BRIGHTNESS:
        if (resource->kind != SLATE_KIND_LIGHT ||
            request->value_type != SLATE_ACTION_VALUE_NUMBER) {
            return ESP_ERR_INVALID_ARG;
        }
        if (request->value.number < resource->capabilities.brightness_min ||
            request->value.number > resource->capabilities.brightness_max) {
            return ESP_ERR_INVALID_SIZE;
        }
        out->light.brightness = (int16_t) request->value.number;
        return ESP_OK;

    case SLATE_ACTION_SET_COLOR_TEMPERATURE:
        if (resource->kind != SLATE_KIND_LIGHT ||
            request->value_type != SLATE_ACTION_VALUE_NUMBER) {
            return ESP_ERR_INVALID_ARG;
        }
        if ((resource->capabilities.color_temperature_min != 0 ||
             resource->capabilities.color_temperature_max != 0) &&
            (request->value.number < resource->capabilities.color_temperature_min ||
             request->value.number > resource->capabilities.color_temperature_max)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (request->value.number <= 0 || request->value.number > INT16_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        out->light.color_temperature = (int16_t) request->value.number;
        return ESP_OK;

    case SLATE_ACTION_OPEN:
    case SLATE_ACTION_STOP:
    case SLATE_ACTION_CLOSE:
        if (resource->kind != SLATE_KIND_COVER || !no_value(request)) {
            return ESP_ERR_INVALID_ARG;
        }
        out->cover.motion = request->action == SLATE_ACTION_OPEN
                                ? SLATE_COVER_OPENING
                                : request->action == SLATE_ACTION_CLOSE ? SLATE_COVER_CLOSING
                                                                        : SLATE_COVER_IDLE;
        return ESP_OK;

    case SLATE_ACTION_SET_POSITION:
        if (resource->kind != SLATE_KIND_COVER ||
            request->value_type != SLATE_ACTION_VALUE_NUMBER) {
            return ESP_ERR_INVALID_ARG;
        }
        if (request->value.number < resource->capabilities.position_min ||
            request->value.number > resource->capabilities.position_max) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (out->cover.position == SLATE_STATE_ABSENT ||
            request->value.number == out->cover.position) {
            out->cover.motion = SLATE_COVER_IDLE;
        } else {
            out->cover.motion = request->value.number > out->cover.position
                                    ? SLATE_COVER_OPENING
                                    : SLATE_COVER_CLOSING;
        }
        out->cover.position = (int16_t) request->value.number;
        return ESP_OK;

    case SLATE_ACTION_ACTIVATE:
        return resource->kind == SLATE_KIND_SCENE && no_value(request) ? ESP_OK
                                                                       : ESP_ERR_INVALID_ARG;
    case SLATE_ACTION_COUNT:
        break;
    }
    return ESP_ERR_INVALID_ARG;
}

static bool snapshot_matches(const pending_t *pending, const slate_snapshot_t *snapshot)
{
    if (pending->kind != snapshot->kind) {
        return false;
    }
    switch (pending->action) {
    case SLATE_ACTION_TOGGLE:
    case SLATE_ACTION_SET_POWER:
        if (pending->kind == SLATE_KIND_LIGHT) {
            return snapshot->state.light.on == pending->optimistic.light.on;
        }
        return snapshot->state.cover.motion == pending->optimistic.cover.motion;
    case SLATE_ACTION_SET_BRIGHTNESS:
        return snapshot->state.light.brightness == pending->optimistic.light.brightness;
    case SLATE_ACTION_SET_COLOR_TEMPERATURE:
        return snapshot->state.light.color_temperature ==
               pending->optimistic.light.color_temperature;
    case SLATE_ACTION_OPEN:
        return snapshot->state.cover.motion == SLATE_COVER_OPENING ||
               snapshot->state.cover.position == 100;
    case SLATE_ACTION_STOP:
        return snapshot->state.cover.motion == SLATE_COVER_IDLE;
    case SLATE_ACTION_CLOSE:
        return snapshot->state.cover.motion == SLATE_COVER_CLOSING ||
               snapshot->state.cover.position == 0;
    case SLATE_ACTION_SET_POSITION:
        return snapshot->state.cover.position == pending->optimistic.cover.position;
    case SLATE_ACTION_ACTIVATE:
        return true;
    case SLATE_ACTION_COUNT:
        return false;
    }
    return false;
}

static void state_published(void *ctx, const char *provider, const slate_snapshot_t *snapshot)
{
    (void) ctx;
    bool changed = false;
    bool matched = false;
    uint32_t id = 0;

    LOCK();
    pending_t *pending = find_pair(provider, snapshot->resource);
    if (pending != NULL && pending->phase == SLATE_ACTION_PENDING) {
        matched = snapshot_matches(pending, snapshot);
        id = pending->id;
        remove_locked(pending);
        changed = true;
    }
    UNLOCK();

    if (changed) {
        ESP_LOGD(TAG, "action %" PRIu32 " %s by provider state", id,
                 matched ? "confirmed" : "superseded");
        wake_observer();
    }
}

static void expire_at(int64_t now_us)
{
    bool changed = false;

    LOCK();
    pending_t **link = &s_pending;
    while (*link != NULL) {
        pending_t *item = *link;
        if (item->deadline_us > now_us) {
            link = &item->next;
            continue;
        }
        if (item->phase == SLATE_ACTION_PENDING) {
            ESP_LOGW(TAG, "action %" PRIu32 " on %s:%s timed out", item->id,
                     item->provider, item->resource);
            item->phase = SLATE_ACTION_ERROR;
            item->deadline_us = now_us + ERROR_VISIBLE_US;
            changed = true;
            link = &item->next;
        } else {
            *link = item->next;
            free(item);
            changed = true;
        }
    }
    UNLOCK();

    if (changed) {
        wake_observer();
    }
}

static void deadline_timer(void *ctx)
{
    (void) ctx;
    expire_at(esp_timer_get_time());
}

esp_err_t slate_action_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = deadline_timer,
        .name = "slate_action",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_timer, TIMER_PERIOD_US);
    }
    if (err != ESP_OK) {
        if (s_timer != NULL) {
            esp_timer_delete(s_timer);
            s_timer = NULL;
        }
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return err;
    }

    slate_state_set_publish_observer(state_published, NULL);
    return ESP_OK;
}

esp_err_t slate_action_provider_register(const slate_action_provider_t *provider)
{
    if (provider == NULL || !identity_ok(provider->id, SLATE_PROVIDER_ID_MAX) ||
        provider->dispatch == NULL) {
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
        strlcpy(slot->id, provider->id, sizeof(slot->id));
        slot->dispatch = provider->dispatch;
        slot->ctx = provider->ctx;
    }
    UNLOCK();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "provider \"%s\" registered", provider->id);
    }
    return err;
}

esp_err_t slate_action_dispatch(const slate_action_request_t *request, uint32_t *out_id)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (request == NULL || !identity_ok(request->provider, SLATE_PROVIDER_ID_MAX) ||
        !identity_ok(request->resource, SLATE_RESOURCE_ID_MAX) ||
        (unsigned) request->action >= SLATE_ACTION_COUNT ||
        (unsigned) request->value_type > SLATE_ACTION_VALUE_NUMBER) {
        return ESP_ERR_INVALID_ARG;
    }

    slate_resource_t resource;
    esp_err_t err = slate_state_get(request->provider, request->resource, &resource);
    if (err != ESP_OK) {
        return err;
    }
    if (resource.presentation != SLATE_PRESENT_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!slate_capabilities_have(&resource.capabilities, request->action)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    slate_state_value_t optimistic;
    err = expected_state(&resource, request, &optimistic);
    if (err != ESP_OK) {
        return err;
    }

    pending_t *fresh = allocate_pending();
    if (fresh == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(fresh->provider, request->provider, sizeof(fresh->provider));
    strlcpy(fresh->resource, request->resource, sizeof(fresh->resource));
    fresh->kind = resource.kind;
    fresh->action = request->action;
    fresh->optimistic = optimistic;
    fresh->phase = SLATE_ACTION_PENDING;
    fresh->deadline_us = esp_timer_get_time() + ACTION_TIMEOUT_US;

    slate_action_dispatch_fn dispatch = NULL;
    void *dispatch_ctx = NULL;
    LOCK();
    provider_t *provider = find_provider(request->provider);
    if (provider != NULL) {
        dispatch = provider->dispatch;
        dispatch_ctx = provider->ctx;
    }
    if (dispatch == NULL) {
        UNLOCK();
        free(fresh);
        return ESP_ERR_NOT_FOUND;
    }

    pending_t *previous = find_pair(request->provider, request->resource);
    if (previous != NULL) {
        remove_locked(previous);
    }
    s_next_id++;
    if (s_next_id == 0) {
        s_next_id++;
    }
    fresh->id = s_next_id;
    uint32_t id = fresh->id;
    fresh->next = s_pending;
    s_pending = fresh;
    UNLOCK();

    if (out_id != NULL) {
        *out_id = id;
    }
    wake_observer();

    err = dispatch(dispatch_ctx, id, request);
    if (err != ESP_OK) {
        LOCK();
        pending_t *current = find_id(request->provider, id);
        if (current != NULL && current->phase == SLATE_ACTION_PENDING) {
            current->phase = SLATE_ACTION_ERROR;
            current->deadline_us = esp_timer_get_time() + ERROR_VISIBLE_US;
        }
        UNLOCK();
        wake_observer();
    }
    return err;
}

void slate_action_result(const char *provider, uint32_t id, bool success, const char *error)
{
    if (s_lock == NULL || provider == NULL || id == 0) {
        return;
    }

    bool changed = false;
    bool ignored = false;
    LOCK();
    pending_t *pending = find_id(provider, id);
    if (pending == NULL || pending->phase != SLATE_ACTION_PENDING) {
        ignored = true;
    } else if (!success) {
        pending->phase = SLATE_ACTION_ERROR;
        pending->deadline_us = esp_timer_get_time() + ERROR_VISIBLE_US;
        changed = true;
    } else if (pending->kind == SLATE_KIND_SCENE && pending->action == SLATE_ACTION_ACTIVATE) {
        remove_locked(pending);
        changed = true;
    } else {
        /* Stateful success deliberately leaves the entry unchanged: delivery
         * was accepted, but only a provider snapshot confirms physical state. */
    }
    UNLOCK();

    if (ignored) {
        ESP_LOGD(TAG, "ignored late or duplicate result for %s action %" PRIu32, provider, id);
    } else if (!success) {
        ESP_LOGW(TAG, "%s action %" PRIu32 " failed: %s", provider, id,
                 error != NULL ? error : "no reason given");
    }
    if (changed) {
        wake_observer();
    }
}

void slate_action_provider_unavailable(const char *provider, const char *error)
{
    if (s_lock == NULL || provider == NULL) {
        return;
    }

    bool changed = false;
    int64_t deadline = esp_timer_get_time() + ERROR_VISIBLE_US;
    LOCK();
    for (pending_t *item = s_pending; item != NULL; item = item->next) {
        if (item->phase == SLATE_ACTION_PENDING && strcmp(item->provider, provider) == 0) {
            item->phase = SLATE_ACTION_ERROR;
            item->deadline_us = deadline;
            changed = true;
        }
    }
    UNLOCK();

    if (changed) {
        ESP_LOGW(TAG, "provider %s unavailable: %s", provider,
                 error != NULL ? error : "transport disconnected");
        wake_observer();
    }
}

esp_err_t slate_action_feedback(const char *provider, const char *resource,
                                slate_action_feedback_t *out)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (provider == NULL || resource == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    LOCK();
    const pending_t *pending = find_pair(provider, resource);
    if (pending != NULL) {
        out->phase = pending->phase;
        out->id = pending->id;
        if (pending->phase == SLATE_ACTION_PENDING) {
            out->optimistic = pending->optimistic;
        }
    }
    UNLOCK();
    return ESP_OK;
}

void slate_action_set_wake(slate_action_wake_fn wake, void *ctx)
{
    if (s_lock == NULL) {
        return;
    }
    LOCK();
    s_wake = wake;
    s_wake_ctx = ctx;
    UNLOCK();
}

#ifdef SLATE_ACTION_SELFTEST

typedef struct {
    esp_err_t result;
    unsigned calls;
    uint32_t last_id;
    slate_action_t last_action;
    slate_action_value_type_t last_value_type;
    int32_t last_value;
} fixture_t;

static unsigned s_selftest_wakes;

static esp_err_t fixture_dispatch(void *ctx, uint32_t id,
                                  const slate_action_request_t *request)
{
    fixture_t *fixture = ctx;
    fixture->calls++;
    fixture->last_id = id;
    fixture->last_action = request->action;
    fixture->last_value_type = request->value_type;
    fixture->last_value = request->value_type == SLATE_ACTION_VALUE_BOOL
                              ? request->value.boolean
                              : request->value_type == SLATE_ACTION_VALUE_NUMBER
                                    ? request->value.number
                                    : 0;
    return fixture->result;
}

static void fixture_wake(void *ctx)
{
    (void) ctx;
    s_selftest_wakes++;
}

static void clear_pending(void)
{
    LOCK();
    while (s_pending != NULL) {
        remove_locked(s_pending);
    }
    UNLOCK();
}

esp_err_t slate_action_selftest(void)
{
    int checks = 0;
    int failures = 0;
#define CHECK(condition, name)                                                \
    do {                                                                      \
        bool passed_ = (condition);                                           \
        checks++;                                                             \
        failures += !passed_;                                                 \
        ESP_LOGI(TAG, "selftest: %-46s %s", name, passed_ ? "PASS" : "FAIL"); \
    } while (0)

    static const char *PROVIDER = "action-test";
    static const char *LIGHT = "lamp";
    static const char *SCENE = "relax";
    static fixture_t fixture;
    memset(&fixture, 0, sizeof(fixture));
    fixture.result = ESP_OK;
    s_selftest_wakes = 0;

    const slate_state_provider_t state_provider = {.id = PROVIDER};
    const slate_action_provider_t action_provider = {
        .id = PROVIDER,
        .dispatch = fixture_dispatch,
        .ctx = &fixture,
    };
    const slate_binding_t bindings[] = {
        {.provider = PROVIDER, .resource = LIGHT, .kind = SLATE_KIND_LIGHT},
        {.provider = PROVIDER, .resource = SCENE, .kind = SLATE_KIND_SCENE},
    };
    slate_snapshot_t lamp = {
        .resource = LIGHT,
        .kind = SLATE_KIND_LIGHT,
        .name = "Fixture lamp",
        .available = true,
        .capabilities = {
            .actions = (1u << SLATE_ACTION_TOGGLE) | (1u << SLATE_ACTION_SET_POWER) |
                       (1u << SLATE_ACTION_SET_BRIGHTNESS),
            .brightness_min = 0,
            .brightness_max = 100,
        },
        .state.light = {
            .on = true,
            .brightness = 62,
            .color_temperature = SLATE_STATE_ABSENT,
        },
    };
    const slate_snapshot_t scene = {
        .resource = SCENE,
        .kind = SLATE_KIND_SCENE,
        .available = true,
        .capabilities.actions = 1u << SLATE_ACTION_ACTIVATE,
    };

    CHECK(slate_state_provider_register(&state_provider) == ESP_OK,
          "fixture state provider registered");
    CHECK(slate_state_provider_set_status(PROVIDER, SLATE_PROVIDER_ONLINE) == ESP_OK,
          "fixture state provider online");
    CHECK(slate_action_provider_register(&action_provider) == ESP_OK,
          "fixture action adapter registered");
    CHECK(slate_state_bind(bindings, sizeof(bindings) / sizeof(bindings[0])) == ESP_OK,
          "fixture resources bound");
    CHECK(slate_state_publish(PROVIDER, &lamp) == ESP_OK &&
              slate_state_publish(PROVIDER, &scene) == ESP_OK,
          "confirmed fixture states published");

    slate_action_set_wake(fixture_wake, NULL);

    slate_action_request_t request = {
        .provider = PROVIDER,
        .resource = LIGHT,
        .action = SLATE_ACTION_SET_COLOR_TEMPERATURE,
        .value_type = SLATE_ACTION_VALUE_NUMBER,
        .value.number = 3000,
    };
    CHECK(slate_action_dispatch(&request, NULL) == ESP_ERR_NOT_SUPPORTED,
          "unadvertised capability refused before transport");
    CHECK(fixture.calls == 0, "refused capability never reaches adapter");

    request.action = (slate_action_t) -1;
    CHECK(slate_action_dispatch(&request, NULL) == ESP_ERR_INVALID_ARG,
          "invalid action enum refused safely");
    request.action = SLATE_ACTION_SET_COLOR_TEMPERATURE;
    request.value_type = (slate_action_value_type_t) -1;
    CHECK(slate_action_dispatch(&request, NULL) == ESP_ERR_INVALID_ARG,
          "invalid value enum refused safely");
    request.value_type = SLATE_ACTION_VALUE_NUMBER;

    request.action = SLATE_ACTION_SET_POWER;
    request.value_type = SLATE_ACTION_VALUE_BOOL;
    request.value.boolean = true;
    uint32_t power_id = 0;
    CHECK(slate_action_dispatch(&request, &power_id) == ESP_OK &&
              fixture.last_value_type == SLATE_ACTION_VALUE_BOOL && fixture.last_value == 1,
          "boolean payload remains typed at adapter boundary");
    CHECK(slate_state_publish(PROVIDER, &lamp) == ESP_OK,
          "boolean action confirmed by real state");

    request.action = SLATE_ACTION_SET_BRIGHTNESS;
    request.value_type = SLATE_ACTION_VALUE_NUMBER;
    request.value.number = 101;
    CHECK(slate_action_dispatch(&request, NULL) == ESP_ERR_INVALID_SIZE,
          "value outside advertised range refused");
    request.action = SLATE_ACTION_TOGGLE;
    request.value_type = SLATE_ACTION_VALUE_BOOL;
    request.value.boolean = true;
    CHECK(slate_action_dispatch(&request, NULL) == ESP_ERR_INVALID_ARG,
          "parameter on a parameterless action refused");

    request.value_type = SLATE_ACTION_VALUE_NONE;
    uint32_t first_id = 0;
    CHECK(slate_action_dispatch(&request, &first_id) == ESP_OK && first_id != 0,
          "toggle dispatched with a correlation id");
    slate_action_feedback_t feedback;
    CHECK(slate_action_feedback(PROVIDER, LIGHT, &feedback) == ESP_OK &&
              feedback.phase == SLATE_ACTION_PENDING && !feedback.optimistic.light.on,
          "toggle applies optimistic power state");
    slate_action_result("another-provider", first_id, false, "wrong_owner");
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_PENDING,
          "another provider cannot complete the action");
    slate_action_result(PROVIDER, first_id, true, NULL);
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_PENDING,
          "delivery success does not confirm physical state");
    lamp.state.light.on = false;
    CHECK(slate_state_publish(PROVIDER, &lamp) == ESP_OK, "matching real state published");
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_IDLE, "matching state clears pending");

    uint32_t disagree_id = 0;
    CHECK(slate_action_dispatch(&request, &disagree_id) == ESP_OK,
          "second toggle dispatched");
    /* The confirmed state is off, so the optimistic value is on. Publishing off
     * again is a real update that disagrees and must win immediately. */
    CHECK(slate_state_publish(PROVIDER, &lamp) == ESP_OK,
          "disagreeing real state published");
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_IDLE,
          "disagreeing state supersedes optimistic state");
    slate_action_result(PROVIDER, disagree_id, false, "late_failure");
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_IDLE, "late result is ignored");

    request.action = SLATE_ACTION_SET_BRIGHTNESS;
    request.value_type = SLATE_ACTION_VALUE_NUMBER;
    request.value.number = 40;
    uint32_t failed_id = 0;
    CHECK(slate_action_dispatch(&request, &failed_id) == ESP_OK,
          "brightness action dispatched");
    slate_action_result(PROVIDER, failed_id, false, "fixture_refused");
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_ERROR,
          "explicit failure reverts and exposes brief error");
    slate_resource_t confirmed;
    CHECK(slate_state_get(PROVIDER, LIGHT, &confirmed) == ESP_OK &&
              confirmed.state.light.brightness == 62,
          "explicit failure leaves confirmed state untouched");
    expire_at(esp_timer_get_time() + ERROR_VISIBLE_US + 1);
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_IDLE, "brief error expires");

    fixture.result = ESP_ERR_INVALID_STATE;
    uint32_t delivery_id = 0;
    CHECK(slate_action_dispatch(&request, &delivery_id) == ESP_ERR_INVALID_STATE,
          "transport refusal is immediate");
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_ERROR,
          "transport refusal also exposes brief error");
    expire_at(esp_timer_get_time() + ERROR_VISIBLE_US + 1);

    fixture.result = ESP_OK;
    uint32_t timeout_id = 0;
    CHECK(slate_action_dispatch(&request, &timeout_id) == ESP_OK,
          "action awaiting confirmation dispatched");
    slate_action_result(PROVIDER, timeout_id, true, NULL);
    expire_at(esp_timer_get_time() + ACTION_TIMEOUT_US + 1);
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_ERROR,
          "accepted action without snapshot still times out");
    expire_at(esp_timer_get_time() + ACTION_TIMEOUT_US + ERROR_VISIBLE_US + 2);

    request.value.number = 55;
    uint32_t disconnected_id = 0;
    slate_action_dispatch(&request, &disconnected_id);
    slate_action_provider_unavailable("another-provider", "wrong_owner");
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_PENDING,
          "another provider disconnect does not revert action");
    slate_action_provider_unavailable(PROVIDER, "fixture_disconnected");
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_ERROR, "provider disconnect reverts immediately");
    expire_at(esp_timer_get_time() + ERROR_VISIBLE_US + 1);

    request.value.number = 20;
    uint32_t old_id = 0;
    slate_action_dispatch(&request, &old_id);
    request.value.number = 30;
    uint32_t new_id = 0;
    slate_action_dispatch(&request, &new_id);
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_PENDING && feedback.id == new_id &&
              feedback.optimistic.light.brightness == 30,
          "new action supersedes older overlay");
    slate_action_result(PROVIDER, old_id, false, "superseded");
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_PENDING && feedback.id == new_id,
          "superseded result cannot revert newer action");
    slate_action_result(PROVIDER, new_id, false, "fixture_refused");
    slate_action_result(PROVIDER, new_id, true, NULL);
    slate_action_feedback(PROVIDER, LIGHT, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_ERROR, "duplicate result is ignored");
    expire_at(esp_timer_get_time() + ERROR_VISIBLE_US + 1);

    request.resource = SCENE;
    request.action = SLATE_ACTION_ACTIVATE;
    request.value_type = SLATE_ACTION_VALUE_NONE;
    uint32_t scene_id = 0;
    CHECK(slate_action_dispatch(&request, &scene_id) == ESP_OK,
          "stateless scene activation dispatched");
    slate_action_result(PROVIDER, scene_id, true, NULL);
    slate_action_feedback(PROVIDER, SCENE, &feedback);
    CHECK(feedback.phase == SLATE_ACTION_IDLE,
          "accepted stateless action completes without snapshot");

    lamp.available = false;
    slate_state_publish(PROVIDER, &lamp);
    request.resource = LIGHT;
    request.action = SLATE_ACTION_TOGGLE;
    CHECK(slate_action_dispatch(&request, NULL) == ESP_ERR_INVALID_STATE,
          "unavailable resource cannot be acted on");
    lamp.available = true;
    lamp.capabilities.actions = 0;
    slate_state_publish(PROVIDER, &lamp);
    CHECK(slate_action_dispatch(&request, NULL) == ESP_ERR_NOT_SUPPORTED,
          "read-only resource cannot be acted on");

    CHECK(s_selftest_wakes > 0, "feedback changes wake the observer");

    slate_action_set_wake(NULL, NULL);
    clear_pending();
    slate_state_bind(NULL, 0);
    ESP_LOGI(TAG, "selftest: %d checks, %d failure(s)", checks, failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
#undef CHECK
}

#endif
