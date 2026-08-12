/*
 * Slate — provider-neutral state store and subscription contract.
 *
 * design.md ADR-3, §5.1 (the provider boundary), §5.2 (normalized resources
 * and state), §3.3 (bindings), §6.2 (memory), §6.4 (the rebuild), §7.5 (what a
 * component has to be able to render).
 *
 * This is the middle of the topology in §2 and the reason the rest of it can
 * stay ignorant of each other. A provider writes normalized snapshots in; the
 * UI runtime reads normalized values out; neither holds a pointer to the other.
 * ADR-3's promise — "a component asks to `toggle` a light; it never constructs
 * a Home Assistant `call_service` frame" — is only enforceable if there is one
 * place where the vocabulary is defined and nothing provider-specific is
 * expressible. This header is that place, and its whole test is that nothing in
 * it names a transport, a domain, an entity or a service.
 *
 * The store deliberately does NOT own:
 *
 *   actions          #18. Dispatch, optimistic state and the 3 s revert are the
 *                    action bus's; the capability bits below are what it
 *                    validates against, and they live here because they arrive
 *                    in the same snapshot as the state.
 *   the LVGL tree    #20. The store notifies; it never draws, and §6.1 keeps
 *                    every LVGL call on one task.
 *   transports       #74 and #16. A provider parses its own payloads and hands
 *                    over the result — §5.6 says the HA adapter expands its
 *                    compressed diffs before the store sees anything, which is
 *                    the same rule stated from the other side.
 *   discovery        §5.7's picker catalog is much larger than the active
 *                    configuration and lives in the adapter (§6.2). The store
 *                    holds only what the dashboard binds.
 *
 *
 * WHAT IS BOUND IS WHAT EXISTS
 *
 * §5.1 makes the active configuration the memory bound: "the runtime state
 * store holds only resources referenced by the active configuration". So the
 * binding set is not a filter applied to a general store — it *is* the store's
 * allocation, replaced whole by slate_state_bind() on every rebuild. A resource
 * nobody binds has nowhere to be written, which is what stops an unbounded LAN
 * client filling PSRAM (§5.4) without a rate limiter or a quota.
 *
 *
 * THREADING
 *
 * slate_state_init() runs once from app_main. Afterwards:
 *
 *   slate_state_bind()      one task only, the UI runtime's. It is the
 *                           configuration lifecycle of §6.4 and there is one of
 *                           those.
 *   everything else         any task. Providers publish from their own.
 *
 * Reads copy. A component holding a pointer into the table across a rebuild is
 * exactly the use-after-free §6.4 invites, so the API does not hand one out.
 *
 * Before slate_state_init(), and after it has failed, every function that can
 * report an error reports ESP_ERR_INVALID_STATE. A store that could not come up
 * is a dashboard that cannot hold values, which §6.5 has a mode for; it is not
 * a reason for the panel to stop answering the API.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Limits ------------------------------------------------------------- */

/*
 * Identity is refused rather than truncated, and presentation is truncated
 * rather than refused. Two different lengths for two different jobs: a resource
 * id cut to fit would alias two resources onto one entry and route somebody's
 * tap to the wrong light, while a name cut to fit is what §7.5 ellipsizes on
 * screen anyway.
 */
#define SLATE_PROVIDER_ID_MAX 15
#define SLATE_RESOURCE_ID_MAX 63
#define SLATE_RESOURCE_NAME_MAX 63
#define SLATE_RESOURCE_AREA_MAX 31
#define SLATE_SENSOR_TEXT_MAX 31
#define SLATE_SENSOR_UNIT_MAX 15

/*
 * §6.2 sizes the store from the configuration at roughly 256 B per bound
 * resource. 256 entries is therefore about 72 KB of PSRAM — comfortably more
 * than a 64 KB configuration (§3.1) can reference, which is the point: the cap
 * exists so a malformed or hostile binding list cannot ask for an allocation
 * the panel then dies of, not to constrain a dashboard anybody would write.
 */
#define SLATE_STATE_MAX_RESOURCES 256

/*
 * §5.1 names two provider ids for version 1, `direct` and `ha`. The table is
 * static and small rather than grown, because a provider registers once at
 * boot and never leaves; the spare slots are for the adapter after next and for
 * the self-test's fixtures, which must not have to displace a real one.
 */
#define SLATE_STATE_MAX_PROVIDERS 6

/** An optional numeric field the resource did not report (§5.2). */
#define SLATE_STATE_ABSENT INT16_MIN

/* --- Vocabulary (§5.2) --------------------------------------------------- */

/**
 * @brief The semantic kind of a resource — never the upstream domain.
 *
 * §5.2: "`kind` is semantic, not the native upstream domain." A provider maps
 * its own vocabulary onto these four before the store sees it, which is what
 * lets §7's components be written once. There is no `unknown` member on
 * purpose: a snapshot the adapter could not classify is one it must not
 * publish, and a binding whose type the parser did not recognise renders §3.1's
 * placeholder tile without ever reaching the store.
 */
typedef enum {
    SLATE_KIND_LIGHT = 0,
    SLATE_KIND_COVER,
    SLATE_KIND_SENSOR,
    SLATE_KIND_SCENE,
} slate_kind_t;

/** @brief `light`, `cover`, `sensor`, `scene` — the §3.3 configuration spelling. */
const char *slate_kind_str(slate_kind_t kind);

/** @brief Parse the same four names. False leaves `*out` untouched. */
bool slate_kind_from_str(const char *name, slate_kind_t *out);

/**
 * @brief Provider lifecycle status (§4.1, §5.2).
 *
 * The distinction that carries weight is `degraded` against `offline`, and it
 * is a rule about somebody else's tiles: `offline` marks that provider's
 * resources stale, `degraded` does not. §5.2 states the case it exists for —
 * "a read-only direct sensor remains fresh even when no action consumer is
 * attached" — and §5.4 is where the direct provider takes it.
 */
typedef enum {
    SLATE_PROVIDER_UNCONFIGURED = 0, /**< needs credentials; `ha` before `POST /ha` */
    SLATE_PROVIDER_CONNECTING,       /**< attempting; last known values go stale */
    SLATE_PROVIDER_ONLINE,           /**< serving its whole contract */
    SLATE_PROVIDER_DEGRADED,         /**< serving part of it; values stay fresh */
    SLATE_PROVIDER_OFFLINE,          /**< transient loss; values go stale */
    SLATE_PROVIDER_ERROR,            /**< needs a person; values go stale */
} slate_provider_status_t;

/** @brief §4.1's spelling of the status, for `GET /status` and `GET /providers`. */
const char *slate_provider_status_str(slate_provider_status_t status);

/**
 * @brief Semantic actions (§5.3), named here because capabilities are declared here.
 *
 * §5.3: "Transport names are not semantic action names." The enum is the whole
 * list a component may emit and the whole list a provider may advertise; #18
 * routes them and each adapter maps them onto its own system. `set_position`
 * is the cover equivalent of `set_brightness` and is advertised the same way,
 * through a range.
 */
typedef enum {
    SLATE_ACTION_TOGGLE = 0,
    SLATE_ACTION_SET_POWER,
    SLATE_ACTION_SET_BRIGHTNESS,
    SLATE_ACTION_SET_COLOR_TEMPERATURE,
    SLATE_ACTION_OPEN,
    SLATE_ACTION_STOP,
    SLATE_ACTION_CLOSE,
    SLATE_ACTION_SET_POSITION,
    SLATE_ACTION_ACTIVATE,
    SLATE_ACTION_COUNT,
} slate_action_t;

/** @brief §5.3's spelling of an action. */
const char *slate_action_str(slate_action_t action);

/** @brief Parse the same names. False leaves `*out` untouched. */
bool slate_action_from_str(const char *name, slate_action_t *out);

/**
 * @brief What a resource says it can be asked to do (§5.2).
 *
 * "Absent capabilities mean read-only", so a zeroed struct is the correct
 * description of a resource that only reports. The ranges are meaningful only
 * while their bit is set, and they are here rather than inside the state union
 * because §7.1 hides a control the resource does not advertise — a slider is
 * drawn from the capability, not from the current value.
 *
 * An advertised percentage with a zeroed range means the whole range: §5.2
 * spells one capability as `true` and the next as `{"min":0,"max":100}`, so a
 * bit without limits is the ordinary encoding and the store fills 0..100 in
 * rather than leaving each component to invent the same fallback. A range that
 * is inverted or outside that scale is malformed and the snapshot carrying it
 * is refused. Colour temperature keeps a zeroed range as "unstated", having no
 * natural scale to substitute.
 */
typedef struct {
    uint16_t actions; /**< bitmask of `1u << slate_action_t` */
    int16_t brightness_min;
    int16_t brightness_max;
    int16_t color_temperature_min;
    int16_t color_temperature_max;
    int16_t position_min;
    int16_t position_max;
} slate_capabilities_t;

/** @brief Whether `caps` advertises `action`. */
bool slate_capabilities_have(const slate_capabilities_t *caps, slate_action_t action);

/** @brief `state.power` plus the two optional fields of §5.2. */
typedef struct {
    bool on;
    int16_t brightness;         /**< per cent, or SLATE_STATE_ABSENT */
    int16_t color_temperature;  /**< kelvin, or SLATE_STATE_ABSENT */
} slate_light_state_t;

/** @brief §7.2's animated indicator reads this, not a timer. */
typedef enum {
    SLATE_COVER_IDLE = 0,
    SLATE_COVER_OPENING,
    SLATE_COVER_CLOSING,
} slate_cover_motion_t;

/** @brief Cover position and movement (§5.2). */
typedef struct {
    int16_t position; /**< per cent open, or SLATE_STATE_ABSENT */
    slate_cover_motion_t motion;
} slate_cover_state_t;

/**
 * @brief What a sensor is measuring (§5.2), which selects icon and formatting.
 *
 * §7.3: "The normalized `measurement` selects the icon and value formatting; HA
 * `device_class` is one input the HA provider maps onto it." One input, not the
 * definition — a direct-provider script has no device classes and must be able
 * to say `temperature` just as plainly.
 */
typedef enum {
    SLATE_MEASUREMENT_NONE = 0,
    SLATE_MEASUREMENT_TEMPERATURE,
    SLATE_MEASUREMENT_HUMIDITY,
    SLATE_MEASUREMENT_PRESSURE,
    SLATE_MEASUREMENT_POWER,
} slate_measurement_t;

/** @brief §5.2's spelling of a measurement; `NULL` for SLATE_MEASUREMENT_NONE. */
const char *slate_measurement_str(slate_measurement_t measurement);

/** @brief Parse the same names. False leaves `*out` untouched. */
bool slate_measurement_from_str(const char *name, slate_measurement_t *out);

/**
 * @brief "numeric or textual `value`, optional `unit`" (§5.2).
 *
 * Both members exist because a sensor is the one kind whose value may not be a
 * number, and §7.5's type scale has to step down for `1013.25` either way. The
 * number is a double rather than a float so that a pressure reading formats to
 * the digits it arrived with instead of to the digits a single-precision round
 * trip left behind.
 */
typedef struct {
    bool numeric;
    double value;
    char text[SLATE_SENSOR_TEXT_MAX + 1];
    char unit[SLATE_SENSOR_UNIT_MAX + 1];
    slate_measurement_t measurement;
} slate_sensor_state_t;

/** @brief The kind-specific half of §5.2. Scenes are stateless and have no member. */
typedef union {
    slate_light_state_t light;
    slate_cover_state_t cover;
    slate_sensor_state_t sensor;
} slate_state_value_t;

/* --- What a provider publishes ------------------------------------------ */

/**
 * @brief One complete normalized snapshot (§5.2).
 *
 * "A complete snapshot replaces the previous one." There is no partial variant
 * on purpose: §5.2 permits diffs "between an adapter and the store" as an
 * internal matter of the adapter, and the moment the store accepted one it
 * would owe every component a rule about which fields a missing field means to
 * keep. The HA adapter expands its compressed diffs against its own last known
 * entity (§5.6) and publishes the result.
 *
 * `provider` is not a member: it is an argument to slate_state_publish(),
 * because §5.4's endpoint fixes it to `direct` rather than letting the body
 * choose. A body that could name its own provider is a body that can write into
 * the Home Assistant half of the store.
 *
 * `name` and `area` are optional (§5.2) and NULL means the resource does not
 * supply one — §7's components fall back to the tile's `label` or to the
 * resource id. They are copied, so the caller's buffers are free immediately.
 */
typedef struct {
    const char *resource;
    slate_kind_t kind;
    const char *name;
    const char *area;
    bool available;
    slate_capabilities_t capabilities;
    slate_state_value_t state;
} slate_snapshot_t;

/* --- What a component reads --------------------------------------------- */

/**
 * @brief The six cases §7.5 makes every component render, decided once.
 *
 * These are computed by the store rather than derived by each component,
 * because they depend on things a component cannot see: whether the provider
 * behind the binding is registered at all, and what its lifecycle status is.
 * §7.5's list is a requirement on all four components at launch and on every
 * component after them, and four independent derivations of it would disagree
 * within a milestone.
 *
 * `MISSING` and `UNAVAILABLE` are §7.5's "resource missing" and "resource
 * unavailable", which are different tiles: one shows `provider:resource` so it
 * can be found in the editor, the other keeps the last known value dimmed with
 * a dash. `STALE` is the same dimmed treatment for a provider-level cause
 * (§6.5's offline mode, per provider), and it wins over `UNAVAILABLE` because a
 * provider that is offline is not in a position to say whether its resource
 * still is. `MISSING_PROVIDER` is §3.3's forward-compatibility case: a
 * configuration written on newer firmware naming a provider this build does not
 * have is accepted and shown, never rejected. `INCOMPATIBLE` is the other half
 * of that paragraph — a `light` tile whose resource arrives with kind `sensor`.
 */
typedef enum {
    SLATE_PRESENT_OK = 0,
    SLATE_PRESENT_STALE,
    SLATE_PRESENT_UNAVAILABLE,
    SLATE_PRESENT_MISSING,
    SLATE_PRESENT_MISSING_PROVIDER,
    SLATE_PRESENT_INCOMPATIBLE,
} slate_presentation_t;

/** @brief A stable lowercase name per presentation, for logs and diagnostics. */
const char *slate_presentation_str(slate_presentation_t presentation);

/**
 * @brief The current value of one bound resource — a copy, not a view.
 *
 * `kind` is the binding's, not the last snapshot's: a component built for a
 * light stays a light when a provider sends it a sensor, and says so through
 * `presentation`. `mismatch_kind` records the provider's most recently
 * rejected kind until a valid snapshot arrives; consumers use it while the
 * presentation is INCOMPATIBLE to explain both halves of the mismatch instead
 * of showing an unexplained warning. `state` and `capabilities` are meaningful
 * only when `presentation` is OK, STALE or UNAVAILABLE, which is exactly when
 * `updated_us` is non-zero.
 */
typedef struct {
    char provider[SLATE_PROVIDER_ID_MAX + 1];
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    slate_kind_t kind;
    slate_kind_t mismatch_kind;
    slate_presentation_t presentation;
    char name[SLATE_RESOURCE_NAME_MAX + 1];
    char area[SLATE_RESOURCE_AREA_MAX + 1];
    slate_capabilities_t capabilities;
    slate_state_value_t state;
    int64_t updated_us; /**< esp_timer time of the last snapshot; 0 = never */
} slate_resource_t;

/* --- Lifecycle ----------------------------------------------------------- */

/**
 * @brief Prepare the store. Call once from app_main, before any provider.
 *
 * Allocates nothing but its lock: the table is sized by the first
 * slate_state_bind(), since §5.1 makes the active configuration the only thing
 * entitled to decide how much memory this component uses. Repeated calls are
 * safe and return ESP_OK.
 */
esp_err_t slate_state_init(void);

/* --- Providers (§5.1) ---------------------------------------------------- */

/**
 * @brief Hand a provider the set of resource ids the configuration references.
 *
 * §5.1's second core operation. The array and the strings it points at are
 * borrowed for the duration of the call and belong to the store; a provider
 * that needs them afterwards copies them. `count == 0` is a real instruction
 * and means unsubscribe from everything — §5.1: "Removing the last binding
 * removes the state on the same rebuild." `resources` is NULL in that case, so
 * a provider must read the count before the pointer.
 *
 * Called on the binding task, outside the store's lock, so an adapter may call
 * back into the store. A provider that cannot subscribe should report its own
 * status rather than returning an error the rebuild can do nothing with; the
 * return value is logged and does not fail the rebuild, because a tree that is
 * already on screen cannot be un-activated because Home Assistant was busy.
 */
typedef esp_err_t (*slate_state_subscribe_fn)(void *ctx, const char *const *resources,
                                              size_t count);

/**
 * @brief A provider's registration.
 *
 * `id` must be a string literal or otherwise outlive the firmware — it is the
 * id §3.3 stores in configurations and §4.1 prints in `GET /providers`, and
 * there are two of them in version 1. `subscribe` may be NULL for a provider
 * that has nothing to do with a subscription set.
 */
typedef struct {
    const char *id;
    slate_state_subscribe_fn subscribe;
    void *ctx;
} slate_state_provider_t;

/**
 * @brief Register a provider. `direct` at boot, `ha` when it is configured.
 *
 * Registration is what makes bindings for that id resolvable; before it, and
 * permanently for an id this firmware does not implement, those bindings
 * present as SLATE_PRESENT_MISSING_PROVIDER instead of being rejected (§3.3).
 * The initial status is SLATE_PROVIDER_UNCONFIGURED, which is the honest state
 * of something that has registered and not yet connected.
 *
 * ESP_ERR_INVALID_STATE if the id is already registered, ESP_ERR_NO_MEM if the
 * table is full, ESP_ERR_INVALID_ARG for a NULL or over-long id.
 */
esp_err_t slate_state_provider_register(const slate_state_provider_t *provider);

/**
 * @brief Report a provider's lifecycle status (§5.1's first core operation).
 *
 * A change that crosses the serving boundary re-presents that provider's
 * resources — and only that provider's, because §5.2 is explicit that "an
 * unavailable HA instance must not dim tiles supplied by `direct`". Observers
 * are woken for the affected entries exactly as a snapshot would.
 */
esp_err_t slate_state_provider_set_status(const char *id, slate_provider_status_t status);

/** @brief Current status, or SLATE_PROVIDER_UNCONFIGURED for an unregistered id. */
slate_provider_status_t slate_state_provider_status(const char *id);

/** @brief §4.1's `/providers` entry, for whoever serialises it. */
typedef struct {
    char id[SLATE_PROVIDER_ID_MAX + 1];
    slate_provider_status_t status;
    size_t resource_count;
} slate_state_provider_info_t;

/** @brief How many providers are registered. */
size_t slate_state_provider_count(void);

/**
 * @brief Copy one registration by index. ESP_ERR_NOT_FOUND past the end.
 *
 * `resource_count` is how many bound resources the store holds for that
 * provider, which is deliberately not §5.7's discovery catalog — that is larger,
 * lives in the adapter and answers a different question (§6.2).
 */
esp_err_t slate_state_provider_at(size_t index, slate_state_provider_info_t *out);

/* --- Bindings (§3.3, §5.1, §6.4) ----------------------------------------- */

/**
 * @brief One tile's binding: the pair, plus the kind its component expects.
 *
 * §3.3: "The pair is stored and compared as two strings; firmware never infers
 * a provider from punctuation or from a component type." `kind` is the third
 * value and comes from the component type rather than from the resource, which
 * is what lets the store name an incompatible binding before any snapshot has
 * arrived to disagree with it.
 */
typedef struct {
    const char *provider;
    const char *resource;
    slate_kind_t kind;
} slate_binding_t;

/**
 * @brief Replace the whole subscription set — §6.4's atomic activation.
 *
 * The one sizing decision this component makes, and it is made from the caller's
 * list rather than from a maximum. Duplicates are collapsed: several tiles may
 * bind one resource, and the store holds one entry per pair. The last known
 * value of a binding that survives the rebuild is carried across, so a dashboard
 * that gains a tile does not blank the eleven that did not change; a binding
 * whose kind changed does not carry its value, because that value described a
 * different component's contract.
 *
 * Each registered provider is then handed its own subset (see
 * slate_state_subscribe_fn), which is the "atomically activate tree + provider
 * subscriptions" half of §6.4.
 *
 * `bindings` may be NULL when `count` is 0, which releases the table entirely.
 * ESP_ERR_INVALID_SIZE beyond SLATE_STATE_MAX_RESOURCES, ESP_ERR_INVALID_ARG for
 * an empty or over-long id — identity is refused, never truncated — and
 * ESP_ERR_NO_MEM if the table cannot be allocated. ESP_ERR_INVALID_STATE is the
 * one contradiction the store cannot hold: two bindings naming the same pair
 * with different kinds, where one entry cannot describe both and at most one of
 * the tiles can be right. #19 reports it against the tile.
 *
 * On every failure the previous set remains active and the caller must not
 * activate its tree.
 *
 * Call from one task only (§6.4's configuration lifecycle).
 */
esp_err_t slate_state_bind(const slate_binding_t *bindings, size_t count);

/** @brief Distinct normalized resources held — §4.1's top-level `resource_count`. */
size_t slate_state_count(void);

/* --- Publication (§5.2, §5.4) -------------------------------------------- */

/**
 * @brief Deliver a complete normalized snapshot into the store.
 *
 * §5.1's third core operation, and the three refusals are §5.4's endpoint
 * contract stated where it can be enforced for every provider rather than for
 * one route:
 *
 *   ESP_ERR_NOT_FOUND       the pair is not in the active binding set —
 *                           `404 resource_not_bound`, and §5.4's bound on how
 *                           much a LAN client can allocate.
 *   ESP_ERR_INVALID_STATE   the snapshot's kind is not the binding's —
 *                           `409 kind_mismatch`. The entry remembers it, and a
 *                           binding with no confirmed value to show instead
 *                           presents SLATE_PRESENT_INCOMPATIBLE: that is how
 *                           §3.3's "incompatible-binding placeholder" reaches
 *                           the screen for a mismatch that arrived at runtime
 *                           rather than in the configuration.
 *   ESP_ERR_INVALID_ARG     malformed common or kind-specific state, or a
 *                           capability range that cannot be drawn —
 *                           `400 invalid_state`.
 *
 * "None disturbs the last confirmed value" (§5.4), the mismatch included: a
 * light that is on stays on screen as on while somebody's script publishes a
 * sensor over it.
 *
 * Safe from any task. On success the entry is marked changed and the wake
 * callback runs — on the publishing task, so it must be cheap (§6.1).
 */
esp_err_t slate_state_publish(const char *provider, const slate_snapshot_t *snapshot);

/**
 * @brief Observe a successful provider publication after it enters the store.
 *
 * The callback runs synchronously on the publishing task, after the store lock
 * has been released. It therefore must be cheap and may call back into the
 * store. `provider` and `snapshot` are borrowed for the duration of the call.
 *
 * This is deliberately separate from the UI wake/drain path below. A consumer
 * that has to distinguish a real provider snapshot from a coalesced redraw —
 * §5.3's action confirmation is the first one — observes the publication here;
 * components still receive complete current resources on the LVGL task.
 */
typedef void (*slate_state_publish_observer_fn)(void *ctx, const char *provider,
                                                const slate_snapshot_t *snapshot);

/** @brief Install the sole publication observer. NULL removes it. */
void slate_state_set_publish_observer(slate_state_publish_observer_fn observer, void *ctx);

/* --- Reading (§7) --------------------------------------------------------- */

/**
 * @brief Copy the current value of one binding. ESP_ERR_NOT_FOUND if unbound.
 *
 * Always a complete current value, never a diff: §5.2 requires the store to
 * expose one whatever the adapter handed it.
 */
esp_err_t slate_state_get(const char *provider, const char *resource, slate_resource_t *out);

/** @brief Copy the entry at `index`. ESP_ERR_NOT_FOUND past the end. */
esp_err_t slate_state_at(size_t index, slate_resource_t *out);

/* --- Observation (§6.1) --------------------------------------------------- */

/**
 * @brief Told that something changed, on whatever task changed it.
 *
 * Not the change itself. §6.1 puts every LVGL call on one task and has
 * providers "post normalized work to it through a queue", and this is the post:
 * #20 supplies a wake that is a slate_display_post(), and the UI task then
 * calls slate_state_drain() on the other side of that queue. A wake must not
 * block, must not call back into the store, and may be called from an
 * interrupt-adjacent context such as a WebSocket receive task.
 */
typedef void (*slate_state_wake_fn)(void *ctx);

/** @brief Install the wake. NULL removes it. Changes are still recorded without one. */
void slate_state_set_wake(slate_state_wake_fn wake, void *ctx);

/** @brief Receives one changed resource, on the draining task. */
typedef void (*slate_state_visit_fn)(const slate_resource_t *resource, void *ctx);

/**
 * @brief Deliver every resource that changed since the last drain, and return how many.
 *
 * The store coalesces rather than queues, and that is a decision worth stating:
 * S-2 measured ten updates per second against a saturated page, and a queue of
 * events would spend a frame delivering values that a later event had already
 * superseded. What a component needs is the current value (§5.2), so the drain
 * carries the current value and each resource appears at most once.
 *
 * `fn` runs outside the store's lock, so it may read the store and publish into
 * it. It must not call slate_state_bind(): the drain walks the table by index,
 * and a rebuild underneath it would renumber the entries it has not reached —
 * some delivered twice, some not at all. A rebuild is what the drain feeds
 * anyway, so it belongs after the loop rather than inside it.
 *
 * A change arriving during the drain is picked up by the next one, with its own
 * wake.
 */
size_t slate_state_drain(slate_state_visit_fn fn, void *ctx);

#ifdef SLATE_STATE_SELFTEST

/**
 * @brief Exercise the store's contract without a provider, a network or a screen.
 *
 * Development verifier for #17, called by main only when built with
 * `-DSLATE_STATE_SELFTEST=1`. The end-to-end sentence in that issue's done-when
 * needs the publishing endpoint of #74 and the tree of #20; this covers the
 * half that is this component's — replacement, refusal, staleness, rebuild
 * carry-over and allocation idempotency — and logs PASS/FAIL per case.
 * ESP_FAIL if any case failed.
 */
esp_err_t slate_state_selftest(void);

#endif

#ifdef __cplusplus
}
#endif
