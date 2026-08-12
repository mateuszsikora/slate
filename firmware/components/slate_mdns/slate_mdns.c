/*
 * Slate — mDNS advertisement. See include/slate_mdns.h.
 *
 * design.md §4.3 (the name), §9.2 (it exists on the access point as well),
 * §10 (what the editor does with it).
 */

#include "slate_mdns.h"

#include <stdatomic.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "mdns.h"

#include "slate_api.h"
#include "slate_store.h"

static const char *TAG = "mdns";

static atomic_bool s_ready;

esp_err_t slate_mdns_init(void)
{
    if (atomic_load(&s_ready)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "responder unavailable: %s", esp_err_to_name(err));
        return err;
    }
    atomic_store(&s_ready, true);

    /* §9.2: the SSID, this name and the device name carry the same suffix, so
     * one panel is called one thing everywhere. */
    const char *name = slate_store_device_name();
    err = mdns_hostname_set(name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "claiming %s.local: %s", name, esp_err_to_name(err));
        return err;
    }
    /* The instance name is what a browsing client lists the panel as. Not
     * fatal if it does not take — the name above is what resolves. */
    esp_err_t instance_err = mdns_instance_name_set(name);
    if (instance_err != ESP_OK) {
        ESP_LOGW(TAG, "setting the instance name: %s", esp_err_to_name(instance_err));
    }

    /*
     * The service is what makes the panel visible to something that is
     * browsing rather than resolving a name it already knows — a second panel
     * on the same network is §16's "multiple panels" question, and a browser
     * that lists them needs a record to list. The TXT keys carry what §4.1's
     * `/info` would answer, so a discovery pass does not have to open a
     * connection to tell two models apart.
     */
    const esp_app_desc_t *app = esp_app_get_description();
    /* Not const: mdns_service_add() takes a mutable pointer even though it only
     * copies what it is given. */
    mdns_txt_item_t txt[] = {
        {"model", SLATE_API_MODEL_ID},
        {"name", name},
        {"api", SLATE_API_BASE_PATH},
        {"fw", app != NULL ? app->version : ""},
    };
    err = mdns_service_add(NULL, "_http", "_tcp", 80, txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "advertising _http._tcp: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "advertising http://%s.local", name);
    return ESP_OK;
}

bool slate_mdns_ready(void)
{
    return atomic_load(&s_ready);
}
