/*
 * Tuya DP ↔ Slate snapshot mapping.
 *
 * WHY CODES, NOT DP NUMBERS
 *
 * A Tuya device exposes numbered data points (DP 1, DP 20, …), and the numbers
 * are reassigned per model — occasionally per firmware revision of the same
 * model. The only stable identity a DP has is the string `code` in the
 * device's functions spec (`GET /v1.0/devices/{id}/functions`), which is why
 * slate_tuya_map_functions() learns each device's spelling of the codes it
 * cares about and everything afterwards keys on those strings. A hardcoded
 * "DP 20 is brightness" table would silently command the wrong datapoint on
 * the next model; a missed code merely leaves a capability unadvertised, which
 * §7.1 renders as a missing slider rather than a wrong action.
 *
 * The same caution applies in the other direction: alias tables
 * (switch_led/switch/switch_led_1, bright_value_v2/bright_value, …) exist
 * because Tuya renamed its own codes between protocol generations and both
 * spellings ship in the wild. The alias index recorded at functions-parse time
 * is what lets a command name the exact code the device declared.
 *
 * SCALING CONVENTIONS
 *
 * Brightness. Tuya's v2 range is 10–1000, where 10 is a hardware floor — the
 * dimmest the driver goes — not the 0 % point. Slate's 0 % means "as dim as it
 * goes", so raw values at or below the floor report 0 % and the rest scale
 * proportionally against the maximum (raw 500 of 10–1000 is 50 %); a commanded
 * 0 % maps back onto the floor. A linear min→max mapping was rejected: it
 * would make the floor report as 0 % but misplace everything above it.
 *
 * Colour temperature. Tuya reports it on a unitless raw scale (0–1000 for
 * temp_value_v2) and the functions spec never states kelvin endpoints, so the
 * raw range is mapped onto a fixed 2700–6500 K window — the range virtually
 * every white-tunable bulb covers. This is a convention, not a measurement;
 * the alternative (publishing raw numbers) would put a meaningless 0–1000
 * slider on screen, which is worse than an approximate kelvin.
 *
 * Sensors. `va_temperature`/`va_humidity`/`cur_power` report integers scaled
 * by 10^scale, with `scale` declared in the DP's values JSON and defaulting to
 * 1 (×10) when the device doesn't say — the common case for wsdcg sensors and
 * deciwatt plugs.
 */

#include "slate_tuya_map.h"

#include <math.h>
#include <string.h>

/* Warm/cool endpoints of the fixed kelvin window; see the header comment. */
#define TUYA_KELVIN_MIN 2700
#define TUYA_KELVIN_MAX 6500

/* Canonical spelling first: the alias index recorded in slate_tuya_dps_t is
 * an index into these tables. */
static const char *const k_switch_codes[] = {"switch_led", "switch", "switch_led_1"};
static const char *const k_bright_codes[] = {"bright_value_v2", "bright_value"};
static const char *const k_temp_codes[] = {"temp_value_v2", "temp_value"};

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

static bool category_in(const char *category, const char *const *list, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(category, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

slate_tuya_class_t slate_tuya_classify(const char *category)
{
    if (category == NULL) {
        return SLATE_TUYA_CLASS_UNSUPPORTED;
    }
    static const char *const light[] = {"dj", "dd", "fwd", "gyd", "xdd"};
    static const char *const cover[] = {"cl", "clkg"};
    static const char *const temp_hum[] = {"wsdcg", "zndb"};
    static const char *const plug[] = {"cz", "pc"};
    if (category_in(category, light, COUNT_OF(light))) {
        return SLATE_TUYA_CLASS_LIGHT;
    }
    if (category_in(category, cover, COUNT_OF(cover))) {
        return SLATE_TUYA_CLASS_COVER;
    }
    if (category_in(category, temp_hum, COUNT_OF(temp_hum))) {
        return SLATE_TUYA_CLASS_SENSOR_TEMP_HUM;
    }
    if (category_in(category, plug, COUNT_OF(plug))) {
        return SLATE_TUYA_CLASS_SENSOR_POWER;
    }
    return SLATE_TUYA_CLASS_UNSUPPORTED;
}

/* The first entry of `codes` the device declares, or NULL; *alias records
 * which spelling matched so commands can echo it back. */
static const cJSON *find_function(const cJSON *functions, const char *const *codes,
                                  size_t count, uint8_t *alias)
{
    const cJSON *fn = NULL;
    cJSON_ArrayForEach(fn, functions) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(fn, "code");
        if (!cJSON_IsString(code)) {
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            if (strcmp(code->valuestring, codes[i]) == 0) {
                *alias = (uint8_t) i;
                return fn;
            }
        }
    }
    return NULL;
}

static const cJSON *find_status_value(const cJSON *status, const char *code)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, status) {
        const cJSON *item_code = cJSON_GetObjectItemCaseSensitive(item, "code");
        if (cJSON_IsString(item_code) && strcmp(item_code->valuestring, code) == 0) {
            return cJSON_GetObjectItemCaseSensitive(item, "value");
        }
    }
    return NULL;
}

/* The values field of a functions entry is a *string* holding a JSON object
 * ("{\"min\":10,\"max\":1000,...}"), frequently empty; parse it or return NULL. */
static cJSON *parse_values(const cJSON *fn)
{
    const cJSON *values = cJSON_GetObjectItemCaseSensitive(fn, "values");
    if (!cJSON_IsString(values) || values->valuestring[0] == '\0') {
        return NULL;
    }
    return cJSON_Parse(values->valuestring);
}

static bool values_int(const cJSON *values, const char *key, int *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(values, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = item->valueint;
    return true;
}

/* Replace the range with the declared one when the spec states a sane one. */
static void apply_range(const cJSON *fn, int *min, int *max)
{
    cJSON *values = parse_values(fn);
    if (values == NULL) {
        return;
    }
    int declared_min, declared_max;
    bool ok = values_int(values, "min", &declared_min) &&
              values_int(values, "max", &declared_max) && declared_min < declared_max;
    cJSON_Delete(values);
    if (ok) {
        *min = declared_min;
        *max = declared_max;
    }
}

/* 10^-scale as the raw→SI multiplier; repeated division keeps small integer
 * scales exact instead of trusting pow() at the last digit. */
static double dp_scale(const cJSON *fn, double fallback)
{
    cJSON *values = parse_values(fn);
    if (values == NULL) {
        return fallback;
    }
    int scale = 0;
    bool ok = values_int(values, "scale", &scale) && scale >= 0 && scale <= 6;
    cJSON_Delete(values);
    if (!ok) {
        return fallback;
    }
    double multiplier = 1.0;
    for (int i = 0; i < scale; i++) {
        multiplier /= 10.0;
    }
    return multiplier;
}

esp_err_t slate_tuya_map_specification(const char *category, const cJSON *functions_result,
                                       const cJSON *status_result, slate_tuya_dps_t *out)
{
    if (category == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (slate_tuya_dps_t) {0};
    out->cls = slate_tuya_classify(category);
    if (out->cls == SLATE_TUYA_CLASS_UNSUPPORTED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!cJSON_IsArray(functions_result) || !cJSON_IsArray(status_result)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t alias = 0;
    switch (out->cls) {
    case SLATE_TUYA_CLASS_LIGHT: {
        const cJSON *status = find_function(status_result, k_switch_codes,
                                            COUNT_OF(k_switch_codes), &alias);
        if (status != NULL) {
            out->has_switch = true;
            out->switch_status_alias = alias;
        }
        const cJSON *fn = find_function(functions_result, k_switch_codes,
                                        COUNT_OF(k_switch_codes), &alias);
        if (fn != NULL) {
            out->can_switch = true;
            out->switch_alias = alias;
        }
        status = find_function(status_result, k_bright_codes, COUNT_OF(k_bright_codes),
                               &alias);
        if (status != NULL) {
            out->has_bright = true;
            out->bright_status_alias = alias;
            out->bright_min = alias == 0 ? 10 : 25;
            out->bright_max = alias == 0 ? 1000 : 255;
            apply_range(status, &out->bright_min, &out->bright_max);
        }
        fn = find_function(functions_result, k_bright_codes, COUNT_OF(k_bright_codes),
                           &alias);
        if (fn != NULL) {
            out->can_bright = true;
            out->bright_alias = alias;
            if (!out->has_bright) {
                out->bright_min = alias == 0 ? 10 : 25;
                out->bright_max = alias == 0 ? 1000 : 255;
            }
            apply_range(fn, &out->bright_min, &out->bright_max);
        }
        status = find_function(status_result, k_temp_codes, COUNT_OF(k_temp_codes),
                               &alias);
        if (status != NULL) {
            out->has_temp = true;
            out->temp_status_alias = alias;
            out->temp_min = 0;
            out->temp_max = alias == 0 ? 1000 : 255;
            apply_range(status, &out->temp_min, &out->temp_max);
        }
        fn = find_function(functions_result, k_temp_codes, COUNT_OF(k_temp_codes),
                           &alias);
        if (fn != NULL) {
            out->can_temp = true;
            out->temp_alias = alias;
            if (!out->has_temp) {
                out->temp_min = 0;
                out->temp_max = alias == 0 ? 1000 : 255;
            }
            apply_range(fn, &out->temp_min, &out->temp_max);
        }
        break;
    }
    case SLATE_TUYA_CLASS_COVER: {
        static const char *const control[] = {"control"};
        static const char *const percent_control[] = {"percent_control"};
        static const char *const percent_state[] = {"percent_state"};
        out->has_control =
            find_function(functions_result, control, 1, &alias) != NULL;
        out->has_percent_control =
            find_function(functions_result, percent_control, 1, &alias) != NULL;
        out->has_percent_state = find_function(status_result, percent_state, 1, &alias) != NULL;
        break;
    }
    case SLATE_TUYA_CLASS_SENSOR_TEMP_HUM: {
        static const char *const temperature[] = {"va_temperature"};
        static const char *const humidity[] = {"va_humidity"};
        const cJSON *schema = find_function(status_result, temperature, 1, &alias);
        if (schema != NULL) {
            out->has_temperature = true;
            out->temp_scale = dp_scale(schema, 0.1);
        }
        schema = find_function(status_result, humidity, 1, &alias);
        if (schema != NULL) {
            out->has_humidity = true;
            out->hum_scale = dp_scale(schema, 0.1);
        }
        break;
    }
    case SLATE_TUYA_CLASS_SENSOR_POWER: {
        static const char *const power[] = {"cur_power"};
        const cJSON *schema = find_function(status_result, power, 1, &alias);
        if (schema == NULL) {
            /* A cz/pc plug with no power DP is a plain switch — not one of
             * Slate's four kinds, so it must never reach the store. */
            out->cls = SLATE_TUYA_CLASS_UNSUPPORTED;
            return ESP_ERR_NOT_SUPPORTED;
        }
        out->has_power = true;
        out->power_scale = dp_scale(schema, 0.1);
        break;
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

esp_err_t slate_tuya_map_functions(const char *category, const cJSON *functions_result,
                                   slate_tuya_dps_t *out)
{
    return slate_tuya_map_specification(category, functions_result, functions_result, out);
}

/* Brightness scaling: floor rule and max-proportional map, per the header. */
static int16_t bright_to_percent(const slate_tuya_dps_t *dps, double raw)
{
    if (raw <= dps->bright_min || dps->bright_max <= 0) {
        return 0;
    }
    long percent = lround(raw * 100.0 / dps->bright_max);
    if (percent > 100) {
        percent = 100;
    }
    return (int16_t) percent;
}

static int percent_to_bright(const slate_tuya_dps_t *dps, int32_t percent)
{
    if (percent <= 0) {
        return dps->bright_min;
    }
    if (percent > 100) {
        percent = 100;
    }
    long raw = lround(percent * (double) dps->bright_max / 100.0);
    if (raw < dps->bright_min) {
        raw = dps->bright_min;
    }
    if (raw > dps->bright_max) {
        raw = dps->bright_max;
    }
    return (int) raw;
}

static int16_t temp_to_kelvin(const slate_tuya_dps_t *dps, double raw)
{
    double span = (double) dps->temp_max - (double) dps->temp_min;
    if (span <= 0) {
        return SLATE_STATE_ABSENT;
    }
    if (raw < dps->temp_min) {
        raw = dps->temp_min;
    }
    if (raw > dps->temp_max) {
        raw = dps->temp_max;
    }
    return (int16_t) lround(TUYA_KELVIN_MIN +
                            (raw - dps->temp_min) * (TUYA_KELVIN_MAX - TUYA_KELVIN_MIN) /
                                span);
}

static int kelvin_to_temp(const slate_tuya_dps_t *dps, int32_t kelvin)
{
    double span = (double) dps->temp_max - (double) dps->temp_min;
    if (span <= 0) {
        return dps->temp_min;
    }
    if (kelvin < TUYA_KELVIN_MIN) {
        kelvin = TUYA_KELVIN_MIN;
    }
    if (kelvin > TUYA_KELVIN_MAX) {
        kelvin = TUYA_KELVIN_MAX;
    }
    return (int) lround(dps->temp_min +
                        (kelvin - TUYA_KELVIN_MIN) * span /
                            (TUYA_KELVIN_MAX - TUYA_KELVIN_MIN));
}

/* Slate binds one reading per resource; a dual temp/hum device gets a second,
 * synthetic resource id for its humidity half. See the header comment. */
static bool selects_humidity(const char *resource)
{
    size_t len = strlen(resource);
    size_t suffix_len = strlen(SLATE_TUYA_HUMIDITY_SUFFIX);
    return len >= suffix_len &&
           strcmp(resource + len - suffix_len, SLATE_TUYA_HUMIDITY_SUFFIX) == 0;
}

static esp_err_t map_light_status(const slate_tuya_dps_t *dps, const cJSON *status,
                                  slate_snapshot_t *out)
{
    out->kind = SLATE_KIND_LIGHT;
    out->state.light = (slate_light_state_t) {
        .on = false,
        .brightness = SLATE_STATE_ABSENT,
        .color_temperature = SLATE_STATE_ABSENT,
    };

    /* The switch reading is the one mandatory part: publishing `on = false`
     * for a light that simply didn't answer would put a wrong state on the
     * wall, so its absence fails the whole snapshot. */
    if (!dps->has_switch) {
        return ESP_ERR_NOT_FOUND;
    }
    const cJSON *value = find_status_value(status, k_switch_codes[dps->switch_status_alias]);
    if (!cJSON_IsBool(value)) {
        return ESP_ERR_NOT_FOUND;
    }
    out->state.light.on = cJSON_IsTrue(value);

    if (dps->can_switch) {
        out->capabilities.actions =
            (uint16_t) ((1u << SLATE_ACTION_TOGGLE) | (1u << SLATE_ACTION_SET_POWER));
    }
    if (dps->has_bright) {
        if (dps->can_bright) {
            out->capabilities.actions |= (uint16_t) (1u << SLATE_ACTION_SET_BRIGHTNESS);
        }
        out->capabilities.brightness_min = 0;
        out->capabilities.brightness_max = 100;
        value = find_status_value(status, k_bright_codes[dps->bright_status_alias]);
        if (cJSON_IsNumber(value)) {
            out->state.light.brightness = bright_to_percent(dps, value->valuedouble);
        }
    }
    if (dps->has_temp) {
        if (dps->can_temp) {
            out->capabilities.actions |=
                (uint16_t) (1u << SLATE_ACTION_SET_COLOR_TEMPERATURE);
        }
        out->capabilities.color_temperature_min = TUYA_KELVIN_MIN;
        out->capabilities.color_temperature_max = TUYA_KELVIN_MAX;
        value = find_status_value(status, k_temp_codes[dps->temp_status_alias]);
        if (cJSON_IsNumber(value)) {
            out->state.light.color_temperature =
                temp_to_kelvin(dps, value->valuedouble);
        }
    }
    return ESP_OK;
}

static esp_err_t map_cover_status(const slate_tuya_dps_t *dps, const cJSON *status,
                                  slate_snapshot_t *out)
{
    out->kind = SLATE_KIND_COVER;
    /* No motion DPs in v1: a polled cover at rest is the only state we can
     * honestly report. */
    out->state.cover = (slate_cover_state_t) {
        .position = SLATE_STATE_ABSENT,
        .motion = SLATE_COVER_IDLE,
    };

    bool publishable = false;
    if (dps->has_control) {
        publishable = true;
        out->capabilities.actions = (uint16_t) ((1u << SLATE_ACTION_OPEN) |
                                                (1u << SLATE_ACTION_STOP) |
                                                (1u << SLATE_ACTION_CLOSE));
    }
    if (dps->has_percent_control) {
        out->capabilities.actions |= (uint16_t) (1u << SLATE_ACTION_SET_POSITION);
        out->capabilities.position_min = 0;
        out->capabilities.position_max = 100;
    }
    if (dps->has_percent_state) {
        const cJSON *value = find_status_value(status, "percent_state");
        if (cJSON_IsNumber(value)) {
            long position = lround(value->valuedouble);
            if (position < 0) {
                position = 0;
            }
            if (position > 100) {
                position = 100;
            }
            out->state.cover.position = (int16_t) position;
            publishable = true;
        }
    }
    return publishable ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t map_sensor_status(const slate_tuya_dps_t *dps, const cJSON *status,
                                   const char *resource, slate_snapshot_t *out)
{
    out->kind = SLATE_KIND_SENSOR;
    slate_sensor_state_t sensor = {0};
    sensor.numeric = true;
    /* Category stays NONE: slate_state_publish() derives it from the
     * measurement, so writing it here would only duplicate that table. */

    if (dps->cls == SLATE_TUYA_CLASS_SENSOR_TEMP_HUM) {
        bool humidity = !dps->has_temperature ||
                        (dps->has_humidity && selects_humidity(resource));
        const cJSON *value;
        if (humidity) {
            value = find_status_value(status, "va_humidity");
            if (!cJSON_IsNumber(value)) {
                return ESP_ERR_NOT_FOUND;
            }
            sensor.value = value->valuedouble * dps->hum_scale;
            sensor.measurement = SLATE_MEASUREMENT_HUMIDITY;
            strcpy(sensor.unit, "%");
        } else {
            value = find_status_value(status, "va_temperature");
            if (!cJSON_IsNumber(value)) {
                return ESP_ERR_NOT_FOUND;
            }
            sensor.value = value->valuedouble * dps->temp_scale;
            sensor.measurement = SLATE_MEASUREMENT_TEMPERATURE;
            strcpy(sensor.unit, "°C");
        }
    } else { /* SLATE_TUYA_CLASS_SENSOR_POWER */
        const cJSON *value = find_status_value(status, "cur_power");
        if (!cJSON_IsNumber(value)) {
            return ESP_ERR_NOT_FOUND;
        }
        sensor.value = value->valuedouble * dps->power_scale;
        sensor.measurement = SLATE_MEASUREMENT_POWER;
        strcpy(sensor.unit, "W");
    }

    out->state.sensor = sensor;
    return ESP_OK;
}

esp_err_t slate_tuya_map_status(const slate_tuya_dps_t *dps, const cJSON *status_result,
                                const char *resource, const char *name,
                                slate_snapshot_t *out)
{
    if (dps == NULL || !cJSON_IsArray(status_result) || resource == NULL ||
        out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* available is the caller's to set — it knows whether the device answered
     * at all, which this module cannot see from one status payload. */
    out->resource = resource;
    out->name = name;
    out->area = NULL;
    out->capabilities = (slate_capabilities_t) {0};

    switch (dps->cls) {
    case SLATE_TUYA_CLASS_LIGHT:
        return map_light_status(dps, status_result, out);
    case SLATE_TUYA_CLASS_COVER:
        return map_cover_status(dps, status_result, out);
    case SLATE_TUYA_CLASS_SENSOR_TEMP_HUM:
    case SLATE_TUYA_CLASS_SENSOR_POWER:
        return map_sensor_status(dps, status_result, resource, out);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t build_body(const char *code, cJSON *value, cJSON **out_body)
{
    cJSON *command = cJSON_CreateObject();
    cJSON *commands = cJSON_CreateArray();
    cJSON *body = cJSON_CreateObject();
    if (command == NULL || commands == NULL || body == NULL || value == NULL) {
        cJSON_Delete(command);
        cJSON_Delete(commands);
        cJSON_Delete(body);
        cJSON_Delete(value);
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(command, "code", code);
    cJSON_AddItemToObject(command, "value", value);
    cJSON_AddItemToArray(commands, command);
    cJSON_AddItemToObject(body, "commands", commands);
    *out_body = body;
    return ESP_OK;
}

esp_err_t slate_tuya_map_command(const slate_tuya_dps_t *dps, slate_action_t action,
                                 bool has_bool, bool bool_value, int32_t number_value,
                                 cJSON **out_body)
{
    if (dps == NULL || out_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_body = NULL;

    const char *code = NULL;
    cJSON *value = NULL;

    switch (dps->cls) {
    case SLATE_TUYA_CLASS_LIGHT:
        switch (action) {
        case SLATE_ACTION_TOGGLE:
            /* Tuya has no toggle command; the provider resolves the target
             * from its last published state and passes it as has_bool. */
            if (!has_bool) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            /* fall through */
        case SLATE_ACTION_SET_POWER:
            if (!has_bool || !dps->can_switch) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            code = k_switch_codes[dps->switch_alias];
            value = cJSON_CreateBool(bool_value);
            break;
        case SLATE_ACTION_SET_BRIGHTNESS:
            if (!dps->can_bright) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            code = k_bright_codes[dps->bright_alias];
            value = cJSON_CreateNumber(percent_to_bright(dps, number_value));
            break;
        case SLATE_ACTION_SET_COLOR_TEMPERATURE:
            if (!dps->can_temp) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            code = k_temp_codes[dps->temp_alias];
            value = cJSON_CreateNumber(kelvin_to_temp(dps, number_value));
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
        }
        break;

    case SLATE_TUYA_CLASS_COVER:
        switch (action) {
        case SLATE_ACTION_OPEN:
        case SLATE_ACTION_STOP:
        case SLATE_ACTION_CLOSE:
            if (!dps->has_control) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            code = "control";
            value = cJSON_CreateString(action == SLATE_ACTION_OPEN    ? "open"
                                       : action == SLATE_ACTION_STOP  ? "stop"
                                                                      : "close");
            break;
        case SLATE_ACTION_SET_POSITION: {
            if (!dps->has_percent_control) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (number_value < 0) {
                number_value = 0;
            }
            if (number_value > 100) {
                number_value = 100;
            }
            code = "percent_control";
            value = cJSON_CreateNumber(number_value);
            break;
        }
        default:
            return ESP_ERR_NOT_SUPPORTED;
        }
        break;

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    return build_body(code, value, out_body);
}

#ifdef SLATE_TUYA_SELFTEST

esp_err_t slate_tuya_map_selftest(void)
{
    static const char FUNCTIONS_JSON[] =
        "[{\"code\":\"switch_led\",\"values\":\"{}\"},"
        "{\"code\":\"bright_value_v2\","
        "\"values\":\"{\\\"min\\\":10,\\\"max\\\":1000}\"},"
        "{\"code\":\"temp_value_v2\","
        "\"values\":\"{\\\"min\\\":0,\\\"max\\\":1000}\"}]";
    static const char STATUS_JSON[] =
        "[{\"code\":\"switch_led\",\"values\":\"{}\"},"
        "{\"code\":\"bright_value_v2\","
        "\"values\":\"{\\\"min\\\":10,\\\"max\\\":1000}\"},"
        "{\"code\":\"temp_value_v2\","
        "\"values\":\"{\\\"min\\\":0,\\\"max\\\":1000}\"}]";
    cJSON *functions = cJSON_Parse(FUNCTIONS_JSON);
    cJSON *status_schema = cJSON_Parse(STATUS_JSON);
    slate_tuya_dps_t dps;
    bool ok = functions != NULL && status_schema != NULL &&
              slate_tuya_map_specification("dj", functions, status_schema, &dps) == ESP_OK &&
              dps.has_switch && dps.can_switch && dps.has_bright && dps.can_bright &&
              dps.has_temp && dps.can_temp;

    cJSON *body = NULL;
    ok = ok && slate_tuya_map_command(&dps, SLATE_ACTION_SET_POWER, true, false, 0,
                                      &body) == ESP_OK;
    cJSON *commands = cJSON_GetObjectItemCaseSensitive(body, "commands");
    cJSON *command = cJSON_IsArray(commands) ? cJSON_GetArrayItem(commands, 0) : NULL;
    cJSON *value = cJSON_GetObjectItemCaseSensitive(command, "value");
    ok = ok && cJSON_IsFalse(value);
    cJSON_Delete(body);

    static const char SENSOR_STATUS_JSON[] =
        "[{\"code\":\"va_temperature\","
        "\"values\":\"{\\\"scale\\\":1}\"},"
        "{\"code\":\"va_humidity\",\"values\":\"{\\\"scale\\\":0}\"}]";
    cJSON *empty_functions = cJSON_Parse("[]");
    cJSON *sensor_status = cJSON_Parse(SENSOR_STATUS_JSON);
    slate_tuya_dps_t sensor;
    ok = ok && empty_functions != NULL && sensor_status != NULL &&
         slate_tuya_map_specification("wsdcg", empty_functions, sensor_status, &sensor) == ESP_OK &&
         sensor.has_temperature && sensor.has_humidity;

    cJSON *plain_plug = cJSON_Parse("[{\"code\":\"switch_1\",\"values\":\"{}\"}]");
    slate_tuya_dps_t plug;
    ok = ok && plain_plug != NULL &&
         slate_tuya_map_specification("cz", empty_functions, plain_plug, &plug) ==
             ESP_ERR_NOT_SUPPORTED;

    cJSON_Delete(plain_plug);
    cJSON_Delete(sensor_status);
    cJSON_Delete(empty_functions);
    cJSON_Delete(status_schema);
    cJSON_Delete(functions);
    return ok ? ESP_OK : ESP_FAIL;
}

#endif /* SLATE_TUYA_SELFTEST */
