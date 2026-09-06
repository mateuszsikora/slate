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
#include "slate_store.h"
#include "slate_time.h"
#include "slate_touch.h"

static const char *TAG = "brightness";

#define BRIGHTNESS_TASK_STACK       4096
#define BRIGHTNESS_TASK_PRIORITY       3
#define BRIGHTNESS_CHECK_PERIOD_MS  1000
#define BRIGHTNESS_DEFAULT_LEVEL     100
#define BRIGHTNESS_ERROR_LOG_PERIOD_US (60LL * 1000000LL)

/*
 * How long the level a previous run left behind stands while the clock has not
 * come back. Two minutes is far longer than a station join and an SNTP exchange
 * take, and short enough that a panel with no route to a time server — a week
 * in setup mode, §9 — is not left on a level nothing can change: after it, the
 * configured day brightness applies and the schedule behaves as it always did.
 */
#define BRIGHTNESS_BOOT_HOLD_US (120LL * 1000000LL)

/* A store that will not take the level is retried on this period rather than on
 * the policy's own, which would put a flash write a second behind the lock a
 * touch waits 100 ms for. Long enough that a failing NVS is not a load, short
 * enough that a transient one heals within a day/night transition. */
#define BRIGHTNESS_WRITE_RETRY_US (60LL * 1000000LL)

typedef enum {
    TARGET_DAY,
    TARGET_NIGHT,
    TARGET_BOOT,
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
    bool boot_hold;
    uint8_t boot_level;
    int local_minute;
} policy_input_t;

typedef struct {
    uint8_t level;
    target_reason_t reason;
} policy_target_t;

typedef struct {
    bool consume;
    bool wake;
    bool record_activity;
    bool clear_inactivity;
} touch_decision_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static atomic_bool s_ready = ATOMIC_VAR_INIT(false);
static brightness_settings_t s_settings;
static bool s_configured;
static uint8_t s_boot_level;
static bool s_boot_level_known;
static int64_t s_boot_hold_until_us;
static bool s_boot_level_write_failed;
static int64_t s_boot_level_retry_at_us;
static int64_t s_last_activity_us;
static bool s_inactivity_blanked;
static uint8_t s_last_target = UINT8_MAX;
static target_reason_t s_last_reason = TARGET_DAY;
static esp_err_t s_last_apply_error = ESP_OK;
static uint8_t s_last_failed_target = UINT8_MAX;
static target_reason_t s_last_failed_reason = TARGET_DAY;
static int64_t s_last_error_log_us;

static brightness_settings_t default_settings(void)
{
    return (brightness_settings_t) {
        .day = BRIGHTNESS_DEFAULT_LEVEL,
        .night = BRIGHTNESS_DEFAULT_LEVEL,
        .wake_on_touch = true,
    };
}

/*
 * The level a boot lights at, which is the level the last run ended on rather
 * than a guess about the time. It has to survive a reboot because none of the
 * three things that would let the policy work it out are available yet — the
 * panel comes up before the configuration is read and long before SNTP answers
 * — and a remembered fact is the only honest answer available in that window.
 *
 * Zero is never remembered. A stored zero is a panel that boots dark, and on
 * one whose clock never syncs it stays dark, which is the failure §9 exists to
 * remove; `brightness_night: 0` therefore comes up at the last lit level and
 * blanks a moment later, from a state somebody watching has seen work.
 */
static uint8_t boot_level(void)
{
    if (!s_boot_level_known) {
        uint32_t stored = 0;
        s_boot_level = slate_store_u32_get(SLATE_KEY_BACKLIGHT, &stored) == ESP_OK &&
                               stored > 0 && stored <= 100
                           ? (uint8_t) stored
                           : BRIGHTNESS_DEFAULT_LEVEL;
        s_boot_level_known = true;
    }
    return s_boot_level;
}

static void remember_boot_level(uint8_t level)
{
    if (level == 0 || level == boot_level()) {
        return;
    }

    /* Twice a day and on a configuration edit, so the flash write is rare
     * enough to make on the policy lock: the touch path's own 100 ms timeout
     * covers the milliseconds it costs, and a deferred-write path would exist
     * for no other reason. That argument only holds while the write succeeds —
     * a store that refuses would otherwise be retried on the policy's own
     * one-second period, which is how a rare write becomes a slow lock a touch
     * is waiting behind. So a failure backs off. */
    int64_t now = esp_timer_get_time();
    if (s_boot_level_write_failed && now < s_boot_level_retry_at_us) {
        return;
    }

    esp_err_t err = slate_store_u32_set(SLATE_KEY_BACKLIGHT, level);
    if (err != ESP_OK) {
        if (!s_boot_level_write_failed) {
            ESP_LOGW(TAG, "remembering %u%% for the next boot: %s — boots will light at %u%%",
                     (unsigned) level, esp_err_to_name(err), (unsigned) s_boot_level);
        }
        s_boot_level_write_failed = true;
        s_boot_level_retry_at_us = now + BRIGHTNESS_WRITE_RETRY_US;
        return;
    }
    s_boot_level_write_failed = false;
    s_boot_level = level;
}

static bool settings_equal(const brightness_settings_t *left,
                           const brightness_settings_t *right)
{
    return left->day == right->day && left->night == right->night &&
           left->night_start == right->night_start &&
           left->night_end == right->night_end &&
           left->night_schedule == right->night_schedule &&
           left->screen_off_after_us == right->screen_off_after_us &&
           left->wake_on_touch == right->wake_on_touch;
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

/*
 * Whether the level a previous run left behind is still the best answer there
 * is. Three conditions, and each is a different way of saying the policy cannot
 * do better yet: the clock has not come back, the schedule it would serve is
 * either unknown or has a night window in it, and the wait has not gone on long
 * enough to be a panel that will never be told the time. A configuration
 * without a night window ends it immediately — there is one level in that
 * document and no uncertainty about which one applies.
 */
static bool boot_hold_active(bool synced, bool configured, bool night_schedule,
                             int64_t now_us, int64_t until_us)
{
    return !synced && (!configured || night_schedule) && now_us < until_us;
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
    /* An unsynced clock still may not decide it is night — slate_time.c is
     * explicit about that, and a week in setup mode must not dim the panel. So
     * this does not infer anything from the time: it keeps the level the last
     * run ended on until the clock can answer, which is the difference between
     * remembering and guessing. Once a configuration without a night schedule
     * has arrived there is nothing to be uncertain about, and the day level is
     * the only level there is. */
    if (input->boot_hold) {
        return (policy_target_t) {.level = input->boot_level, .reason = TARGET_BOOT};
    }
    return (policy_target_t) {.level = settings->day, .reason = TARGET_DAY};
}

static touch_decision_t touch_decision(bool dark, bool inactivity_blanked,
                                       bool wake_on_touch, uint8_t resume_level)
{
    bool policy_keeps_dark = dark && resume_level == 0;
    bool wake = dark && inactivity_blanked && wake_on_touch && resume_level > 0;
    return (touch_decision_t) {
        .consume = dark,
        .wake = wake,
        .record_activity = !dark || policy_keeps_dark || wake,
        .clear_inactivity = policy_keeps_dark || wake,
    };
}

static const char *reason_name(target_reason_t reason)
{
    switch (reason) {
    case TARGET_DAY:
        return "day";
    case TARGET_NIGHT:
        return "night";
    case TARGET_BOOT:
        return "boot";
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
        .boot_level = boot_level(),
    };
    if (input.synced) {
        time_t now = time(NULL);
        struct tm local;
        if (slate_time_localtime(now, &local)) {
            input.local_minute = local.tm_hour * 60 + local.tm_min;
        } else {
            input.synced = false;
        }
    }
    /* Before a configuration arrives the settings are the defaults, and their
     * day of 100 % is exactly the value this issue is about — so the hold
     * covers that window too, not only a configured night schedule. */
    input.boot_hold = boot_hold_active(input.synced, s_configured,
                                       s_settings.night_schedule,
                                       esp_timer_get_time(), s_boot_hold_until_us);
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
        int64_t now = esp_timer_get_time();
        bool changed = err != s_last_apply_error ||
                       target.level != s_last_failed_target ||
                       target.reason != s_last_failed_reason;
        if (changed || now - s_last_error_log_us >= BRIGHTNESS_ERROR_LOG_PERIOD_US) {
            ESP_LOGE(TAG, "%s target %u%%: %s", reason_name(target.reason),
                     (unsigned) target.level, esp_err_to_name(err));
            s_last_error_log_us = now;
        }
        s_last_apply_error = err;
        s_last_failed_target = target.level;
        s_last_failed_reason = target.reason;
        return err;
    }
    s_last_apply_error = ESP_OK;

    /* Only the two scheduled reasons are worth carrying into the next boot.
     * Inactivity is a level a person is not looking at, and §9.4's setup 100 %
     * is a card rather than a brightness — a panel that rebooted out of either
     * should come back to the schedule, not to the state that interrupted it. */
    if (target.reason == TARGET_DAY || target.reason == TARGET_NIGHT) {
        remember_boot_level(target.level);
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
    policy_input_t resume_input = current_input_locked(slate_display_setup_active());
    resume_input.inactivity_blanked = false;
    policy_target_t resume_target = policy_target(&s_settings, &resume_input);
    touch_decision_t decision = touch_decision(dark, s_inactivity_blanked,
                                               s_settings.wake_on_touch,
                                               resume_target.level);
    if (decision.record_activity) {
        s_last_activity_us = esp_timer_get_time();
    }
    if (decision.clear_inactivity) {
        s_inactivity_blanked = false;
        esp_err_t err = apply_locked(slate_display_setup_active());
        if (decision.wake) {
            if (err == ESP_OK && slate_display_brightness_level() > 0) {
                ESP_LOGI(TAG, "backlight woken by touch");
            } else {
                ESP_LOGW(TAG, "touch wake deferred; backlight remains off");
            }
        }
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
    s_configured = false;
    s_boot_hold_until_us = esp_timer_get_time() + BRIGHTNESS_BOOT_HOLD_US;
    /* Read here rather than in the log line below, which runs after the task
     * that owns this state has started. */
    uint8_t booted_at = boot_level();
    s_last_activity_us = esp_timer_get_time();
    s_inactivity_blanked = false;
    s_last_target = UINT8_MAX;
    s_last_apply_error = ESP_OK;
    s_last_failed_target = UINT8_MAX;
    s_last_error_log_us = 0;

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
    ESP_LOGI(TAG,
             "brightness policy ready; %u%% stands until the clock syncs, a configuration "
             "without a night window arrives, or %d s pass; touch wake %s",
             (unsigned) booted_at, (int) (BRIGHTNESS_BOOT_HOLD_US / 1000000LL),
             slate_touch_ready() ? "attached" : "unavailable");
    return ESP_OK;
}

uint8_t slate_brightness_boot_level(void)
{
    return boot_level();
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
    bool changed = !s_configured || !settings_equal(&s_settings, &next);
    s_settings = next;
    s_configured = true;
    s_last_activity_us = esp_timer_get_time();
    s_inactivity_blanked = false;
    esp_err_t err = apply_locked(slate_display_setup_active());
    xSemaphoreGive(s_lock);

    if (changed) {
        ESP_LOGI(TAG, "configured day %u%%, night %u%%, schedule %s, screen off %llu min, "
                      "wake on touch %s",
                 (unsigned) next.day, (unsigned) next.night,
                 next.night_schedule ? "enabled" : "disabled",
                 (unsigned long long) (next.screen_off_after_us / (60ULL * 1000000ULL)),
                 next.wake_on_touch ? "yes" : "no");
    }
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
    input.boot_hold = true;
    input.boot_level = 45;
    target = policy_target(&settings, &input);
    SELFTEST_CHECK(target.level == 45 && target.reason == TARGET_BOOT,
                   "unsynced boot keeps the remembered level");
    /* The hold is only ever in force while the clock is unsynced, so the two
     * checks that follow are the pair that matters: a clock that works outranks
     * what was remembered, and the setup card and the inactivity timer below
     * outrank it as well — those two run with the hold still set. */
    input.synced = true;
    target = policy_target(&settings, &input);
    SELFTEST_CHECK(target.level == 15 && target.reason == TARGET_NIGHT,
                   "a working clock outranks the remembered level");
    input.synced = false;

    const int64_t until = 120 * 1000000LL;
    SELFTEST_CHECK(boot_hold_active(false, false, false, 0, until),
                   "an unconfigured panel holds its boot level");
    SELFTEST_CHECK(boot_hold_active(false, true, true, 0, until),
                   "a configured night schedule holds it too");
    SELFTEST_CHECK(!boot_hold_active(false, true, false, 0, until),
                   "a configuration without a night window ends the hold");
    SELFTEST_CHECK(!boot_hold_active(true, true, true, 0, until),
                   "a synced clock ends the hold");
    SELFTEST_CHECK(!boot_hold_active(false, false, false, until, until),
                   "the hold expires rather than waiting for ever");

    input.setup_active = true;
    input.inactivity_blanked = true;
    target = policy_target(&settings, &input);
    SELFTEST_CHECK(target.level == 100 && target.reason == TARGET_SETUP,
                   "setup outranks schedule and inactivity");
    input.setup_active = false;
    target = policy_target(&settings, &input);
    SELFTEST_CHECK(target.level == 0 && target.reason == TARGET_INACTIVITY,
                   "inactivity blanks outside setup");

    touch_decision_t touch = touch_decision(true, true, true, 80);
    SELFTEST_CHECK(touch.consume && touch.wake && touch.record_activity &&
                       touch.clear_inactivity,
                   "first dark inactivity touch wakes and consumes");
    touch = touch_decision(true, true, false, 80);
    SELFTEST_CHECK(touch.consume && !touch.wake && !touch.record_activity &&
                       !touch.clear_inactivity,
                   "wake-disabled dark touch is still consumed");
    touch = touch_decision(true, false, true, 0);
    SELFTEST_CHECK(touch.consume && !touch.wake && touch.record_activity &&
                       touch.clear_inactivity,
                   "scheduled-off touch records activity without waking");
    touch = touch_decision(true, true, true, 0);
    SELFTEST_CHECK(touch.consume && !touch.wake && touch.record_activity &&
                       touch.clear_inactivity,
                   "schedule-off outranks overlapping inactivity");
    touch = touch_decision(false, false, true, 80);
    SELFTEST_CHECK(!touch.consume && !touch.wake && touch.record_activity &&
                       !touch.clear_inactivity,
                   "visible touch reaches the dashboard");

    ESP_LOGI(TAG, "selftest: %u failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif
