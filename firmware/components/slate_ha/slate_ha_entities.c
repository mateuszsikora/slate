/* Home Assistant subscribe_entities diff expansion and neutral state mapping. */

#include "slate_ha_entities.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef SLATE_HA_SELFTEST
#include <stdio.h> /* snprintf, for building fixture JSON in the self-test only */
#endif

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "slate_ha.h"
#include "slate_state.h"

#define HA_STATE_MAX        31
#define HA_DEVICE_CLASS_MAX 31

#define HA_COVER_FEATURE_OPEN         (1u << 0)
#define HA_COVER_FEATURE_CLOSE        (1u << 1)
#define HA_COVER_FEATURE_STOP         (1u << 3)

static const char *TAG = "slate_ha_entities";

typedef struct {
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    bool present;
    bool has_state;
    bool raw_state_numeric;
    double raw_state_number;
    char raw_state[HA_STATE_MAX + 1];

    bool has_friendly_name;
    char friendly_name[SLATE_RESOURCE_NAME_MAX + 1];
    bool has_unit;
    char unit[SLATE_SENSOR_UNIT_MAX + 1];
    bool has_device_class;
    char device_class[HA_DEVICE_CLASS_MAX + 1];

    bool has_brightness;
    double brightness;
    bool has_color_temp_kelvin;
    double color_temp_kelvin;
    bool has_color_temp_mired;
    double color_temp_mired;
    bool has_min_color_temp_kelvin;
    double min_color_temp_kelvin;
    bool has_max_color_temp_kelvin;
    double max_color_temp_kelvin;
    bool has_min_mireds;
    double min_mireds;
    bool has_max_mireds;
    double max_mireds;
    bool has_supported_color_modes;
    bool brightness_mode;
    bool color_temp_mode;
    bool has_current_position;
    double current_position;
    bool has_supported_features;
    double supported_features;

    char normalized_name[SLATE_RESOURCE_NAME_MAX + 1];
    slate_capabilities_t normalized_capabilities;
    slate_light_state_t normalized_light;
    slate_cover_state_t normalized_cover;
    slate_sensor_state_t normalized_sensor;
    bool normalized_light_ready;
    bool normalized_cover_ready;
    bool normalized_sensor_ready;
} ha_entity_t;

static SemaphoreHandle_t s_lock;
static ha_entity_t *s_entities;
static size_t s_count;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static bool resource_ok(const char *resource)
{
    return resource != NULL && resource[0] != '\0' &&
           strnlen(resource, SLATE_RESOURCE_ID_MAX + 1) <= SLATE_RESOURCE_ID_MAX;
}

static ha_entity_t *find_entity(const char *resource)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_entities[i].resource, resource) == 0) {
            return &s_entities[i];
        }
    }
    return NULL;
}

static bool parse_sensor_number(const char *text, double *out);

static bool number_value(const cJSON *item, double *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

static void clear_raw_attributes(ha_entity_t *entity)
{
    entity->has_friendly_name = false;
    entity->has_unit = false;
    entity->has_device_class = false;
    entity->has_brightness = false;
    entity->has_color_temp_kelvin = false;
    entity->has_color_temp_mired = false;
    entity->has_min_color_temp_kelvin = false;
    entity->has_max_color_temp_kelvin = false;
    entity->has_min_mireds = false;
    entity->has_max_mireds = false;
    entity->has_supported_color_modes = false;
    entity->brightness_mode = false;
    entity->color_temp_mode = false;
    entity->has_current_position = false;
    entity->has_supported_features = false;
}

static void set_supported_color_modes(ha_entity_t *entity, const cJSON *item)
{
    if (!cJSON_IsArray(item)) {
        entity->has_supported_color_modes = false;
        entity->brightness_mode = false;
        entity->color_temp_mode = false;
        return;
    }

    entity->has_supported_color_modes = true;
    entity->brightness_mode = false;
    entity->color_temp_mode = false;
    const cJSON *mode = NULL;
    cJSON_ArrayForEach(mode, item) {
        if (!cJSON_IsString(mode)) {
            continue;
        }
        if (strcmp(mode->valuestring, "color_temp") == 0) {
            entity->color_temp_mode = true;
            entity->brightness_mode = true;
        } else if (strcmp(mode->valuestring, "onoff") != 0 &&
                   strcmp(mode->valuestring, "unknown") != 0) {
            entity->brightness_mode = true;
        }
    }
}

static void set_attribute(ha_entity_t *entity, const char *name, const cJSON *item)
{
    if (strcmp(name, "friendly_name") == 0) {
        entity->has_friendly_name = cJSON_IsString(item);
        if (entity->has_friendly_name) {
            strlcpy(entity->friendly_name, item->valuestring,
                    sizeof(entity->friendly_name));
        } else {
            entity->normalized_name[0] = '\0';
        }
    } else if (strcmp(name, "unit_of_measurement") == 0) {
        entity->has_unit = cJSON_IsString(item);
        if (entity->has_unit) {
            strlcpy(entity->unit, item->valuestring, sizeof(entity->unit));
        }
    } else if (strcmp(name, "device_class") == 0) {
        entity->has_device_class = cJSON_IsString(item);
        if (entity->has_device_class) {
            strlcpy(entity->device_class, item->valuestring,
                    sizeof(entity->device_class));
        }
    } else if (strcmp(name, "brightness") == 0) {
        entity->has_brightness = number_value(item, &entity->brightness);
    } else if (strcmp(name, "color_temp_kelvin") == 0) {
        entity->has_color_temp_kelvin = number_value(item, &entity->color_temp_kelvin);
    } else if (strcmp(name, "color_temp") == 0) {
        entity->has_color_temp_mired = number_value(item, &entity->color_temp_mired);
    } else if (strcmp(name, "min_color_temp_kelvin") == 0) {
        entity->has_min_color_temp_kelvin =
            number_value(item, &entity->min_color_temp_kelvin);
    } else if (strcmp(name, "max_color_temp_kelvin") == 0) {
        entity->has_max_color_temp_kelvin =
            number_value(item, &entity->max_color_temp_kelvin);
    } else if (strcmp(name, "min_mireds") == 0) {
        entity->has_min_mireds = number_value(item, &entity->min_mireds);
    } else if (strcmp(name, "max_mireds") == 0) {
        entity->has_max_mireds = number_value(item, &entity->max_mireds);
    } else if (strcmp(name, "supported_color_modes") == 0) {
        set_supported_color_modes(entity, item);
    } else if (strcmp(name, "current_position") == 0) {
        entity->has_current_position = number_value(item, &entity->current_position);
    } else if (strcmp(name, "supported_features") == 0) {
        entity->has_supported_features = number_value(item, &entity->supported_features);
    }
}

static void remove_attribute(ha_entity_t *entity, const char *name)
{
    if (strcmp(name, "friendly_name") == 0) {
        entity->has_friendly_name = false;
        entity->normalized_name[0] = '\0';
    } else if (strcmp(name, "unit_of_measurement") == 0) {
        entity->has_unit = false;
    } else if (strcmp(name, "device_class") == 0) {
        entity->has_device_class = false;
    } else if (strcmp(name, "brightness") == 0) {
        entity->has_brightness = false;
    } else if (strcmp(name, "color_temp_kelvin") == 0) {
        entity->has_color_temp_kelvin = false;
    } else if (strcmp(name, "color_temp") == 0) {
        entity->has_color_temp_mired = false;
    } else if (strcmp(name, "min_color_temp_kelvin") == 0) {
        entity->has_min_color_temp_kelvin = false;
    } else if (strcmp(name, "max_color_temp_kelvin") == 0) {
        entity->has_max_color_temp_kelvin = false;
    } else if (strcmp(name, "min_mireds") == 0) {
        entity->has_min_mireds = false;
    } else if (strcmp(name, "max_mireds") == 0) {
        entity->has_max_mireds = false;
    } else if (strcmp(name, "supported_color_modes") == 0) {
        entity->has_supported_color_modes = false;
        entity->brightness_mode = false;
        entity->color_temp_mode = false;
    } else if (strcmp(name, "current_position") == 0) {
        entity->has_current_position = false;
    } else if (strcmp(name, "supported_features") == 0) {
        entity->has_supported_features = false;
    }
}

static void apply_attributes(ha_entity_t *entity, const cJSON *attributes, bool replace)
{
    if (replace) {
        clear_raw_attributes(entity);
    }
    if (!cJSON_IsObject(attributes)) {
        return;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, attributes) {
        if (item->string != NULL) {
            set_attribute(entity, item->string, item);
        }
    }
}

static void apply_state_fields(ha_entity_t *entity, const cJSON *fields, bool replace)
{
    if (replace) {
        entity->present = true;
        entity->has_state = false;
        entity->raw_state_numeric = false;
        entity->normalized_name[0] = '\0';
        clear_raw_attributes(entity);
    }
    if (!cJSON_IsObject(fields)) {
        return;
    }

    const cJSON *state = cJSON_GetObjectItemCaseSensitive(fields, "s");
    if (state != NULL) {
        entity->has_state = cJSON_IsString(state);
        entity->raw_state_numeric = false;
        if (entity->has_state) {
            entity->raw_state_numeric =
                parse_sensor_number(state->valuestring, &entity->raw_state_number);
            strlcpy(entity->raw_state, state->valuestring, sizeof(entity->raw_state));
        }
    }
    const cJSON *attributes = cJSON_GetObjectItemCaseSensitive(fields, "a");
    if (attributes != NULL) {
        apply_attributes(entity, attributes, replace);
    }
}

static void apply_removals(ha_entity_t *entity, const cJSON *removals)
{
    if (!cJSON_IsObject(removals)) {
        return;
    }
    const cJSON *attributes = cJSON_GetObjectItemCaseSensitive(removals, "a");
    if (!cJSON_IsArray(attributes)) {
        return;
    }
    const cJSON *name = NULL;
    cJSON_ArrayForEach(name, attributes) {
        if (cJSON_IsString(name)) {
            remove_attribute(entity, name->valuestring);
        }
    }
}

static bool domain_is(const char *resource, const char *domain)
{
    size_t len = strlen(domain);
    return strncmp(resource, domain, len) == 0 && resource[len] == '.' &&
           resource[len + 1] != '\0';
}

static int16_t rounded_i16(double value)
{
    if (!isfinite(value) || value <= 0 || value > INT16_MAX) {
        return SLATE_STATE_ABSENT;
    }
    return (int16_t) lround(value);
}

static int16_t mired_to_kelvin(double value)
{
    return value > 0 ? rounded_i16(1000000.0 / value) : SLATE_STATE_ABSENT;
}

static bool cover_state_current(const ha_entity_t *entity)
{
    if (!entity->present || !entity->has_state) {
        return false;
    }
    return strcmp(entity->raw_state, "open") == 0 ||
           strcmp(entity->raw_state, "closed") == 0 ||
           strcmp(entity->raw_state, "opening") == 0 ||
           strcmp(entity->raw_state, "closing") == 0 ||
           strcmp(entity->raw_state, "stopped") == 0;
}

static uint32_t cover_features(const ha_entity_t *entity)
{
    if (!entity->has_supported_features || entity->supported_features < 0 ||
        entity->supported_features > UINT32_MAX ||
        floor(entity->supported_features) != entity->supported_features) {
        return 0;
    }
    return (uint32_t) entity->supported_features;
}

static slate_measurement_t measurement(const ha_entity_t *entity)
{
    if (!entity->has_device_class) {
        return SLATE_MEASUREMENT_NONE;
    }
    if (strcmp(entity->device_class, "temperature") == 0) {
        return SLATE_MEASUREMENT_TEMPERATURE;
    }
    if (strcmp(entity->device_class, "humidity") == 0) {
        return SLATE_MEASUREMENT_HUMIDITY;
    }
    if (strcmp(entity->device_class, "pressure") == 0) {
        return SLATE_MEASUREMENT_PRESSURE;
    }
    if (strcmp(entity->device_class, "power") == 0) {
        return SLATE_MEASUREMENT_POWER;
    }
    return SLATE_MEASUREMENT_NONE;
}

/*
 * §5.2's `category` from `device_class` — the same table for both sensor
 * domains, because the class names are one namespace and a `battery` reads as
 * a battery whether it arrives as 41 % or as `Low`.
 *
 * Several classes share a member. That is deliberate: §5.2's category answers
 * "what is this about" only as far as the panel draws it differently, and
 * `door` and `opening` are the same door. Classes with no glyph in
 * `tools/fonts/icons.txt` are left out rather than aimed at something close —
 * §7.3 falls back to the question mark, which is honest about not knowing, and
 * the tile's own `icon` remains the override for anything the adapter cannot
 * name.
 */
typedef struct {
    const char *device_class;
    slate_category_t category;
} ha_category_t;

static const ha_category_t k_categories[] = {
    {"temperature", SLATE_CATEGORY_TEMPERATURE},
    {"cold", SLATE_CATEGORY_TEMPERATURE},
    {"heat", SLATE_CATEGORY_TEMPERATURE},
    {"humidity", SLATE_CATEGORY_HUMIDITY},
    {"pressure", SLATE_CATEGORY_PRESSURE},
    {"atmospheric_pressure", SLATE_CATEGORY_PRESSURE},
    {"power", SLATE_CATEGORY_POWER},
    {"current", SLATE_CATEGORY_POWER},
    {"voltage", SLATE_CATEGORY_POWER},
    {"energy", SLATE_CATEGORY_POWER},
    {"power_factor", SLATE_CATEGORY_POWER},
    {"illuminance", SLATE_CATEGORY_ILLUMINANCE},
    {"light", SLATE_CATEGORY_ILLUMINANCE},
    {"aqi", SLATE_CATEGORY_AIR_QUALITY},
    {"pm1", SLATE_CATEGORY_AIR_QUALITY},
    {"pm25", SLATE_CATEGORY_AIR_QUALITY},
    {"pm10", SLATE_CATEGORY_AIR_QUALITY},
    {"gas", SLATE_CATEGORY_GAS},
    {"carbon_monoxide", SLATE_CATEGORY_GAS},
    {"carbon_dioxide", SLATE_CATEGORY_GAS},
    {"volatile_organic_compounds", SLATE_CATEGORY_GAS},
    {"nitrogen_dioxide", SLATE_CATEGORY_GAS},
    {"ozone", SLATE_CATEGORY_GAS},
    {"sulphur_dioxide", SLATE_CATEGORY_GAS},
    {"sound", SLATE_CATEGORY_SOUND},
    {"sound_pressure", SLATE_CATEGORY_SOUND},
    {"speed", SLATE_CATEGORY_SPEED},
    {"wind_speed", SLATE_CATEGORY_SPEED},
    {"battery", SLATE_CATEGORY_BATTERY},
    {"battery_charging", SLATE_CATEGORY_BATTERY},
    {"connectivity", SLATE_CATEGORY_CONNECTIVITY},
    {"signal_strength", SLATE_CATEGORY_CONNECTIVITY},
    {"door", SLATE_CATEGORY_DOOR},
    {"opening", SLATE_CATEGORY_DOOR},
    {"window", SLATE_CATEGORY_WINDOW},
    {"garage_door", SLATE_CATEGORY_GARAGE},
    {"motion", SLATE_CATEGORY_MOTION},
    {"moving", SLATE_CATEGORY_MOTION},
    {"vibration", SLATE_CATEGORY_MOTION},
    {"occupancy", SLATE_CATEGORY_OCCUPANCY},
    {"presence", SLATE_CATEGORY_OCCUPANCY},
    {"moisture", SLATE_CATEGORY_MOISTURE},
    {"smoke", SLATE_CATEGORY_SMOKE},
    {"lock", SLATE_CATEGORY_LOCK},
    {"plug", SLATE_CATEGORY_PLUG},
    {"problem", SLATE_CATEGORY_PROBLEM},
    {"safety", SLATE_CATEGORY_PROBLEM},
    {"tamper", SLATE_CATEGORY_PROBLEM},
    {"running", SLATE_CATEGORY_RUNNING},
    {"update", SLATE_CATEGORY_RUNNING},
};

static slate_category_t category(const ha_entity_t *entity)
{
    if (!entity->has_device_class) {
        return SLATE_CATEGORY_NONE;
    }
    for (size_t i = 0; i < sizeof(k_categories) / sizeof(k_categories[0]); i++) {
        if (strcmp(entity->device_class, k_categories[i].device_class) == 0) {
            return k_categories[i].category;
        }
    }
    return SLATE_CATEGORY_NONE;
}

/*
 * §5.2 keeps `kind` at four values, so a `binary_sensor` normalizes to `sensor`
 * with a textual value rather than earning a fifth. It is not a `light`: the
 * on/off shape would fit, but `light` carries `toggle` and `set_power` in
 * §5.2, and a door contact cannot honour either.
 *
 * The meaning lives entirely in `device_class`, so the words do too. These are
 * Home Assistant's own — the phrasing its frontend shows for each class — so
 * the panel says what the app the user came from says. `On`/`Off` is the
 * fallback for an absent or unrecognised class, which is also HA's.
 */
typedef struct {
    const char *device_class;
    const char *on;
    const char *off;
} ha_binary_phrasing_t;

static const ha_binary_phrasing_t k_binary_phrasings[] = {
    {"door", "Open", "Closed"},
    {"garage_door", "Open", "Closed"},
    {"opening", "Open", "Closed"},
    {"window", "Open", "Closed"},
    {"carbon_monoxide", "Detected", "Clear"},
    {"gas", "Detected", "Clear"},
    {"motion", "Detected", "Clear"},
    {"occupancy", "Detected", "Clear"},
    {"smoke", "Detected", "Clear"},
    {"sound", "Detected", "Clear"},
    {"tamper", "Detected", "Clear"},
    {"vibration", "Detected", "Clear"},
    {"moisture", "Wet", "Dry"},
    {"presence", "Home", "Away"},
    {"lock", "Unlocked", "Locked"},
    {"connectivity", "Connected", "Disconnected"},
    {"problem", "Problem", "OK"},
    {"safety", "Unsafe", "Safe"},
    {"battery", "Low", "Normal"},
    {"battery_charging", "Charging", "Not charging"},
    {"cold", "Cold", "Normal"},
    {"heat", "Hot", "Normal"},
    {"light", "Detected", "No light"},
    {"power", "Detected", "No power"},
    {"moving", "Moving", "Not moving"},
    {"running", "Running", "Not running"},
    {"plug", "Plugged in", "Unplugged"},
    {"update", "Update available", "Up-to-date"},
};

static const char *binary_sensor_text(const ha_entity_t *entity, bool on)
{
    if (entity->has_device_class) {
        for (size_t i = 0; i < sizeof(k_binary_phrasings) / sizeof(k_binary_phrasings[0]);
             i++) {
            if (strcmp(entity->device_class, k_binary_phrasings[i].device_class) == 0) {
                return on ? k_binary_phrasings[i].on : k_binary_phrasings[i].off;
            }
        }
    }
    return on ? "On" : "Off";
}

static bool parse_sensor_number(const char *text, double *out)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    double value = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' || !isfinite(value)) {
        return false;
    }
    *out = value;
    return true;
}

/* Mutates the last-normalized fields, then returns a self-contained copy. */
static bool normalize_entity(ha_entity_t *entity, ha_entity_t *out,
                             slate_kind_t *kind, bool *available)
{
    if (entity->has_friendly_name) {
        strlcpy(entity->normalized_name, entity->friendly_name,
                sizeof(entity->normalized_name));
    }

    if (domain_is(entity->resource, "light")) {
        *kind = SLATE_KIND_LIGHT;
        bool current = entity->present && entity->has_state &&
                       (strcmp(entity->raw_state, "on") == 0 ||
                        strcmp(entity->raw_state, "off") == 0);
        if (!entity->normalized_light_ready) {
            entity->normalized_light.brightness = SLATE_STATE_ABSENT;
            entity->normalized_light.color_temperature = SLATE_STATE_ABSENT;
            entity->normalized_light_ready = true;
        }
        if (current) {
            entity->normalized_light.on = strcmp(entity->raw_state, "on") == 0;
            entity->normalized_light.brightness =
                entity->has_brightness && entity->brightness >= 0 && entity->brightness <= 255
                    ? (int16_t) lround(entity->brightness * 100.0 / 255.0)
                    : SLATE_STATE_ABSENT;
            if (entity->has_color_temp_kelvin) {
                entity->normalized_light.color_temperature =
                    rounded_i16(entity->color_temp_kelvin);
            } else if (entity->has_color_temp_mired) {
                entity->normalized_light.color_temperature =
                    mired_to_kelvin(entity->color_temp_mired);
            } else {
                entity->normalized_light.color_temperature = SLATE_STATE_ABSENT;
            }

            slate_capabilities_t caps = {
                .actions = (uint16_t) ((1u << SLATE_ACTION_TOGGLE) |
                                       (1u << SLATE_ACTION_SET_POWER)),
            };
            if (entity->brightness_mode ||
                (!entity->has_supported_color_modes && entity->has_brightness)) {
                caps.actions |= (uint16_t) (1u << SLATE_ACTION_SET_BRIGHTNESS);
                caps.brightness_max = 100;
            }
            if (entity->color_temp_mode || entity->has_color_temp_kelvin ||
                entity->has_color_temp_mired) {
                caps.actions |= (uint16_t) (1u << SLATE_ACTION_SET_COLOR_TEMPERATURE);
                if (entity->has_min_color_temp_kelvin &&
                    entity->has_max_color_temp_kelvin) {
                    caps.color_temperature_min = rounded_i16(entity->min_color_temp_kelvin);
                    caps.color_temperature_max = rounded_i16(entity->max_color_temp_kelvin);
                } else if (entity->has_min_mireds && entity->has_max_mireds) {
                    caps.color_temperature_min = mired_to_kelvin(entity->max_mireds);
                    caps.color_temperature_max = mired_to_kelvin(entity->min_mireds);
                }
                if (caps.color_temperature_min == SLATE_STATE_ABSENT ||
                    caps.color_temperature_max == SLATE_STATE_ABSENT ||
                    caps.color_temperature_min > caps.color_temperature_max) {
                    caps.color_temperature_min = 0;
                    caps.color_temperature_max = 0;
                }
            }
            entity->normalized_capabilities = caps;
        }
        *available = current;
    } else if (domain_is(entity->resource, "cover")) {
        *kind = SLATE_KIND_COVER;
        bool current = cover_state_current(entity);
        if (!entity->normalized_cover_ready) {
            entity->normalized_cover.position = SLATE_STATE_ABSENT;
            entity->normalized_cover.motion = SLATE_COVER_IDLE;
            entity->normalized_cover_ready = true;
        }
        if (current) {
            if (entity->has_current_position && entity->current_position >= 0 &&
                entity->current_position <= 100) {
                entity->normalized_cover.position =
                    (int16_t) lround(entity->current_position);
            } else if (strcmp(entity->raw_state, "open") == 0) {
                entity->normalized_cover.position = 100;
            } else if (strcmp(entity->raw_state, "closed") == 0) {
                entity->normalized_cover.position = 0;
            }
            entity->normalized_cover.motion =
                strcmp(entity->raw_state, "opening") == 0
                    ? SLATE_COVER_OPENING
                    : strcmp(entity->raw_state, "closing") == 0
                          ? SLATE_COVER_CLOSING
                          : SLATE_COVER_IDLE;

            uint32_t features = cover_features(entity);
            slate_capabilities_t caps = {0};
            if ((features & (HA_COVER_FEATURE_OPEN | HA_COVER_FEATURE_CLOSE)) ==
                (HA_COVER_FEATURE_OPEN | HA_COVER_FEATURE_CLOSE)) {
                caps.actions |= 1u << SLATE_ACTION_TOGGLE;
            }
            if ((features & HA_COVER_FEATURE_OPEN) != 0) {
                caps.actions |= 1u << SLATE_ACTION_OPEN;
            }
            if ((features & HA_COVER_FEATURE_STOP) != 0) {
                caps.actions |= 1u << SLATE_ACTION_STOP;
            }
            if ((features & HA_COVER_FEATURE_CLOSE) != 0) {
                caps.actions |= 1u << SLATE_ACTION_CLOSE;
            }
            entity->normalized_capabilities = caps;
        }
        *available = current;
    } else if (domain_is(entity->resource, "sensor")) {
        *kind = SLATE_KIND_SENSOR;
        bool current = entity->present && entity->has_state &&
                       strcmp(entity->raw_state, "unavailable") != 0;
        if (!entity->normalized_sensor_ready) {
            strlcpy(entity->normalized_sensor.text, "unknown",
                    sizeof(entity->normalized_sensor.text));
            entity->normalized_sensor_ready = true;
        }
        if (current) {
            slate_sensor_state_t sensor = {0};
            sensor.numeric = entity->raw_state_numeric;
            if (sensor.numeric) {
                sensor.value = entity->raw_state_number;
            }
            if (!sensor.numeric) {
                strlcpy(sensor.text, entity->raw_state, sizeof(sensor.text));
                if (sensor.text[0] == '\0') {
                    strlcpy(sensor.text, "unknown", sizeof(sensor.text));
                }
            }
            if (entity->has_unit) {
                strlcpy(sensor.unit, entity->unit, sizeof(sensor.unit));
            }
            sensor.measurement = measurement(entity);
            entity->normalized_sensor = sensor;
            entity->normalized_capabilities = (slate_capabilities_t) {0};
        }
        /* Outside `current`, because `device_class` is an attribute and not a
         * state: what a resource is stays true while it has nothing to report.
         * §7.5 dims the tile and shows a dash for the value; the icon is the
         * one part of it that is still honest.
         *
         * But only while there is an entity to read it from. `present` is the
         * difference between "has nothing to say" and "there is nothing here":
         * a resubscription and a removal both clear the attributes without the
         * entity having lost a `device_class`, and recomputing the category
         * from that cleared copy would publish NONE over a known one. */
        if (entity->present) {
            entity->normalized_sensor.category = category(entity);
        }
        *available = current;
    } else if (domain_is(entity->resource, "binary_sensor")) {
        *kind = SLATE_KIND_SENSOR;
        /* The light rule, not the sensor one. A `sensor` may legitimately read
         * `unknown` as text, and the branch above shows it; here `unknown` is
         * the absence of an answer, and §7.5's dash says that without claiming
         * the door is closed. */
        bool current = entity->present && entity->has_state &&
                       (strcmp(entity->raw_state, "on") == 0 ||
                        strcmp(entity->raw_state, "off") == 0);
        if (!entity->normalized_sensor_ready) {
            strlcpy(entity->normalized_sensor.text, "unknown",
                    sizeof(entity->normalized_sensor.text));
            entity->normalized_sensor_ready = true;
        }
        if (current) {
            slate_sensor_state_t sensor = {0};
            strlcpy(sensor.text,
                    binary_sensor_text(entity, strcmp(entity->raw_state, "on") == 0),
                    sizeof(sensor.text));
            /* No unit and no measurement: `device_class` here names what the
             * contact means, not what it measures. §5.2 has one field for each,
             * so the door reaches §7.3 as a door without claiming a magnitude
             * the tile would then try to format. */
            entity->normalized_sensor = sensor;
            entity->normalized_capabilities = (slate_capabilities_t) {0};
        }
        /* A contact that has never answered is the case #133 exists for: it is
         * unavailable, so §7.5 gives it a dash — but it is still a door, and
         * without this it was a dash beside a question mark. Assigned after the
         * block above, which replaces the whole struct, and gated on `present`
         * for the reason given in the `sensor` branch: a reconnect clears the
         * attributes of every bound entity before the new state arrives, and
         * this is the one line that could overwrite a good category with the
         * question mark it exists to remove. */
        if (entity->present) {
            entity->normalized_sensor.category = category(entity);
        }
        *available = current;
    } else if (domain_is(entity->resource, "scene")) {
        *kind = SLATE_KIND_SCENE;
        bool current = entity->present && entity->has_state &&
                       strcmp(entity->raw_state, "unavailable") != 0;
        if (current) {
            entity->normalized_capabilities = (slate_capabilities_t) {
                .actions = 1u << SLATE_ACTION_ACTIVATE,
            };
        }
        *available = current;
    } else {
        return false;
    }

    *out = *entity;
    return true;
}

static esp_err_t publish_copy(const ha_entity_t *entity, slate_kind_t kind, bool available)
{
    slate_snapshot_t snapshot = {
        .resource = entity->resource,
        .kind = kind,
        .name = entity->normalized_name[0] != '\0' ? entity->normalized_name : NULL,
        .area = NULL,
        .available = available,
        .capabilities = entity->normalized_capabilities,
    };
    if (kind == SLATE_KIND_LIGHT) {
        snapshot.state.light = entity->normalized_light;
    } else if (kind == SLATE_KIND_COVER) {
        snapshot.state.cover = entity->normalized_cover;
    } else if (kind == SLATE_KIND_SENSOR) {
        snapshot.state.sensor = entity->normalized_sensor;
    }

    esp_err_t err = slate_state_publish(SLATE_HA_PROVIDER_ID, &snapshot);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "discarding mapped state for %s: %s", entity->resource,
                 esp_err_to_name(err));
    }
    return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t update_one(const char *resource, const cJSON *fields,
                            const cJSON *removals, bool replace, bool removed,
                            bool publish)
{
    ha_entity_t copy;
    slate_kind_t kind = SLATE_KIND_SENSOR;
    bool available = false;

    LOCK();
    ha_entity_t *entity = find_entity(resource);
    if (entity == NULL) {
        UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    if (removed) {
        entity->present = false;
        entity->has_state = false;
        clear_raw_attributes(entity);
    } else {
        entity->present = true;
        apply_state_fields(entity, fields, replace);
        apply_removals(entity, removals);
    }
    bool supported = normalize_entity(entity, &copy, &kind, &available);
    UNLOCK();

    if (!supported || !publish) {
        return ESP_OK;
    }
    return publish_copy(&copy, kind, available);
}

static esp_err_t process_event(const cJSON *event, bool publish)
{
    if (!cJSON_IsObject(event)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t first_error = ESP_OK;

    const cJSON *added = cJSON_GetObjectItemCaseSensitive(event, "a");
    if (added != NULL && !cJSON_IsObject(added)) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, added) {
        if (item->string == NULL || !cJSON_IsObject(item)) {
            continue;
        }
        esp_err_t err = update_one(item->string, item, NULL, true, false, publish);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND && first_error == ESP_OK) {
            first_error = err;
        }
    }

    const cJSON *changed = cJSON_GetObjectItemCaseSensitive(event, "c");
    if (changed != NULL && !cJSON_IsObject(changed)) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_ArrayForEach(item, changed) {
        if (item->string == NULL || !cJSON_IsObject(item)) {
            continue;
        }
        const cJSON *plus = cJSON_GetObjectItemCaseSensitive(item, "+");
        const cJSON *minus = cJSON_GetObjectItemCaseSensitive(item, "-");
        esp_err_t err = update_one(item->string, plus, minus, false, false, publish);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND && first_error == ESP_OK) {
            first_error = err;
        }
    }

    const cJSON *removed = cJSON_GetObjectItemCaseSensitive(event, "r");
    if (removed != NULL && !cJSON_IsArray(removed)) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_ArrayForEach(item, removed) {
        if (!cJSON_IsString(item)) {
            continue;
        }
        esp_err_t err = update_one(item->valuestring, NULL, NULL, false, true, publish);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND && first_error == ESP_OK) {
            first_error = err;
        }
    }
    return first_error;
}

esp_err_t slate_ha_entities_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t slate_ha_entities_bind(const char *const *resources, size_t count,
                                 bool *changed)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (changed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *changed = false;
    if (count > SLATE_STATE_MAX_RESOURCES || (count > 0 && resources == NULL)) {
        return count > SLATE_STATE_MAX_RESOURCES ? ESP_ERR_INVALID_SIZE
                                                 : ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; i++) {
        if (!resource_ok(resources[i])) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(resources[i], resources[j]) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    /* Membership is the contract. A layout-only rebuild may reorder the same
     * ids and must not interrupt a healthy HA subscription. */
    LOCK();
    bool same = count == s_count;
    for (size_t i = 0; same && i < count; i++) {
        same = find_entity(resources[i]) != NULL;
    }
    UNLOCK();
    if (same) {
        return ESP_OK;
    }

    ha_entity_t *fresh = NULL;
    if (count > 0) {
        fresh = heap_caps_calloc_prefer(count, sizeof(*fresh), 2,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (fresh == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    LOCK();
    for (size_t i = 0; i < count; i++) {
        strlcpy(fresh[i].resource, resources[i], sizeof(fresh[i].resource));
        ha_entity_t *previous = find_entity(resources[i]);
        if (previous != NULL) {
            char resource[sizeof(fresh[i].resource)];
            strlcpy(resource, fresh[i].resource, sizeof(resource));
            fresh[i] = *previous;
            strlcpy(fresh[i].resource, resource, sizeof(fresh[i].resource));
        }
    }
    ha_entity_t *old = s_entities;
    s_entities = fresh;
    s_count = count;
    UNLOCK();
    free(old);
    *changed = true;
    return ESP_OK;
}

esp_err_t slate_ha_entities_append_ids(cJSON *array, size_t *count)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!cJSON_IsArray(array) || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    LOCK();
    *count = s_count;
    for (size_t i = 0; i < s_count; i++) {
        cJSON *id = cJSON_CreateString(s_entities[i].resource);
        if (id == NULL || !cJSON_AddItemToArray(array, id)) {
            cJSON_Delete(id);
            err = ESP_ERR_NO_MEM;
            *count = 0;
            break;
        }
    }
    UNLOCK();
    return err;
}

esp_err_t slate_ha_entities_prepare_subscription(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    typedef struct {
        ha_entity_t entity;
        slate_kind_t kind;
        bool supported;
    } prepared_t;

    LOCK();
    size_t count = s_count;
    prepared_t *prepared = count > 0
                               ? heap_caps_calloc_prefer(
                                     count, sizeof(*prepared), 2,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                               : NULL;
    if (count > 0 && prepared == NULL) {
        UNLOCK();
        ESP_LOGW(TAG, "no memory to mark a new subscription unavailable");
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < count; i++) {
        s_entities[i].present = false;
        s_entities[i].has_state = false;
        clear_raw_attributes(&s_entities[i]);
        bool available = false;
        prepared[i].supported = normalize_entity(&s_entities[i], &prepared[i].entity,
                                                  &prepared[i].kind, &available);
    }
    UNLOCK();

    esp_err_t first_error = ESP_OK;
    for (size_t i = 0; i < count; i++) {
        if (prepared[i].supported) {
            esp_err_t err = publish_copy(&prepared[i].entity, prepared[i].kind, false);
            if (err != ESP_OK && first_error == ESP_OK) {
                first_error = err;
            }
        }
    }
    free(prepared);
    return first_error;
}

esp_err_t slate_ha_entities_process_event(const cJSON *event)
{
    return process_event(event, true);
}

esp_err_t slate_ha_entities_normalize_full_state(const cJSON *state,
                                                 slate_resource_t *out)
{
    if (!cJSON_IsObject(state) || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *entity_id = cJSON_GetObjectItemCaseSensitive(state, "entity_id");
    const cJSON *raw_state = cJSON_GetObjectItemCaseSensitive(state, "state");
    const cJSON *attributes = cJSON_GetObjectItemCaseSensitive(state, "attributes");
    if (!cJSON_IsString(entity_id) || !resource_ok(entity_id->valuestring) ||
        !cJSON_IsString(raw_state) || !cJSON_IsObject(attributes)) {
        return ESP_ERR_INVALID_ARG;
    }

    ha_entity_t entity = {0};
    strlcpy(entity.resource, entity_id->valuestring, sizeof(entity.resource));
    entity.present = true;
    entity.has_state = true;
    entity.raw_state_numeric =
        parse_sensor_number(raw_state->valuestring, &entity.raw_state_number);
    strlcpy(entity.raw_state, raw_state->valuestring, sizeof(entity.raw_state));
    apply_attributes(&entity, attributes, true);

    ha_entity_t normalized;
    slate_kind_t kind;
    bool available = false;
    if (!normalize_entity(&entity, &normalized, &kind, &available)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    *out = (slate_resource_t) {0};
    strlcpy(out->provider, SLATE_HA_PROVIDER_ID, sizeof(out->provider));
    strlcpy(out->resource, normalized.resource, sizeof(out->resource));
    strlcpy(out->name, normalized.normalized_name, sizeof(out->name));
    out->kind = kind;
    out->presentation = available ? SLATE_PRESENT_OK : SLATE_PRESENT_UNAVAILABLE;
    out->capabilities = normalized.normalized_capabilities;
    if (kind == SLATE_KIND_LIGHT) {
        out->state.light = normalized.normalized_light;
    } else if (kind == SLATE_KIND_COVER) {
        out->state.cover = normalized.normalized_cover;
    } else if (kind == SLATE_KIND_SENSOR) {
        out->state.sensor = normalized.normalized_sensor;
    }
    return ESP_OK;
}

#ifdef SLATE_HA_SELFTEST

esp_err_t slate_ha_entities_selftest(void)
{
    unsigned failures = 0;
#define CHECK(condition, name) do {                                                \
        if (condition) {                                                          \
            ESP_LOGI(TAG, "selftest PASS: %s", name);                            \
        } else {                                                                  \
            ESP_LOGE(TAG, "selftest FAIL: %s", name);                            \
            failures++;                                                          \
        }                                                                         \
    } while (0)

    const char *ids[] = {
        "light.kitchen",
        "cover.office_blind",
        "sensor.room_temperature",
        "scene.relax",
        "binary_sensor.front_door",
    };
    const size_t id_count = sizeof(ids) / sizeof(ids[0]);
    bool binding_changed = false;
    CHECK(slate_ha_entities_bind(ids, id_count, &binding_changed) == ESP_OK &&
              binding_changed,
          "bind explicit entity ids");
    const char *reordered[] = {
        "scene.relax",
        "binary_sensor.front_door",
        "sensor.room_temperature",
        "cover.office_blind",
        "light.kitchen",
    };
    CHECK(slate_ha_entities_bind(reordered, id_count, &binding_changed) == ESP_OK &&
              !binding_changed,
          "unchanged entity set avoids resubscription");
    const char *duplicates[] = {"light.kitchen", "light.kitchen"};
    CHECK(slate_ha_entities_bind(duplicates, 2, &binding_changed) == ESP_ERR_INVALID_ARG &&
              !binding_changed,
          "duplicate entity ids are refused");
    CHECK(slate_ha_entities_prepare_subscription() == ESP_OK,
          "prepare bound entities as unavailable");

    cJSON *initial = cJSON_Parse(
        "{\"a\":{\"light.kitchen\":{\"s\":\"on\",\"a\":{"
        "\"friendly_name\":\"Kitchen\",\"brightness\":128,"
        "\"supported_color_modes\":[\"brightness\"]}},"
        "\"cover.office_blind\":{\"s\":\"opening\",\"a\":{"
        "\"friendly_name\":\"Office blind\",\"current_position\":43,"
        "\"supported_features\":15}},"
        "\"sensor.room_temperature\":{\"s\":\"21.5\",\"a\":{"
        "\"friendly_name\":\"Room temperature\","
        "\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\"}},"
        "\"scene.relax\":{\"s\":\"2026-08-12T12:00:00.000000+00:00\",\"a\":{"
        "\"friendly_name\":\"Relax\"}},"
        "\"binary_sensor.front_door\":{\"s\":\"on\",\"a\":{"
        "\"friendly_name\":\"Front door\",\"device_class\":\"door\"}}}}"
    );
    CHECK(initial != NULL && process_event(initial, false) == ESP_OK,
          "expand initial additions");
    cJSON_Delete(initial);

    ha_entity_t light;
    ha_entity_t cover;
    ha_entity_t sensor;
    ha_entity_t scene;
    slate_kind_t light_kind;
    slate_kind_t cover_kind;
    slate_kind_t sensor_kind;
    slate_kind_t scene_kind;
    bool light_available = false;
    bool cover_available = false;
    bool sensor_available = false;
    bool scene_available = false;
    LOCK();
    bool mapped_light = normalize_entity(find_entity(ids[0]), &light, &light_kind,
                                          &light_available);
    bool mapped_cover = normalize_entity(find_entity(ids[1]), &cover, &cover_kind,
                                          &cover_available);
    bool mapped_sensor = normalize_entity(find_entity(ids[2]), &sensor, &sensor_kind,
                                           &sensor_available);
    bool mapped_scene = normalize_entity(find_entity(ids[3]), &scene, &scene_kind,
                                          &scene_available);
    UNLOCK();
    CHECK(mapped_light && light_kind == SLATE_KIND_LIGHT && light_available &&
              light.normalized_light.on && light.normalized_light.brightness == 50 &&
              slate_capabilities_have(&light.normalized_capabilities,
                                      SLATE_ACTION_SET_BRIGHTNESS),
          "map light state and capability");
    CHECK(mapped_cover && cover_kind == SLATE_KIND_COVER && cover_available &&
              cover.normalized_cover.position == 43 &&
              cover.normalized_cover.motion == SLATE_COVER_OPENING &&
              slate_capabilities_have(&cover.normalized_capabilities,
                                      SLATE_ACTION_TOGGLE) &&
              slate_capabilities_have(&cover.normalized_capabilities,
                                      SLATE_ACTION_OPEN) &&
              slate_capabilities_have(&cover.normalized_capabilities,
                                      SLATE_ACTION_STOP) &&
              slate_capabilities_have(&cover.normalized_capabilities,
                                      SLATE_ACTION_CLOSE) &&
              !slate_capabilities_have(&cover.normalized_capabilities,
                                       SLATE_ACTION_SET_POSITION),
          "map cover position, motion and feature bits");

    cJSON *cover_change = cJSON_Parse(
        "{\"c\":{\"cover.office_blind\":{\"+\":{\"s\":\"closing\","
        "\"a\":{\"current_position\":62}}}}}"
    );
    CHECK(cover_change != NULL && process_event(cover_change, false) == ESP_OK,
          "apply compressed cover motion update");
    cJSON_Delete(cover_change);
    LOCK();
    mapped_cover = normalize_entity(find_entity(ids[1]), &cover, &cover_kind,
                                    &cover_available);
    UNLOCK();
    CHECK(mapped_cover && cover_available && cover.normalized_cover.position == 62 &&
              cover.normalized_cover.motion == SLATE_COVER_CLOSING,
          "map a mid-travel closing cover");

    cJSON *cover_unavailable = cJSON_Parse(
        "{\"c\":{\"cover.office_blind\":{\"+\":{\"s\":\"unavailable\"}}}}"
    );
    CHECK(cover_unavailable != NULL &&
              process_event(cover_unavailable, false) == ESP_OK,
          "apply unavailable cover state");
    cJSON_Delete(cover_unavailable);
    LOCK();
    mapped_cover = normalize_entity(find_entity(ids[1]), &cover, &cover_kind,
                                    &cover_available);
    UNLOCK();
    CHECK(mapped_cover && !cover_available && cover.normalized_cover.position == 62 &&
              cover.normalized_cover.motion == SLATE_COVER_CLOSING,
          "unavailable cover keeps its last normalized state");

    CHECK(mapped_sensor && sensor_kind == SLATE_KIND_SENSOR && sensor_available &&
              sensor.normalized_sensor.numeric && sensor.normalized_sensor.value == 21.5 &&
              sensor.normalized_sensor.measurement == SLATE_MEASUREMENT_TEMPERATURE &&
              sensor.normalized_sensor.category == SLATE_CATEGORY_TEMPERATURE &&
              strcmp(sensor.normalized_sensor.unit, "°C") == 0,
          "map numeric sensor metadata");
    CHECK(mapped_scene && scene_kind == SLATE_KIND_SCENE && scene_available &&
              strcmp(scene.normalized_name, "Relax") == 0 &&
              slate_capabilities_have(&scene.normalized_capabilities,
                                      SLATE_ACTION_ACTIVATE),
          "map stateless scene and activation capability");

    ha_entity_t contact;
    slate_kind_t contact_kind;
    bool contact_available = false;
    LOCK();
    bool mapped_contact = normalize_entity(find_entity(ids[4]), &contact, &contact_kind,
                                           &contact_available);
    UNLOCK();
    CHECK(mapped_contact && contact_kind == SLATE_KIND_SENSOR && contact_available &&
              !contact.normalized_sensor.numeric &&
              strcmp(contact.normalized_sensor.text, "Open") == 0 &&
              contact.normalized_sensor.unit[0] == '\0' &&
              contact.normalized_sensor.measurement == SLATE_MEASUREMENT_NONE &&
              contact.normalized_sensor.category == SLATE_CATEGORY_DOOR &&
              contact.normalized_capabilities.actions == 0,
          "map a binary_sensor onto a read-only textual sensor with a category");

    cJSON *door_closed = cJSON_Parse(
        "{\"c\":{\"binary_sensor.front_door\":{\"+\":{\"s\":\"off\"}}}}"
    );
    CHECK(door_closed != NULL && process_event(door_closed, false) == ESP_OK,
          "apply compressed binary_sensor update");
    cJSON_Delete(door_closed);
    LOCK();
    mapped_contact = normalize_entity(find_entity(ids[4]), &contact, &contact_kind,
                                      &contact_available);
    UNLOCK();
    CHECK(mapped_contact && contact_available &&
              strcmp(contact.normalized_sensor.text, "Closed") == 0,
          "binary_sensor follows the entity to the other phrasing");

    /* `unknown` is not `off`. A sensor may read `unknown` as text and the
     * branch above shows it; a contact that says so has not answered, and
     * §7.5's dash is the honest rendering. */
    static const struct {
        const char *state;
        const char *name;
    } unanswered[] = {
        {"unknown", "unknown binary_sensor goes stale keeping its last word"},
        {"unavailable", "unavailable binary_sensor goes stale keeping its last word"},
    };
    for (size_t i = 0; i < sizeof(unanswered) / sizeof(unanswered[0]); i++) {
        char diff[128];
        snprintf(diff, sizeof(diff),
                 "{\"c\":{\"binary_sensor.front_door\":{\"+\":{\"s\":\"%s\"}}}}",
                 unanswered[i].state);
        cJSON *parsed = cJSON_Parse(diff);
        CHECK(parsed != NULL && process_event(parsed, false) == ESP_OK,
              "apply an unanswered binary_sensor state");
        cJSON_Delete(parsed);
        LOCK();
        mapped_contact = normalize_entity(find_entity(ids[4]), &contact, &contact_kind,
                                          &contact_available);
        UNLOCK();
        CHECK(mapped_contact && !contact_available &&
                  strcmp(contact.normalized_sensor.text, "Closed") == 0,
              unanswered[i].name);
    }

    /*
     * Every `device_class` the adapter knows, both ways round, plus the
     * fallback for a contact that declares no class at all.
     *
     * The words are spelled out here rather than read from k_binary_phrasings
     * on purpose: a test that iterated the production table would assert it
     * against itself and a typo would pass. Written out, changing a word is a
     * visible change to a test, which is what DESIGN.md §5.6 promises. The
     * count is asserted below so the duplication cannot go stale in the other
     * direction either — a class added to the table without a case here fails.
     */
    static const struct {
        const char *device_class;
        const char *on;
        const char *off;
    } phrasings[] = {
        {"door", "Open", "Closed"},
        {"garage_door", "Open", "Closed"},
        {"opening", "Open", "Closed"},
        {"window", "Open", "Closed"},
        {"carbon_monoxide", "Detected", "Clear"},
        {"gas", "Detected", "Clear"},
        {"motion", "Detected", "Clear"},
        {"occupancy", "Detected", "Clear"},
        {"smoke", "Detected", "Clear"},
        {"sound", "Detected", "Clear"},
        {"tamper", "Detected", "Clear"},
        {"vibration", "Detected", "Clear"},
        {"moisture", "Wet", "Dry"},
        {"presence", "Home", "Away"},
        {"lock", "Unlocked", "Locked"},
        {"connectivity", "Connected", "Disconnected"},
        {"problem", "Problem", "OK"},
        {"safety", "Unsafe", "Safe"},
        {"battery", "Low", "Normal"},
        {"battery_charging", "Charging", "Not charging"},
        {"cold", "Cold", "Normal"},
        {"heat", "Hot", "Normal"},
        {"light", "Detected", "No light"},
        {"power", "Detected", "No power"},
        {"moving", "Moving", "Not moving"},
        {"running", "Running", "Not running"},
        {"plug", "Plugged in", "Unplugged"},
        {"update", "Update available", "Up-to-date"},
        {NULL, "On", "Off"},
    };
    /* -1 for the NULL fallback, which is not a row of the table. */
    CHECK(sizeof(phrasings) / sizeof(phrasings[0]) - 1 ==
              sizeof(k_binary_phrasings) / sizeof(k_binary_phrasings[0]),
          "every known device_class has a phrasing case");
    bool phrasings_ok = true;
    for (size_t i = 0; i < sizeof(phrasings) / sizeof(phrasings[0]); i++) {
        for (unsigned on = 0; on <= 1; on++) {
            char class_attribute[64] = "";
            if (phrasings[i].device_class != NULL) {
                snprintf(class_attribute, sizeof(class_attribute),
                         ",\"device_class\":\"%s\"", phrasings[i].device_class);
            }
            char full[256];
            snprintf(full, sizeof(full),
                     "{\"entity_id\":\"binary_sensor.probe\",\"state\":\"%s\","
                     "\"attributes\":{\"friendly_name\":\"Probe\"%s}}",
                     on ? "on" : "off", class_attribute);
            cJSON *parsed = cJSON_Parse(full);
            slate_resource_t probe;
            const char *want = on ? phrasings[i].on : phrasings[i].off;
            bool ok = parsed != NULL &&
                      slate_ha_entities_normalize_full_state(parsed, &probe) == ESP_OK &&
                      probe.kind == SLATE_KIND_SENSOR &&
                      probe.presentation == SLATE_PRESENT_OK &&
                      !probe.state.sensor.numeric &&
                      strcmp(probe.state.sensor.text, want) == 0;
            cJSON_Delete(parsed);
            if (!ok) {
                ESP_LOGE(TAG, "binary_sensor %s/%s did not read \"%s\"",
                         phrasings[i].device_class != NULL ? phrasings[i].device_class
                                                           : "(no device_class)",
                         on ? "on" : "off", want);
                phrasings_ok = false;
            }
        }
    }
    CHECK(phrasings_ok, "every binary_sensor device_class reads its words");

    /* §7.3 picks the icon from `category`, so a member no `device_class` names
     * is a glyph no Home Assistant entity can ever reach — and the tile falls
     * back to the question mark this exists to remove. Checked both ways: every
     * member is reachable, and every row of the table names a real member. */
    bool categories_reachable = true;
    for (int member = SLATE_CATEGORY_NONE + 1; member < SLATE_CATEGORY_COUNT; member++) {
        bool named = false;
        for (size_t i = 0; i < sizeof(k_categories) / sizeof(k_categories[0]); i++) {
            named = named || k_categories[i].category == member;
        }
        if (!named) {
            ESP_LOGE(TAG, "no device_class maps onto category %s",
                     slate_category_str((slate_category_t) member));
            categories_reachable = false;
        }
    }
    for (size_t i = 0; i < sizeof(k_categories) / sizeof(k_categories[0]); i++) {
        if (k_categories[i].category == SLATE_CATEGORY_NONE ||
            k_categories[i].category >= SLATE_CATEGORY_COUNT) {
            ESP_LOGE(TAG, "device_class %s maps onto no category",
                     k_categories[i].device_class);
            categories_reachable = false;
        }
        /* A class listed twice is dead code the two-way check above cannot
         * see: the second row is never reached, and the second answer is the
         * one a reader of the table would believe. */
        for (size_t j = i + 1; j < sizeof(k_categories) / sizeof(k_categories[0]); j++) {
            if (strcmp(k_categories[i].device_class, k_categories[j].device_class) == 0) {
                ESP_LOGE(TAG, "device_class %s is in the table twice",
                         k_categories[i].device_class);
                categories_reachable = false;
            }
        }
    }
    CHECK(categories_reachable, "every category is named by one device_class and back");

    /* The two domains through one table: a contact with no magnitude, and a
     * numeric reading whose class is outside §5.2's four measurements. Both
     * used to arrive with nothing an icon could be chosen from. */
    static const struct {
        const char *entity;
        const char *state;
        const char *device_class;
        slate_category_t category;
        slate_measurement_t measurement;
        slate_presentation_t presentation;
    } categorised[] = {
        {"binary_sensor.probe", "on", "window", SLATE_CATEGORY_WINDOW,
         SLATE_MEASUREMENT_NONE, SLATE_PRESENT_OK},
        {"binary_sensor.probe", "off", "motion", SLATE_CATEGORY_MOTION,
         SLATE_MEASUREMENT_NONE, SLATE_PRESENT_OK},
        {"binary_sensor.probe", "on", "carbon_monoxide", SLATE_CATEGORY_GAS,
         SLATE_MEASUREMENT_NONE, SLATE_PRESENT_OK},
        {"sensor.probe", "412", "illuminance", SLATE_CATEGORY_ILLUMINANCE,
         SLATE_MEASUREMENT_NONE, SLATE_PRESENT_OK},
        {"sensor.probe", "41", "battery", SLATE_CATEGORY_BATTERY, SLATE_MEASUREMENT_NONE,
         SLATE_PRESENT_OK},
        {"sensor.probe", "21.5", "temperature", SLATE_CATEGORY_TEMPERATURE,
         SLATE_MEASUREMENT_TEMPERATURE, SLATE_PRESENT_OK},
        {"sensor.probe", "8", "irradiance", SLATE_CATEGORY_NONE, SLATE_MEASUREMENT_NONE,
         SLATE_PRESENT_OK},
        /* The panel that came up before the Zigbee integration did, and the
         * contact whose battery died before anyone bound it. A `device_class`
         * is an attribute, so it is there to be read even though no state ever
         * arrived — and the whole point of #133 is that this tile stops being
         * a dash beside a question mark. */
        {"binary_sensor.probe", "unknown", "door", SLATE_CATEGORY_DOOR,
         SLATE_MEASUREMENT_NONE, SLATE_PRESENT_UNAVAILABLE},
        {"binary_sensor.probe", "unavailable", "moisture", SLATE_CATEGORY_MOISTURE,
         SLATE_MEASUREMENT_NONE, SLATE_PRESENT_UNAVAILABLE},
        {"sensor.probe", "unavailable", "battery", SLATE_CATEGORY_BATTERY,
         SLATE_MEASUREMENT_NONE, SLATE_PRESENT_UNAVAILABLE},
    };
    bool categorised_ok = true;
    for (size_t i = 0; i < sizeof(categorised) / sizeof(categorised[0]); i++) {
        char full[256];
        snprintf(full, sizeof(full),
                 "{\"entity_id\":\"%s\",\"state\":\"%s\","
                 "\"attributes\":{\"friendly_name\":\"Probe\",\"device_class\":\"%s\"}}",
                 categorised[i].entity, categorised[i].state, categorised[i].device_class);
        cJSON *parsed = cJSON_Parse(full);
        slate_resource_t probe;
        bool ok = parsed != NULL &&
                  slate_ha_entities_normalize_full_state(parsed, &probe) == ESP_OK &&
                  probe.kind == SLATE_KIND_SENSOR &&
                  probe.presentation == categorised[i].presentation &&
                  probe.state.sensor.category == categorised[i].category &&
                  probe.state.sensor.measurement == categorised[i].measurement;
        cJSON_Delete(parsed);
        if (!ok) {
            ESP_LOGE(TAG, "%s/%s (%s) did not normalize to category %s",
                     categorised[i].entity, categorised[i].device_class,
                     categorised[i].state,
                     slate_category_str(categorised[i].category) != NULL
                         ? slate_category_str(categorised[i].category)
                         : "(none)");
            categorised_ok = false;
        }
    }
    CHECK(categorised_ok, "a device_class becomes a category, answered or not");

    cJSON *change = cJSON_Parse(
        "{\"c\":{\"light.kitchen\":{\"+\":{\"s\":\"off\","
        "\"a\":{\"brightness\":0}},\"-\":{\"a\":[\"friendly_name\","
        "\"supported_color_modes\"]}}}}"
    );
    CHECK(change != NULL && process_event(change, false) == ESP_OK,
          "apply compressed additions and removals");
    cJSON_Delete(change);
    LOCK();
    mapped_light = normalize_entity(find_entity(ids[0]), &light, &light_kind,
                                    &light_available);
    UNLOCK();
    CHECK(mapped_light && !light.normalized_light.on &&
              light.normalized_light.brightness == 0 && light.normalized_name[0] == '\0',
          "partial light update keeps a complete snapshot");

    cJSON *long_number = cJSON_Parse(
        "{\"c\":{\"sensor.room_temperature\":{\"+\":{"
        "\"s\":\"1234567890123456789012345678901234567890\"}}}}"
    );
    CHECK(long_number != NULL && process_event(long_number, false) == ESP_OK,
          "accept long textual sensor state");
    cJSON_Delete(long_number);
    LOCK();
    mapped_sensor = normalize_entity(find_entity(ids[2]), &sensor, &sensor_kind,
                                     &sensor_available);
    UNLOCK();
    CHECK(mapped_sensor && sensor_available && sensor.normalized_sensor.numeric &&
              sensor.normalized_sensor.value > 1e39,
          "long numeric sensor state is parsed before text truncation");

    cJSON *restore_sensor = cJSON_Parse(
        "{\"c\":{\"sensor.room_temperature\":{\"+\":{\"s\":\"21.5\"}}}}"
    );
    CHECK(restore_sensor != NULL && process_event(restore_sensor, false) == ESP_OK,
          "restore numeric sensor fixture");
    cJSON_Delete(restore_sensor);

    cJSON *removed = cJSON_Parse("{\"r\":[\"sensor.room_temperature\"]}");
    CHECK(removed != NULL && process_event(removed, false) == ESP_OK,
          "expand entity removal");
    cJSON_Delete(removed);
    LOCK();
    mapped_sensor = normalize_entity(find_entity(ids[2]), &sensor, &sensor_kind,
                                     &sensor_available);
    UNLOCK();
    CHECK(mapped_sensor && !sensor_available && sensor.normalized_sensor.numeric &&
              sensor.normalized_sensor.value == 21.5,
          "removed entity keeps its last value unavailable");

    cJSON *bad_diff = cJSON_Parse("{\"c\":[]}");
    CHECK(bad_diff != NULL && process_event(bad_diff, false) == ESP_ERR_INVALID_ARG,
          "malformed compressed diff is refused");
    cJSON_Delete(bad_diff);

    /*
     * A reconnect, on entities that already have a category.
     *
     * §5.6 re-subscribes on every reconnect, and slate_ha_entities_prepare_
     * subscription() marks every bound entity unavailable first — which clears
     * its attributes, `device_class` among them. The category has to survive
     * that: the entity did not lose anything, the socket did, and a tile that
     * answers a dropped WebSocket with a question mark is the symptom #133
     * exists to remove, arriving through a different door.
     *
     * Last in the file because prepare_subscription() takes the state away from
     * every entity, so nothing after it could assume one.
     */
    cJSON *restated = cJSON_Parse(
        "{\"a\":{\"binary_sensor.front_door\":{\"s\":\"on\",\"a\":{"
        "\"friendly_name\":\"Front door\",\"device_class\":\"door\"}},"
        "\"sensor.room_temperature\":{\"s\":\"21.5\",\"a\":{"
        "\"friendly_name\":\"Room temperature\","
        "\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\"}}}}"
    );
    bool restated_ok = restated != NULL &&
                       slate_ha_entities_bind(ids, id_count, &binding_changed) == ESP_OK &&
                       process_event(restated, false) == ESP_OK;
    cJSON_Delete(restated);
    LOCK();
    mapped_contact = restated_ok && normalize_entity(find_entity(ids[4]), &contact,
                                                     &contact_kind, &contact_available);
    mapped_sensor = restated_ok && normalize_entity(find_entity(ids[2]), &sensor,
                                                    &sensor_kind, &sensor_available);
    UNLOCK();
    CHECK(mapped_contact && contact_available &&
              contact.normalized_sensor.category == SLATE_CATEGORY_DOOR && mapped_sensor &&
              sensor.normalized_sensor.category == SLATE_CATEGORY_TEMPERATURE,
          "entities carry a category before the reconnect");

    CHECK(slate_ha_entities_prepare_subscription() == ESP_OK,
          "resubscribe marks bound entities unavailable again");
    LOCK();
    mapped_contact = normalize_entity(find_entity(ids[4]), &contact, &contact_kind,
                                      &contact_available);
    mapped_sensor = normalize_entity(find_entity(ids[2]), &sensor, &sensor_kind,
                                     &sensor_available);
    UNLOCK();
    CHECK(mapped_contact && !contact_available &&
              contact.normalized_sensor.category == SLATE_CATEGORY_DOOR &&
              strcmp(contact.normalized_sensor.text, "Open") == 0 && mapped_sensor &&
              !sensor_available &&
              sensor.normalized_sensor.category == SLATE_CATEGORY_TEMPERATURE,
          "a reconnect does not take a category away with the attributes");

    /* The other caller of clear_raw_attributes(): an entity Home Assistant no
     * longer has. The tile goes to a dash either way, and keeping the door is
     * what stops that dash from acquiring a question mark beside it. */
    cJSON *gone = cJSON_Parse("{\"r\":[\"binary_sensor.front_door\"]}");
    CHECK(gone != NULL && process_event(gone, false) == ESP_OK,
          "apply a removal of a bound contact");
    cJSON_Delete(gone);
    LOCK();
    mapped_contact = normalize_entity(find_entity(ids[4]), &contact, &contact_kind,
                                      &contact_available);
    UNLOCK();
    CHECK(mapped_contact && !contact_available &&
              contact.normalized_sensor.category == SLATE_CATEGORY_DOOR,
          "a removed entity keeps the category it had");

    CHECK(slate_ha_entities_bind(NULL, 0, &binding_changed) == ESP_OK && binding_changed,
          "empty set unsubscribes everything");

#undef CHECK
    ESP_LOGI(TAG, "selftest: %u failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif
