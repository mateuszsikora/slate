/* Slate — configured backlight schedule and inactivity policy. */

#include "slate_brightness.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "slate_display.h"
#include "slate_time.h"
#include "slate_touch.h"

static const char *TAG = "brightness";

#define BRIGHTNESS_TASK_STACK       4096
#define BRIGHTNESS_TASK_PRIORITY       3
#define BRIGHTNESS_CHECK_PERIOD_MS  1000
#define BRIGHTNESS_DEFAULT_LEVEL     100

typedef enum {
    TARGET_DAY,
    TARGET_NIGHT,
    TARGET_INACTIVITY,
    TARGET_SETUP,
} target_reason_t;

typedef struct {
    uint8_t day;
    uint8_t night;
    int night_start;
    int night_end;
    bool night_schedule;
    uint64_t screen_off_after_us;
    bool wake_on_touch;
} brightness_settings_t;

typedef struct {
    bool synced;
    bool setup_active;
    bool inactivity_blanked;
    int local_minute;
} policy_input_t;

typedef struct {
    uint8_t level;
    target_reason_t reason;
} policy_target_t;

typedef struct {
    bool consume;
    bool wake;
} touch_decision_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static atomic_bool s_ready = ATOMIC_VAR_INIT(false);
static brightness_settings_t s_settings;
static int64_t s_last_activity_us;
static bool s_inactivity_blanked;
static uint8_t s_last_target = UINT8_MAX;
static target_reason_t s_last_reason = TARGET_DAY;

static brightness_settings_t default_settings(void)
{
    return (brightness_settings_t) {
        .day = BRIGHTNESS_DEFAULT_LEVEL,
        .night = BRIGHTNESS_DEFAULT_LEVEL,
        .wake_on_touch = true,
    };
}

static uint8_t level_value(int value, const char *name)
{
    if (value < 0) {
        ESP_LOGW(TAG, "%s %d is below 0; using 0", name, value);
        return 0;
    }
    if (value > 100) {
        ESP_LOGW(TAG, "%s %d is above 100; using 100", name, value);
        return 100;
    }
    return (uint8_t) value;
}

static bool parse_clock(const char *text, int *minute)
{
    if (text == NULL || minute == NULL || strlen(text) != 5 || text[2] != ':' ||
        text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9' ||
        text[3] < '0' || text[3] > '9' || text[4] < '0' || text[4] > '9') {
        return false;
    }

    int hour = (text[0] - '0') * 10 + text[1] - '0';
    int min = (text[3] - '0') * 10 + text[4] - '0';
    if (hour > 23 || min > 59) {
        return false;
    }
    *minute = hour * 60 + min;
    return true;
}

static bool minute_is_night(int minute, int start, int end)
{
    if (start == end) {
        return false;
    }
    if (start < end) {
        return minute >= start && minute < end;
    }
    return minute >= start || minute < end;
}

static policy_target_t policy_target(const brightness_settings_t *settings,
                                     const policy_input_t *input)
{
    if (input->setup_active) {
        return (policy_target_t) {.level = 100, .reason = TARGET_SETUP};
    }
    if (input->inactivity_blanked) {
        return (policy_target_t) {.level = 0, .reason = TARGET_INACTIVITY};
    }
    if (settings->night_schedule && input->synced &&
        minute_is_night(input->local_minute, settings->night_start,
                        settings->night_end)) {
        return (policy_target_t) {.level = settings->night, .reason = TARGET_NIGHT};
    }
    return (policy_target_t) {.level = settings->day, .reason = TARGET_DAY};
}

static touch_decision_t touch_decision(bool dark, bool inactivity_blanked,
                                       bool wake_on_touch)
{
    return (touch_decision_t) {
        .consume = dark,
        .wake = dark && inactivity_blanked && wake_on_touch,
    };
}

static const char *reason_name(target_reason_t reason)
{
    switch (reason) {
    case TARGET_DAY:
        return "day";
    case TARGET_NIGHT:
        return "night";
    case TARGET_INACTIVITY:
        return "inactivity";
    case TARGET_SETUP:
        return "setup";
    }
    return "unknown";
}

static policy_input_t current_input_locked(bool setup_active)
{
    policy_input_t input = {
        .synced = slate_time_synced(),
        .setup_active = setup_active,
        .inactivity_blanked = s_inactivity_blanked,
    };
    if (input.synced) {
        time_t now = time(NULL);
        struct tm local;
        if (localtime_r(&now, &local) != NULL) {
            input.local_minute = local.tm_hour * 60 + local.tm_min;
        } else {
            input.synced = false;
        }
    }
    return input;
}

static esp_err_t apply_locked(bool setup_active)
{
    policy_input_t input = current_input_locked(setup_active);
    policy_target_t target = policy_target(&s_settings, &input);
    uint8_t expected = setup_active
                           ? 100
                           : slate_display_backlight_mode() == SLATE_DISPLAY_BACKLIGHT_ON_OFF
                                 ? (target.level == 0 ? 0 : 100)
                                 : target.level;
    esp_err_t err = ESP_OK;
    if (slate_display_brightness_level() != expected) {
        err = slate_display_brightness_set(target.level);
    }
    if (err != ESP_OK) {
        if (target.level != s_last_target || target.reason != s_last_reason) {
            ESP_LOGE(TAG, "%s target %u%%: %s", reason_name(target.reason),
                     (unsigned) target.level, esp_err_to_name(err));
        }
        return err;
    }

    if (target.level != s_last_target || target.reason != s_last_reason) {
        uint8_t actual = slate_display_brightness_level();
        ESP_LOGI(TAG, "%s target %u%% -> backlight %u%% (%s)",
                 reason_name(target.reason), (unsigned) target.level,
                 (unsigned) actual,
                 slate_display_backlight_mode() == SLATE_DISPLAY_BACKLIGHT_PWM
                     ? "pwm"
                     : "on/off");
        s_last_target = target.level;
        s_last_reason = target.reason;
    }
    return ESP_OK;
}

static void service_locked(void)
{
    int64_t now = esp_timer_get_time();
    bool setup_active = slate_display_setup_active();
    if (setup_active) {
        /* Suspension means setup time is not charged to the inactivity timer.
         * Resetting continuously is the simplest representation of that. */
        s_last_activity_us = now;
        s_inactivity_blanked = false;
    } else if (s_settings.screen_off_after_us > 0 &&
               (uint64_t) (now - s_last_activity_us) >=
                   s_settings.screen_off_after_us) {
        s_inactivity_blanked = true;
    }
    (void) apply_locked(setup_active);
}

static bool on_touch_press(void *ctx)
{
    (void) ctx;
    if (!atomic_load_explicit(&s_ready, memory_order_acquire) ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        /* A dark press is still consumed when the policy lock is unavailable:
         * an invisible provider action is worse than a delayed wake. */
        return slate_display_brightness_level() == 0;
    }

    bool dark = slate_display_brightness_level() == 0;
    touch_decision_t decision =
        touch_decision(dark, s_inactivity_blanked, s_settings.wake_on_touch);
    if (!dark) {
        s_last_activity_us = esp_timer_get_time();
    } else if (decision.wake) {
        s_last_activity_us = esp_timer_get_time();
        s_inactivity_blanked = false;
        (void) apply_locked(slate_display_setup_active());
        ESP_LOGI(TAG, "backlight wake requested by touch");
    }
    xSemaphoreGive(s_lock);

    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
    return decision.consume;
}

static void brightness_task(void *ctx)
{
    (void) ctx;
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BRIGHTNESS_CHECK_PERIOD_MS));
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            service_locked();
            xSemaphoreGive(s_lock);
        }
    }
}

esp_err_t slate_brightness_init(void)
{
    if (atomic_load_explicit(&s_ready, memory_order_acquire) ||
        s_lock != NULL || s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!slate_display_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_settings = default_settings();
    s_last_activity_us = esp_timer_get_time();
    s_inactivity_blanked = false;
    s_last_target = UINT8_MAX;

    if (xTaskCreate(brightness_task, "slate_bright", BRIGHTNESS_TASK_STACK, NULL,
                    BRIGHTNESS_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = slate_touch_set_press_observer(on_touch_press, NULL);
    if (err != ESP_OK) {
        vTaskDelete(s_task);
        s_task = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return err;
    }

    atomic_store_explicit(&s_ready, true, memory_order_release);
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        service_locked();
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "brightness policy ready; touch wake %s",
             slate_touch_ready() ? "attached" : "unavailable");
    return ESP_OK;
}

esp_err_t slate_brightness_configure(const slate_config_settings_t *settings)
{
    if (!atomic_load_explicit(&s_ready, memory_order_acquire) || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    brightness_settings_t next = default_settings();
    if (settings->has_brightness_day) {
        next.day = level_value(settings->brightness_day, "brightness_day");
    }
    if (settings->has_brightness_night) {
        next.night = level_value(settings->brightness_night, "brightness_night");
    }
    if (settings->has_screen_off_after && settings->screen_off_after > 0) {
        next.screen_off_after_us =
            (uint64_t) settings->screen_off_after * 60ULL * 1000000ULL;
    }
    if (settings->has_wake_on_touch) {
        next.wake_on_touch = settings->wake_on_touch;
    }

    bool has_start = settings->night_start != NULL;
    bool has_end = settings->night_end != NULL;
    if (has_start && has_end &&
        parse_clock(settings->night_start, &next.night_start) &&
        parse_clock(settings->night_end, &next.night_end) &&
        next.night_start != next.night_end) {
        next.night_schedule = true;
    } else if (has_start || has_end) {
        ESP_LOGW(TAG, "night schedule ignored: expected distinct HH:MM start and end");
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_settings = next;
    s_last_activity_us = esp_timer_get_time();
    s_inactivity_blanked = false;
    s_last_target = UINT8_MAX;
    esp_err_t err = apply_locked(slate_display_setup_active());
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "configured day %u%%, night %u%%, schedule %s, screen off %llu min, "
                  "wake on touch %s",
             (unsigned) next.day, (unsigned) next.night,
             next.night_schedule ? "enabled" : "disabled",
             (unsigned long long) (next.screen_off_after_us / (60ULL * 1000000ULL)),
             next.wake_on_touch ? "yes" : "no");
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
    return err;
}

#ifdef SLATE_BRIGHTNESS_SELFTEST

#define SELFTEST_CHECK(condition, label)                                  \
    do {                                                                  \
        bool passed_ = (condition);                                       \
        ESP_LOGI(TAG, "selftest: %-42s %s", label, passed_ ? "PASS" : "FAIL"); \
        failures += passed_ ? 0 : 1;                                     \
    } while (0)

esp_err_t slate_brightness_selftest(void)
{
    unsigned failures = 0;
    int minute = -1;
    SELFTEST_CHECK(parse_clock("00:00", &minute) && minute == 0,
                   "midnight parses");
    SELFTEST_CHECK(parse_clock("23:59", &minute) && minute == 1439,
                   "last minute parses");
    SELFTEST_CHECK(!parse_clock("24:00", &minute), "hour 24 rejected");
    SELFTEST_CHECK(!parse_clock("09:60", &minute), "minute 60 rejected");
    SELFTEST_CHECK(!parse_clock("9:00", &minute), "non-HH:MM clock rejected");

    SELFTEST_CHECK(minute_is_night(22 * 60 + 30, 22 * 60 + 30, 6 * 60 + 30),
                   "overnight start inclusive");
    SELFTEST_CHECK(minute_is_night(6 * 60 + 29, 22 * 60 + 30, 6 * 60 + 30),
                   "overnight last minute included");
    SELFTEST_CHECK(!minute_is_night(6 * 60 + 30, 22 * 60 + 30, 6 * 60 + 30),
                   "overnight end exclusive");
    SELFTEST_CHECK(minute_is_night(12 * 60, 9 * 60, 17 * 60),
                   "same-day window included");
    SELFTEST_CHECK(!minute_is_night(18 * 60, 9 * 60, 17 * 60),
                   "same-day outside excluded");
    SELFTEST_CHECK(!minute_is_night(12 * 60, 9 * 60, 9 * 60),
                   "equal boundaries disable schedule");

    brightness_settings_t settings = default_settings();
    settings.day = 80;
    settings.night = 15;
    settings.night_start = 22 * 60 + 30;
    settings.night_end = 6 * 60 + 30;
    settings.night_schedule = true;
    policy_input_t input = {.synced = true, .local_minute = 23 * 60};
    policy_target_t target = policy_target(&settings, &input);
    SELFTEST_CHECK(target.level == 15 && target.reason == TARGET_NIGHT,
                   "synced night uses night level");
    input.local_minute = 12 * 60;
    target = policy_target(&settings, &input);
    SELFTEST_CHECK(target.level == 80 && target.reason == TARGET_DAY,
                   "synced daytime uses day level");
    input.synced = false;
    input.local_minute = 23 * 60;
    target = policy_target(&settings, &input);
    SELFTEST_CHECK(target.level == 80 && target.reason == TARGET_DAY,
                   "unsynced clock cannot enter night mode");
    input.setup_active = true;
    input.inactivity_blanked = true;
    target = policy_target(&settings, &input);
    SELFTEST_CHECK(target.level == 100 && target.reason == TARGET_SETUP,
                   "setup outranks schedule and inactivity");
    input.setup_active = false;
    target = policy_target(&settings, &input);
    SELFTEST_CHECK(target.level == 0 && target.reason == TARGET_INACTIVITY,
                   "inactivity blanks outside setup");

    touch_decision_t touch = touch_decision(true, true, true);
    SELFTEST_CHECK(touch.consume && touch.wake,
                   "first dark inactivity touch wakes and consumes");
    touch = touch_decision(true, true, false);
    SELFTEST_CHECK(touch.consume && !touch.wake,
                   "wake-disabled dark touch is still consumed");
    touch = touch_decision(true, false, true);
    SELFTEST_CHECK(touch.consume && !touch.wake,
                   "scheduled-off touch does not override schedule");
    touch = touch_decision(false, false, true);
    SELFTEST_CHECK(!touch.consume && !touch.wake,
                   "visible touch reaches the dashboard");

    ESP_LOGI(TAG, "selftest: %u failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif
