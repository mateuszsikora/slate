/*
 * Slate — the direct integration provider. See include/slate_direct.h for what
 * this component owns and what it deliberately leaves to #18.
 *
 * design.md ADR-3, §4.1, §4.2, §5.1 to §5.4.
 */

#include "slate_direct.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "slate_action.h"
#include "slate_api.h"
#include "slate_state.h"
#include "slate_wifi.h"
#include "slate_ws.h"

#ifdef SLATE_DIRECT_SELFTEST
#include "esp_timer.h"
#endif

static const char *TAG = "direct";

#define SLATE_DIRECT_PROVIDER_ID "direct"

/*
 * A snapshot is not a document, and this is not §3.1's 64 KB.
 *
 * The largest thing §5.2 can say about one resource is a saturated identity
 * (63 + 63 + 31 characters), a state object and every capability with its range,
 * which lands a little over 450 bytes. 768 leaves room for the whitespace a
 * hand-written `curl` body arrives with and still fits the stack of the one HTTP
 * task, which is the reason it is not simply generous: a body buffer that had to
 * be allocated would make the endpoint fail in a new way under memory pressure.
 */
#define BODY_MAX 768

/*
 * The API starts before the radio (§9), so the provider initially has no Wi-Fi
 * lifecycle to observe and retains §5.4's ordinary publish-only status. Once
 * slate_direct_start() runs, this becomes the station's actual state and every
 * later consumer attach/detach is constrained by it: a WebSocket callback
 * racing the station-down event cannot accidentally make stale values fresh.
 */
static bool s_network_up = true;
static bool s_consumer_attached;
static SemaphoreHandle_t s_status_lock;
static StaticSemaphore_t s_status_lock_storage;
static bool s_initialized;
static bool s_started;

#ifdef SLATE_DIRECT_SELFTEST
static void selftest_consumer_attached(void);
#endif

/* --- §5.2 parsing -------------------------------------------------------- */

/**
 * An optional string field: present, absent, or the wrong shape entirely.
 *
 * `null` reads as absent rather than as an error, because §5.2's optional
 * fields are the ones a publisher omits and a JSON generator that writes `null`
 * for an empty column is describing the same absence.
 */
static bool optional_string(const cJSON *object, const char *name, const char **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL || cJSON_IsNull(item)) {
        *out = NULL;
        return true;
    }
    if (!cJSON_IsString(item)) {
        return false;
    }
    *out = item->valuestring;
    return true;
}

/**
 * An optional whole-number field, in the store's absent-or-in-range spelling.
 *
 * The bounds are int16_t's rather than the semantic ones: §5.2's percentages and
 * kelvin are checked by slate_state_publish() for every provider, and a second
 * opinion here would be a second place to keep in step with §7.
 */
static bool optional_number(const cJSON *object, const char *name, int16_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    *out = SLATE_STATE_ABSENT;
    if (item == NULL || cJSON_IsNull(item)) {
        return true;
    }
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
        return false;
    }

    double rounded = round(item->valuedouble);
    if (rounded <= SLATE_STATE_ABSENT || rounded > INT16_MAX) {
        return false;
    }
    *out = (int16_t) rounded;
    return true;
}

static bool parse_light(const cJSON *state, slate_light_state_t *out)
{
    const cJSON *power = cJSON_GetObjectItemCaseSensitive(state, "power");
    if (!cJSON_IsString(power)) {
        return false;
    }
    if (strcmp(power->valuestring, "on") == 0) {
        out->on = true;
    } else if (strcmp(power->valuestring, "off") == 0) {
        out->on = false;
    } else {
        return false;
    }
    return optional_number(state, "brightness", &out->brightness) &&
           optional_number(state, "color_temperature", &out->color_temperature);
}

static bool parse_cover(const cJSON *state, slate_cover_state_t *out)
{
    if (!optional_number(state, "position", &out->position)) {
        return false;
    }

    /* The three names are slate_cover_motion_t's, which is the vocabulary §7.2's
     * animated indicator reads. §5.2 names the concept — "position and movement
     * state" — and leaves the spelling to the store rather than to this route. */
    const char *motion = NULL;
    if (!optional_string(state, "motion", &motion)) {
        return false;
    }
    if (motion == NULL || strcmp(motion, "idle") == 0) {
        out->motion = SLATE_COVER_IDLE;
    } else if (strcmp(motion, "opening") == 0) {
        out->motion = SLATE_COVER_OPENING;
    } else if (strcmp(motion, "closing") == 0) {
        out->motion = SLATE_COVER_CLOSING;
    } else {
        return false;
    }
    return true;
}

static bool parse_sensor(const cJSON *state, slate_sensor_state_t *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(state, "value");
    if (cJSON_IsNumber(value)) {
        out->numeric = true;
        out->value = value->valuedouble;
    } else if (cJSON_IsString(value)) {
        /* Presentation, so it truncates rather than refusing — §5.2's own rule,
         * and §7.5 ellipsizes what does not fit on the tile anyway. */
        strlcpy(out->text, value->valuestring, sizeof(out->text));
    } else {
        return false;
    }

    const char *unit = NULL;
    const char *measurement = NULL;
    if (!optional_string(state, "unit", &unit) ||
        !optional_string(state, "measurement", &measurement)) {
        return false;
    }
    if (unit != NULL) {
        strlcpy(out->unit, unit, sizeof(out->unit));
    }

    /* §5.2: "Unknown state fields and capabilities are ignored." A measurement
     * this firmware does not have an icon for is the same forward-compatibility
     * case one field up, so it reads as unmeasured rather than as a refusal —
     * the value, its unit and its name are all still renderable. */
    if (measurement != NULL && !slate_measurement_from_str(measurement, &out->measurement)) {
        out->measurement = SLATE_MEASUREMENT_NONE;
    }
    return true;
}

/**
 * §5.2's `capabilities`, whose two spellings are one rule.
 *
 * "Absent capabilities mean read-only", so an absent object is a resource that
 * only reports and a zeroed struct says exactly that. An unknown action name is
 * ignored for forward compatibility; a known one carrying something that is
 * neither `true`, `false` nor a range is not an unknown capability but a
 * malformed one, and §5.4 has `invalid_state` for it.
 */
static bool parse_capabilities(const cJSON *object, slate_capabilities_t *out)
{
    if (object == NULL || cJSON_IsNull(object)) {
        return true;
    }
    if (!cJSON_IsObject(object)) {
        return false;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, object) {
        slate_action_t action;
        if (item->string == NULL || !slate_action_from_str(item->string, &action)) {
            continue;
        }

        if (cJSON_IsBool(item)) {
            if (cJSON_IsTrue(item)) {
                out->actions |= (uint16_t) (1u << action);
            }
            continue;
        }
        if (!cJSON_IsObject(item)) {
            return false;
        }

        out->actions |= (uint16_t) (1u << action);
        int16_t min = SLATE_STATE_ABSENT;
        int16_t max = SLATE_STATE_ABSENT;
        if (!optional_number(item, "min", &min) || !optional_number(item, "max", &max)) {
            return false;
        }

        /* An unstated bound is zero rather than absent: the store reads these
         * as a range and settles what an unstated one means (§5.2), which it
         * cannot do if half of one arrives as a sentinel. */
        min = min == SLATE_STATE_ABSENT ? 0 : min;
        max = max == SLATE_STATE_ABSENT ? 0 : max;
        switch (action) {
        case SLATE_ACTION_SET_BRIGHTNESS:
            out->brightness_min = min;
            out->brightness_max = max;
            break;
        case SLATE_ACTION_SET_COLOR_TEMPERATURE:
            out->color_temperature_min = min;
            out->color_temperature_max = max;
            break;
        case SLATE_ACTION_SET_POSITION:
            out->position_min = min;
            out->position_max = max;
            break;
        default:
            break; /* A range on a capability that has none is ignored, not fatal. */
        }
    }
    return true;
}

/** Fill `out` from §5.2's document. False is `400 invalid_state`. */
static bool parse_snapshot(const cJSON *root, slate_snapshot_t *out)
{
    const cJSON *resource = cJSON_GetObjectItemCaseSensitive(root, "resource");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
    const cJSON *available = cJSON_GetObjectItemCaseSensitive(root, "available");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");

    /*
     * §5.2: "`provider`, `resource`, `kind`, `available` and `state` are
     * required." `provider` is the one it does not ask for here, because §5.4
     * fixes it to `direct` at the endpoint — a body that could name its own
     * would be a body that can write into the Home Assistant half of the store.
     *
     * A body that carries one anyway is ignored rather than refused, which is
     * §3.1's rule for a field this route does not read. Ignoring is also the
     * safe half of the pair: the value never reaches slate_state_publish(),
     * which is called with the id above and cannot be told otherwise.
     */
    if (!cJSON_IsString(resource) || resource->valuestring[0] == '\0' ||
        !cJSON_IsString(kind) || !slate_kind_from_str(kind->valuestring, &out->kind) ||
        !cJSON_IsBool(available) || !cJSON_IsObject(state)) {
        return false;
    }
    out->resource = resource->valuestring;
    out->available = cJSON_IsTrue(available);

    if (!optional_string(root, "name", &out->name) ||
        !optional_string(root, "area", &out->area) ||
        !parse_capabilities(cJSON_GetObjectItemCaseSensitive(root, "capabilities"),
                            &out->capabilities)) {
        return false;
    }

    switch (out->kind) {
    case SLATE_KIND_LIGHT:
        return parse_light(state, &out->state.light);
    case SLATE_KIND_COVER:
        return parse_cover(state, &out->state.cover);
    case SLATE_KIND_SENSOR:
        return parse_sensor(state, &out->state.sensor);
    case SLATE_KIND_SCENE:
        return true; /* §5.2: "`scene`: stateless". */
    }
    return false;
}

/* --- POST /direct/state (§4.1, §5.4) ------------------------------------- */

/** Read the whole body, or say why not. NULL on success. */
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
        received += (size_t) chunk;
    }
    buffer[received] = '\0';
    return NULL;
}

static esp_err_t state_handler(httpd_req_t *req)
{
    char body[BODY_MAX];
    const char *problem = read_body(req, body, sizeof(body));
    if (problem != NULL) {
        return slate_api_refuse(req,
                                strcmp(problem, "too_large") == 0 ? "413 Payload Too Large"
                                                                  : "400 Bad Request",
                                problem);
    }

    cJSON *root = cJSON_Parse(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return slate_api_refuse(req, "400 Bad Request", "invalid_json");
    }

    slate_snapshot_t snapshot = {0};
    bool parsed = parse_snapshot(root, &snapshot);

    /* The accepted id outlives `root`, which is deleted before the response is
     * built — §5.4 answers with the resource it took, and borrowing freed cJSON
     * storage to say so is the mistake `POST /wifi` documents next door. */
    char accepted[SLATE_RESOURCE_ID_MAX + 1] = {0};
    esp_err_t err = ESP_OK;
    if (parsed) {
        strlcpy(accepted, snapshot.resource, sizeof(accepted));
        err = slate_state_publish(SLATE_DIRECT_PROVIDER_ID, &snapshot);
    }
    cJSON_Delete(root);

    if (!parsed) {
        return slate_api_refuse(req, "400 Bad Request", "invalid_state");
    }

    /*
     * §5.4's three refusals, and they are the store's return values rather than
     * a second opinion formed here: an HA mapping bug produces the same nonsense
     * as a malformed body, and the place that can refuse both identically is the
     * one both go through. "None disturbs the last confirmed value."
     */
    switch (err) {
    case ESP_ERR_NOT_FOUND:
        ESP_LOGW(TAG, "%s is not bound by the active configuration", accepted);
        return slate_api_refuse(req, "404 Not Found", "resource_not_bound");
    case ESP_ERR_INVALID_STATE:
        ESP_LOGW(TAG, "%s was published as a different kind than its binding", accepted);
        return slate_api_refuse(req, "409 Conflict", "kind_mismatch");
    case ESP_ERR_INVALID_ARG:
        return slate_api_refuse(req, "400 Bad Request", "invalid_state");
    case ESP_OK:
        break;
    default:
        return slate_api_refuse(req, "500 Internal Server Error", "out_of_memory");
    }

    /*
     * 202, because §5.4 says so and because it is true: the snapshot is accepted
     * and the tile that renders it is rebuilt on the UI task, which has not
     * necessarily run yet. The body carries the id it was taken for, which is
     * what a script publishing several resources in a loop matches against.
     */
    cJSON *response = cJSON_CreateObject();
    if (response == NULL || cJSON_AddStringToObject(response, "resource", accepted) == NULL) {
        cJSON_Delete(response);
        return slate_api_send_json(req, NULL);
    }
    httpd_resp_set_status(req, "202 Accepted");
    return slate_api_send_json(req, response);
}

/* --- Actions (§4.2, §5.3, §5.4) ------------------------------------------ */

/**
 * §5.4's status rule, which is a rule about somebody else's tiles.
 *
 * `degraded` rather than `offline` with no consumer attached, because §5.2 makes
 * `offline` stale a provider's resources and a publish-only script has nothing
 * wrong with it: "a read-only direct sensor remains fresh even when no action
 * consumer is attached."
 */
/* Caller holds s_status_lock. */
static void update_provider_status_locked(void)
{
    slate_state_provider_set_status(
        SLATE_DIRECT_PROVIDER_ID,
        !s_network_up ? SLATE_PROVIDER_OFFLINE
                      : s_consumer_attached ? SLATE_PROVIDER_ONLINE
                                            : SLATE_PROVIDER_DEGRADED);
}

static void consumer_changed(void *ctx, bool attached)
{
    (void) ctx;
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_consumer_attached = attached;
    update_provider_status_locked();
    if (!attached) {
        slate_action_provider_unavailable(SLATE_DIRECT_PROVIDER_ID, "consumer_disconnected");
    }
    xSemaphoreGive(s_status_lock);
#ifdef SLATE_DIRECT_SELFTEST
    if (attached) {
        selftest_consumer_attached();
    }
#endif
}

static void network_changed(bool connected)
{
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_network_up = connected;
    if (!connected) {
        /* A consumer belongs to the station connection that carried its
         * WebSocket. Do not let that stale attachment make a fast reconnect
         * look online before the replacement session attaches. */
        s_consumer_attached = false;
    }
    update_provider_status_locked();
    if (!connected) {
        /* The WebSocket close normally reaches consumer_changed() as well, but
         * the station event is the first authoritative loss and pending state
         * must revert even if the transport takes longer to notice it. */
        slate_action_provider_unavailable(SLATE_DIRECT_PROVIDER_ID, "network_offline");
    }
    xSemaphoreGive(s_status_lock);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) data;

    if (id == SLATE_WIFI_EVENT_CONNECTED) {
        network_changed(true);
    } else if (id == SLATE_WIFI_EVENT_DISCONNECTED) {
        network_changed(false);
    }
}

static void action_result(void *ctx, uint32_t id, bool success, const char *error)
{
    (void) ctx;
    if (!success) {
        ESP_LOGW(TAG, "action %" PRIu32 " failed: %s", id, error ? error : "no reason given");
    } else {
        /* Debug rather than info, and present rather than absent: until #18
         * installs a handler this is the only trace a delivered action leaves,
         * and a seam with no observable side effect is one whose bring-up
         * starts by wondering whether the frame ever arrived. */
        ESP_LOGD(TAG, "action %" PRIu32 " accepted by the consumer", id);
    }

    slate_action_result(SLATE_DIRECT_PROVIDER_ID, id, success, error);
}

static esp_err_t bus_dispatch(void *ctx, uint32_t id, const slate_action_request_t *request)
{
    (void) ctx;
    slate_direct_action_t action = {
        .resource = request->resource,
        .action = request->action,
        .value_type = request->value_type,
    };
    if (request->value_type == SLATE_ACTION_VALUE_BOOL) {
        action.value.boolean = request->value.boolean;
    } else if (request->value_type == SLATE_ACTION_VALUE_NUMBER) {
        action.value.number = request->value.number;
    }
    return slate_direct_dispatch(id, &action);
}

esp_err_t slate_direct_dispatch(uint32_t id, const slate_direct_action_t *action)
{
    if (action == NULL || action->resource == NULL || action->resource[0] == '\0' ||
        (unsigned) action->action >= SLATE_ACTION_COUNT ||
        (unsigned) action->value_type > SLATE_ACTION_VALUE_NUMBER) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_status_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Built with cJSON rather than printed, because the resource id is opaque
     * (§3.3) and a quote inside one would otherwise produce a frame that parses
     * as something else on the client. */
    cJSON *frame = cJSON_CreateObject();
    cJSON *params = frame ? cJSON_AddObjectToObject(frame, "params") : NULL;
    bool ok = frame && params &&
              cJSON_AddStringToObject(frame, "type", "action") != NULL &&
              cJSON_AddNumberToObject(frame, "id", id) != NULL &&
              cJSON_AddStringToObject(frame, "provider", SLATE_DIRECT_PROVIDER_ID) != NULL &&
              cJSON_AddStringToObject(frame, "resource", action->resource) != NULL &&
              cJSON_AddStringToObject(frame, "action", slate_action_str(action->action)) != NULL;
    if (ok && action->value_type == SLATE_ACTION_VALUE_BOOL) {
        ok = cJSON_AddBoolToObject(params, "value", action->value.boolean) != NULL;
    } else if (ok && action->value_type == SLATE_ACTION_VALUE_NUMBER) {
        ok = cJSON_AddNumberToObject(params, "value", action->value.number) != NULL;
    }

    char *text = ok ? cJSON_PrintUnformatted(frame) : NULL;
    cJSON_Delete(frame);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* The length is the transport's to judge, and only its to judge: a second
     * ceiling here would be a constant to keep in step with one in another
     * component, which is the kind of pair that drifts quietly. */
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    esp_err_t err = s_network_up && s_consumer_attached
                        ? slate_ws_provider_send(SLATE_DIRECT_PROVIDER_ID, text, strlen(text))
                        : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_status_lock);
    cJSON_free(text);

    if (err != ESP_OK) {
        /* §5.4: with no consumer attached "new actions fail immediately rather
         * than waiting three seconds", which is a better tile than a pulse that
         * runs out. The caller sees it as a return value, not as a callback. */
        ESP_LOGW(TAG, "action %" PRIu32 " on %s not delivered: %s", id, action->resource,
                 esp_err_to_name(err));
    }
    return err;
}

/* --- Lifecycle ----------------------------------------------------------- */

esp_err_t slate_direct_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_status_lock == NULL) {
        s_status_lock = xSemaphoreCreateMutexStatic(&s_status_lock_storage);
        if (s_status_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    /*
     * §5.1: the direct provider "is always present". Registering with the store
     * first, and separately from everything below, is what makes that true of a
     * panel whose HTTP server ran out of handler slots — a binding for `direct`
     * still resolves and its tile shows a missing resource (§7.5) rather than
     * §3.3's missing-provider placeholder, which would be a different and less
     * accurate thing to tell somebody.
     *
     * `subscribe` is NULL because this provider has nothing to subscribe to:
     * §5.4's publishers push, and an id the configuration does not reference is
     * refused by the store on arrival rather than filtered in advance.
     */
    const slate_state_provider_t provider = {
        .id = SLATE_DIRECT_PROVIDER_ID,
    };
    esp_err_t err = slate_state_provider_register(&provider);
    if (err != ESP_OK) {
        return err;
    }
    s_initialized = true;
    slate_state_provider_set_status(SLATE_DIRECT_PROVIDER_ID, SLATE_PROVIDER_DEGRADED);

    const slate_action_provider_t action_provider = {
        .id = SLATE_DIRECT_PROVIDER_ID,
        .dispatch = bus_dispatch,
    };
    esp_err_t action_err = slate_action_provider_register(&action_provider);
    if (action_err != ESP_OK) {
        ESP_LOGE(TAG, "semantic action dispatch unavailable: %s", esp_err_to_name(action_err));
    }

    const slate_ws_provider_t consumer = {
        .id = SLATE_DIRECT_PROVIDER_ID,
        .on_attach = consumer_changed,
        .on_result = action_result,
    };
    err = slate_ws_provider_register(&consumer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "action consumers cannot attach: %s", esp_err_to_name(err));
    }

    const httpd_uri_t state = {
        .uri = SLATE_API_BASE_PATH "/direct/state",
        .method = HTTP_POST,
        .handler = state_handler,
    };
    esp_err_t route_err = slate_api_register_uri(&state, SLATE_API_AUTH_DEVICE_TOKEN);
    if (route_err != ESP_OK) {
        ESP_LOGE(TAG, "state publication unavailable: %s", esp_err_to_name(route_err));
    }

    if (err == ESP_OK && route_err == ESP_OK) {
        ESP_LOGI(TAG, "provider ready: POST " SLATE_API_BASE_PATH "/direct/state");
    }
    if (action_err != ESP_OK) {
        return action_err;
    }
    return err != ESP_OK ? err : route_err;
}

esp_err_t slate_direct_start(void)
{
    if (!s_initialized || s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_event_handler_instance_register(
        SLATE_WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* Serialize the initial snapshot with the event callback. If the station
     * changes after this read, its callback waits and applies the newer state;
     * if it changed before the lock, the snapshot already contains it. */
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    slate_wifi_status_t wifi;
    slate_wifi_status(&wifi);
    s_network_up = wifi.connected;
    update_provider_status_locked();
    s_started = true;
    xSemaphoreGive(s_status_lock);
    return ESP_OK;
}

#ifdef SLATE_DIRECT_SELFTEST

/* --- Development verifier ------------------------------------------------ */

/*
 * What this knob is for, and what it is honestly not.
 *
 * #74's done-when ends with a tap on a tile, which needs #20's tree and #22's
 * light. Everything on this side of them is real and reachable now — but not
 * from outside the device, because nothing binds a resource until #19 parses a
 * configuration, and an unbound panel answers every publication with §5.4's
 * `resource_not_bound`. So the fixture below is what a configuration will be,
 * and it stays bound after the checks run: `tools/direct/` is then a real client
 * talking to a real panel, which is a better test of a public contract (ADR-4)
 * than a loopback socket that shares the firmware's own idea of it.
 *
 * The demonstration dispatch is the other half. With no tile (#22) nothing in
 * the firmware can originate a tap yet, so an attaching consumer is given one
 * through the real action bus to answer. The fixture goes away with #19 and the
 * synthetic action goes away with #20 and #22.
 */
static esp_err_t selftest_bind_fixture(void)
{
    static const slate_binding_t FIXTURE[] = {
        {.provider = SLATE_DIRECT_PROVIDER_ID, .resource = "living-room",
         .kind = SLATE_KIND_LIGHT},
        {.provider = SLATE_DIRECT_PROVIDER_ID, .resource = "hall-temperature",
         .kind = SLATE_KIND_SENSOR},
    };
    esp_err_t err = slate_state_bind(FIXTURE, sizeof(FIXTURE) / sizeof(FIXTURE[0]));
    if (err != ESP_OK) {
        return err;
    }

    const slate_snapshot_t lamp = {
        .resource = "living-room",
        .kind = SLATE_KIND_LIGHT,
        .name = "Living room",
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
    return slate_state_publish(SLATE_DIRECT_PROVIDER_ID, &lamp);
}

static esp_timer_handle_t s_demo_timer;
static uint32_t s_demo_id;

static void selftest_demo_action(void *ctx)
{
    (void) ctx;
    const slate_action_request_t set_power = {
        .provider = SLATE_DIRECT_PROVIDER_ID,
        .resource = "living-room",
        .action = SLATE_ACTION_SET_POWER,
        .value_type = SLATE_ACTION_VALUE_BOOL,
        .value.boolean = false,
    };

    esp_err_t err = slate_action_dispatch(&set_power, &s_demo_id);
    ESP_LOGI(TAG, "selftest: action bus dispatched %" PRIu32 " (%s)", s_demo_id,
             esp_err_to_name(err));
}

/* Deferred rather than sent from the attach callback: that one runs on the HTTP
 * task inside the frame that caused it, and a client which has just attached is
 * not yet reading. */
static void selftest_consumer_attached(void)
{
    if (s_demo_timer) {
        esp_timer_stop(s_demo_timer);
        esp_timer_start_once(s_demo_timer, 2 * 1000 * 1000);
    }
}

esp_err_t slate_direct_selftest(void)
{
    int failures = 0;
#define CHECK(condition, name)                                                \
    do {                                                                      \
        bool passed_ = (condition);                                           \
        failures += !passed_;                                                 \
        ESP_LOGI(TAG, "selftest: %-38s %s", name, passed_ ? "PASS" : "FAIL"); \
    } while (0)

    CHECK(slate_state_provider_status(SLATE_DIRECT_PROVIDER_ID) == SLATE_PROVIDER_DEGRADED,
          "registered and degraded with no consumer");

    CHECK(selftest_bind_fixture() == ESP_OK, "fixture bound in place of a configuration");

    slate_resource_t lamp_state;
    consumer_changed(NULL, true);
    CHECK(slate_state_provider_status(SLATE_DIRECT_PROVIDER_ID) == SLATE_PROVIDER_ONLINE,
          "consumer attachment makes provider online");
    wifi_event(NULL, SLATE_WIFI_EVENT, SLATE_WIFI_EVENT_DISCONNECTED, NULL);
    CHECK(slate_state_provider_status(SLATE_DIRECT_PROVIDER_ID) == SLATE_PROVIDER_OFFLINE,
          "station loss makes the provider offline");
    CHECK(slate_state_get(SLATE_DIRECT_PROVIDER_ID, "living-room", &lamp_state) == ESP_OK &&
              lamp_state.presentation == SLATE_PRESENT_STALE,
          "station loss makes direct state stale");

    wifi_event(NULL, SLATE_WIFI_EVENT, SLATE_WIFI_EVENT_CONNECTED, NULL);
    CHECK(slate_state_provider_status(SLATE_DIRECT_PROVIDER_ID) == SLATE_PROVIDER_DEGRADED,
          "station recovery rejects the stale consumer");
    CHECK(slate_state_get(SLATE_DIRECT_PROVIDER_ID, "living-room", &lamp_state) == ESP_OK &&
              lamp_state.presentation == SLATE_PRESENT_OK,
          "station recovery makes direct state fresh");

    /* §5.4's immediate failure, which is a return value and not a callback: a
     * caller must never have to decide whether an answer is still coming. */
    const slate_action_request_t orphan = {
        .provider = SLATE_DIRECT_PROVIDER_ID,
        .resource = "living-room",
        .action = SLATE_ACTION_TOGGLE,
        .value_type = SLATE_ACTION_VALUE_NONE,
    };
    CHECK(slate_action_dispatch(&orphan, NULL) == ESP_ERR_INVALID_STATE,
          "bus action with no consumer fails immediately");
    CHECK(!slate_ws_provider_is_attached(SLATE_DIRECT_PROVIDER_ID), "nothing attached at boot");

    const slate_direct_action_t nonsense = {.resource = "", .action = SLATE_ACTION_TOGGLE};
    CHECK(slate_direct_dispatch(2, &nonsense) == ESP_ERR_INVALID_ARG,
          "action without a resource is refused");

    const slate_direct_action_t invalid_value = {
        .resource = "living-room",
        .action = SLATE_ACTION_SET_POWER,
        .value_type = (slate_action_value_type_t) -1,
    };
    CHECK(slate_direct_dispatch(3, &invalid_value) == ESP_ERR_INVALID_ARG,
          "action with invalid value type is refused");

    const esp_timer_create_args_t timer = {
        .callback = selftest_demo_action,
        .name = "direct_demo",
    };
    CHECK(esp_timer_create(&timer, &s_demo_timer) == ESP_OK, "demonstration dispatch armed");

    ESP_LOGI(TAG, "selftest: %d failure(s); fixture left bound for tools/direct", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
#undef CHECK
}

#endif
