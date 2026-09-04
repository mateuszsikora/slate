/*
 * Slate — clock. See include/slate_time.h for the contract.
 *
 * DESIGN.md §3.3, §6.1, §9.4.
 */

#include "slate_time.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "slate_wifi.h"

static const char *TAG = "time";

/*
 * The fallback pool, used only when the router does not offer an NTP server
 * over DHCP — see slate_time_init(). A LAN with no route to the internet is a
 * configuration this project has to work on (§2 puts Home Assistant on the
 * same LAN, and nothing else needs the outside world), so DHCP comes first and
 * this is the backstop rather than the other way round.
 */
#define FALLBACK_NTP_SERVER "pool.ntp.org"

static char s_zone[SLATE_TIME_ZONE_MAX_LEN] = SLATE_TIME_DEFAULT_ZONE;
static atomic_bool s_synced = ATOMIC_VAR_INIT(false);
static StaticSemaphore_t s_zone_lock_storage;
static SemaphoreHandle_t s_zone_lock;

static void zone_lock(void)
{
    if (s_zone_lock != NULL) {
        xSemaphoreTake(s_zone_lock, portMAX_DELAY);
    }
}

static void zone_unlock(void)
{
    if (s_zone_lock != NULL) {
        xSemaphoreGive(s_zone_lock);
    }
}

/* --- The generated zone table ------------------------------------------- */

typedef struct {
    const char *iana;
    const char *posix;
} zone_t;

static const zone_t ZONES[] = {
#include "slate_time_zones.inc"
};

#define ZONE_COUNT (sizeof(ZONES) / sizeof(ZONES[0]))

/*
 * Binary search, because the table is 599 entries and this runs on every
 * `PUT /config` (#24) — a linear scan of 599 strcmp() calls inside the UI
 * rebuild is the kind of cost that is invisible until the editor's live
 * preview does it on every drag.
 */
static const char *posix_for(const char *iana)
{
    size_t lo = 0;
    size_t hi = ZONE_COUNT;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(iana, ZONES[mid].iana);
        if (cmp == 0) {
            return ZONES[mid].posix;
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return NULL;
}

/* --- Timezone ----------------------------------------------------------- */

esp_err_t slate_time_set_timezone(const char *iana)
{
    if (iana == NULL || iana[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(iana) >= sizeof(s_zone)) {
        return ESP_ERR_NOT_FOUND; /* too long to be in the table */
    }

    const char *posix = posix_for(iana);
    if (posix == NULL) {
        ESP_LOGW(TAG, "unknown timezone \"%s\" — keeping the previous timezone", iana);
        return ESP_ERR_NOT_FOUND;
    }

    /* setenv copies, and tzset() parses into newlib's own state. Serialize the
     * process-wide replacement with every localtime conversion once the clock
     * component has initialized its static lock. */
    zone_lock();
    if (strcmp(iana, s_zone) == 0) {
        zone_unlock();
        return ESP_OK;
    }
    if (setenv("TZ", posix, 1) != 0) {
        zone_unlock();
        return ESP_ERR_NO_MEM;
    }
    tzset();

    strlcpy(s_zone, iana, sizeof(s_zone));
    zone_unlock();
    ESP_LOGI(TAG, "timezone %s (%s)", iana, posix);
    return ESP_OK;
}

const char *slate_time_timezone(void)
{
    return s_zone;
}

bool slate_time_synced(void)
{
    return atomic_load_explicit(&s_synced, memory_order_acquire);
}

bool slate_time_localtime(time_t value, struct tm *out)
{
    if (out == NULL) {
        return false;
    }
    zone_lock();
    bool converted = localtime_r(&value, out) != NULL;
    zone_unlock();
    return converted;
}

/* --- SNTP --------------------------------------------------------------- */

static void on_sync(struct timeval *tv)
{
    (void) tv;

    char stamp[32];
    char zone[SLATE_TIME_ZONE_MAX_LEN];
    time_t now = time(NULL);
    struct tm tm;
    zone_lock();
    bool converted = localtime_r(&now, &tm) != NULL;
    strlcpy(zone, s_zone, sizeof(zone));
    zone_unlock();
    if (converted) {
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    } else {
        strlcpy(stamp, "unknown", sizeof(stamp));
    }

    atomic_store_explicit(&s_synced, true, memory_order_release);

    ESP_LOGI(TAG, "clock synced: %s %s", stamp, zone);
}

static void on_station_up(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) id;
    (void) data;

    /*
     * Restarted on every association rather than only the first. lwip's SNTP
     * client keeps its poll interval and its idea of which server is reachable
     * across an outage, and a panel that comes back after a two-hour router
     * failure should ask immediately rather than at the end of whatever
     * interval it had backed off to. esp_netif_sntp_start() stops and
     * re-initialises the client, which is exactly that.
     */
    esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "starting SNTP: %s", esp_err_to_name(err));
    }
}

esp_err_t slate_time_init(void)
{
    if (s_zone_lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_zone_lock = xSemaphoreCreateMutexStatic(&s_zone_lock_storage);
    if (s_zone_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* The UI runtime may have loaded §3.3's persisted timezone before the
     * station starts. Preserve that choice instead of resetting it to UTC
     * during network bring-up; UTC remains the initial value when no config
     * supplied one. */
    const char *posix = posix_for(s_zone);
    zone_lock();
    if (setenv("TZ", posix != NULL ? posix : "UTC0", 1) != 0) {
        zone_unlock();
        return ESP_ERR_NO_MEM;
    }
    tzset();
    zone_unlock();

    /*
     * `server_from_dhcp` puts the router's NTP server in slot 0 and moves the
     * pool to slot 1, which is what `index_of_first_server` means here — the
     * renew handler rewrites the configured servers above the DHCP one after
     * every lease. Needs CONFIG_LWIP_DHCP_GET_NTP_SRV and room for two
     * servers; both are in sdkconfig.defaults next to this comment's twin.
     */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(FALLBACK_NTP_SERVER);
    cfg.start = false; /* §9.4: there is no route to a time server in setup mode */
    cfg.wait_for_sync = false;
    cfg.server_from_dhcp = true;
    cfg.renew_servers_after_new_IP = true;
    cfg.index_of_first_server = 1;
    cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    cfg.sync_cb = on_sync;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(SLATE_WIFI_EVENT, SLATE_WIFI_EVENT_CONNECTED,
                                              on_station_up, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* Ordering against slate_wifi_init() is not the caller's problem: a
     * station that associated before this subscription existed would otherwise
     * never start the clock, and the failure would only show on a device fast
     * enough to associate inside app_main. */
    slate_wifi_status_t status;
    slate_wifi_status(&status);
    if (status.connected) {
        on_station_up(NULL, SLATE_WIFI_EVENT, SLATE_WIFI_EVENT_CONNECTED, NULL);
    }

    return ESP_OK;
}
