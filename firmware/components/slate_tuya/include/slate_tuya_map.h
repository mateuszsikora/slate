/*
 * Tuya DP ↔ Slate snapshot mapping — the pure half of the slate_tuya provider.
 *
 * This module is deliberately free of ESP-IDF, FreeRTOS and networking: it
 * takes parsed cJSON and produces slate_state.h vocabulary, nothing else. That
 * is what lets the one piece of the provider where "a wrong number on a wall"
 * lives be exercised on the host, because Tuya has no trustworthy simulator
 * and the scaling rules below are exactly the kind of code that passes review
 * and fails on hardware.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "slate_state.h"   /* slate_snapshot_t, SLATE_KIND_*, capabilities */

#ifdef __cplusplus
extern "C" {
#endif

/* The resource-id suffix under which a dual temp/humidity sensor's second
 * reading is published: "<device id>/humidity". The provider mints it in its
 * discovery catalog, subscribe() splits it off again (base device id for
 * polling, the flag for snapshot selection), and slate_tuya_map_status()
 * selects the humidity DP when a resource ends in it. One spelling, exported
 * so none of those three places can drift from the others. */
#define SLATE_TUYA_HUMIDITY_SUFFIX "/humidity"

typedef enum {
    SLATE_TUYA_CLASS_UNSUPPORTED = 0,
    SLATE_TUYA_CLASS_LIGHT,
    SLATE_TUYA_CLASS_COVER,
    SLATE_TUYA_CLASS_SENSOR_TEMP_HUM, /* wsdcg-style: temperature+humidity */
    SLATE_TUYA_CLASS_SENSOR_POWER,    /* metered plug: power */
} slate_tuya_class_t;

/* Map a Tuya category string ("dj", "cl", "wsdcg", "cz"/"pc" plugs, ...) to a
 * class. Anything not in the mapping table is UNSUPPORTED — the store has no
 * `unknown` kind, so an unclassifiable device must never be published. */
slate_tuya_class_t slate_tuya_classify(const char *category);

/* Parsed per-device DP layout, filled from the functions-spec JSON "result"
 * array. A code that is absent gets its has_* flag false. The *_alias fields
 * record *which* spelling of a code this device declared (0 = the canonical
 * one, e.g. "switch_led"); they index the module's alias tables and matter
 * only because commands must name the same code the device reported —
 * reporting on "bright_value_v2" and commanding "bright_value" does nothing.
 *
 * temp_min/temp_max are the raw DP range of the colour-temperature code, NOT
 * kelvin: Tuya reports colour temperature on a unitless scale (0–1000 for
 * temp_value_v2) with no kelvin endpoints anywhere in the spec, so this module
 * maps that range onto the fixed 2700–6500 K window most white-tunable bulbs
 * actually cover. bright_min/bright_max are likewise the raw brightness DP
 * range. The *_scale fields are multipliers turning a raw integer into the SI
 * reading (°C, %, W); Tuya's "scale":n in the values JSON means the raw value
 * is scaled by 10^n, so the multiplier is 10^-n. */
typedef struct {
    slate_tuya_class_t cls;
    /* light: has_* is reportable status; can_* is a command function. */
    bool has_switch, has_bright, has_temp;
    bool can_switch, can_bright, can_temp;
    int bright_min, bright_max;
    int temp_min, temp_max;
    uint8_t switch_alias, bright_alias, temp_alias;
    uint8_t switch_status_alias, bright_status_alias, temp_status_alias;
    /* cover */  bool has_control, has_percent_control, has_percent_state;
    /* sensor temp/hum */ bool has_temperature, has_humidity; double temp_scale, hum_scale;
    /* sensor power */ bool has_power; double power_scale;
} slate_tuya_dps_t;

/* Parse the two arrays returned by Tuya's device specification endpoint.
 * Reportable readings are derived only from status_result; commands are
 * derived only from functions_result. This matters for read-only sensors,
 * whose DPs legitimately never appear in functions. */
esp_err_t slate_tuya_map_specification(const char *category, const cJSON *functions_result,
                                       const cJSON *status_result, slate_tuya_dps_t *out);

/* Compatibility helper for callers still using the legacy /functions
 * endpoint. It treats that array as both schemas; new discovery code must use
 * slate_tuya_map_specification(). */
esp_err_t slate_tuya_map_functions(const char *category, const cJSON *functions_result,
                                   slate_tuya_dps_t *out);

/* Build a normalized snapshot from a status "result" array. Everything except
 * out->available is written; `available` belongs to the caller, which alone
 * knows whether the device answered at all. `resource` and `name` are borrowed
 * — the store copies them at publish time.
 *
 * Returns ESP_ERR_NOT_FOUND when nothing publishable is present: a light whose
 * status carries no switch DP, a cover with neither control nor percent_state,
 * a sensor whose reading DP is absent. Optional readings (brightness, colour
 * temperature) degrade to SLATE_STATE_ABSENT instead of failing the snapshot.
 *
 * A temp/hum device reports temperature on its bare device resource. When the
 * functions spec has both DPs, a resource ending in "/humidity" selects the
 * humidity reading instead — the provider mints that second resource id for
 * the same physical device, because Slate binds exactly one reading per
 * resource. */
esp_err_t slate_tuya_map_status(const slate_tuya_dps_t *dps, const cJSON *status_result,
                                const char *resource, const char *name,
                                slate_snapshot_t *out);

/* Build the commands body for an action: {"commands":[{"code":...,"value":...}]}.
 * value_type/value mirror slate_action_request_t: has_bool/bool_value carry a
 * boolean action value, number_value the numeric one (percent for brightness
 * and position, kelvin for colour temperature). Returns ESP_ERR_NOT_SUPPORTED
 * for actions the DPs don't cover. Caller frees the returned cJSON.
 *
 * Tuya has no "toggle" command, so SLATE_ACTION_TOGGLE is honoured only when
 * has_bool supplies the already-resolved target state (the provider knows the
 * last published `on`); a bare toggle gets ESP_ERR_NOT_SUPPORTED. */
esp_err_t slate_tuya_map_command(const slate_tuya_dps_t *dps, slate_action_t action,
                                 bool has_bool, bool bool_value, int32_t number_value,
                                 cJSON **out_body);

#ifdef SLATE_TUYA_SELFTEST
/* Deterministic, network-free primitive fixtures used by the provider selftest. */
esp_err_t slate_tuya_map_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
