#include "provisioning.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "prov_store.h"
#include "prov_wifi.h"
#include "prov_portal.h"

static const char *TAG = "provisioning";

// How long to keep the SoftAP up after a confirmed app join so the phone can read the
// "connected" status before we reboot into station-only mode. This board has no Kconfig for the
// provisioning flow, so it is a local constant rather than a CONFIG_* option.
#define PROV_CONFIRM_GRACE_MS 9000

static const prov_options_t *s_opts;

// App-driven credential check (POST /api/provision): the pending credentials and an in-flight
// guard so a double submit does not spawn two connect tasks.
static prov_config_t s_pending;
static volatile bool s_app_connecting;

static void emit(prov_event_t event, const char *info)
{
    if (s_opts && s_opts->event_cb) {
        s_opts->event_cb(event, info, s_opts->user);
    }
}

static void reboot_task(void *arg)
{
    (void)arg;
    // Give the HTTP response time to flush to the browser before restarting.
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "restarting to apply new configuration");
    esp_restart();
}

static void on_portal_save(const prov_config_t *cfg, void *user)
{
    (void)user;
    if (!prov_store_save(cfg)) {
        // Persisting failed: do NOT reboot (that would just relaunch the empty
        // portal). Stay up so the user can retry the submission.
        ESP_LOGE(TAG, "save failed — staying in setup portal for retry");
        return;
    }
    emit(PROV_EVENT_CONFIG_SAVED, cfg->ssid);
    xTaskCreate(reboot_task, "prov_reboot", 2048, NULL, 5, NULL);
}

// Verify app-submitted credentials with a live station join WHILE the SoftAP stays up (so the
// app can poll GET /api/status), and only on success persist them (SSID/password + location)
// and reboot into station mode. Runs as its own task because prov_wifi_connect_keep_ap blocks
// for the connect timeout.
static void app_connect_task(void *arg)
{
    (void)arg;
    prov_config_t cfg = s_pending;
    uint32_t timeout_ms = s_opts ? s_opts->sta_connect_timeout_ms : 15000;

    bool joined = prov_wifi_connect_keep_ap(cfg.ssid, cfg.password, timeout_ms);
    if (joined && prov_store_save(&cfg)) {
        emit(PROV_EVENT_CONFIG_SAVED, cfg.ssid);
        prov_portal_set_status(PROV_PORTAL_CONNECTED, cfg.ssid, NULL);
        // Hold the SoftAP a moment so the phone reads "connected", then reboot into clean
        // station-only mode where the boot path reconnects with the saved credentials.
        ESP_LOGI(TAG, "credentials confirmed for '%s'; rebooting into station mode in %dms",
                 cfg.ssid, PROV_CONFIRM_GRACE_MS);
        vTaskDelay(pdMS_TO_TICKS(PROV_CONFIRM_GRACE_MS));
        ESP_LOGI(TAG, "restarting to apply confirmed configuration");
        esp_restart();
    } else {
        const char *reason = joined ? "save_failed" : "auth_failed";
        ESP_LOGW(TAG, "provision attempt for '%s' failed (%s)", cfg.ssid, reason);
        prov_portal_set_status(PROV_PORTAL_FAILED, cfg.ssid, reason);
        s_app_connecting = false;  // let the app retry with new credentials
    }
    vTaskDelete(NULL);
}

// Portal callback for POST /api/provision: start the async connect test, ignoring a duplicate
// submission while one is already running.
static void on_app_provision(const prov_config_t *cfg, void *user)
{
    (void)user;
    if (s_app_connecting) {
        ESP_LOGW(TAG, "provision already in progress — ignoring duplicate submit");
        return;
    }
    s_app_connecting = true;
    s_pending = *cfg;
    // Set CONNECTING only here — after the duplicate guard accepts — so a duplicate/retry submit
    // can never clobber a terminal (failed/connected) status with a "connecting" that has no task
    // driving it to a terminal state.
    prov_portal_set_status(PROV_PORTAL_CONNECTING, cfg->ssid, NULL);
    if (xTaskCreate(app_connect_task, "prov_connect", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start connect task");
        s_app_connecting = false;
        prov_portal_set_status(PROV_PORTAL_FAILED, cfg->ssid, "internal_error");
    }
}

static void ensure_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void provisioning_default_options(prov_options_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    strlcpy(opts->ap_ssid_prefix, "Obsidian Board", sizeof(opts->ap_ssid_prefix));
    opts->sta_connect_timeout_ms = 15000;
    opts->event_cb = NULL;
    opts->user = NULL;
}

bool provisioning_run(const prov_options_t *opts, prov_config_t *out)
{
    s_opts = opts;

    ensure_nvs();

    prov_config_t cfg;
    bool have_config = prov_store_load(&cfg);
    // One-shot escape hatch (KEY2 held 5s): skip the connect and go straight to the setup
    // portal, but keep the saved config so the portal pre-fills (the user only re-enters Wi-Fi).
    bool forced = prov_store_take_force_portal();

    prov_wifi_init();

    if (have_config && !forced) {
        ESP_LOGI(TAG, "stored network '%s' — attempting to connect", cfg.ssid);
        emit(PROV_EVENT_STA_CONNECTING, cfg.ssid);
        if (prov_wifi_connect(cfg.ssid, cfg.password, opts->sta_connect_timeout_ms)) {
            emit(PROV_EVENT_STA_CONNECTED, cfg.ssid);
            *out = cfg;
            return true;
        }
        ESP_LOGW(TAG, "could not join '%s' — falling back to setup portal", cfg.ssid);
    } else if (forced) {
        ESP_LOGI(TAG, "user forced setup mode (KEY2 long-press) — starting portal");
    } else {
        ESP_LOGI(TAG, "no stored network — starting setup portal");
    }

    char suffix[5];
    prov_wifi_mac_suffix(suffix);
    char ap_ssid[40];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s-%s", opts->ap_ssid_prefix, suffix);

    // Prime the scan cache while still STA-only (no client to disrupt), then bring up the AP.
    // The portal's /scan serves this cache; a live scan inside the request would leave the AP
    // channel and reset the connected phone's session. start_scanning also begins a periodic
    // background refresh that runs once the AP is up.
    prov_wifi_start_scanning();
    prov_wifi_start_ap(ap_ssid);
    // Identity for GET /api/info is derived internally (the public options carry no device id):
    // the MAC suffix uniquely names this board, and the model is the fixed product name.
    prov_portal_info_t info = {
        .device_id = suffix,
        .model = "Obsidian Board",
        .ap_ssid = ap_ssid,
    };
    prov_portal_start(have_config ? &cfg : NULL, on_portal_save, on_app_provision, &info, NULL);
    emit(PROV_EVENT_PORTAL_STARTED, ap_ssid);

    ESP_LOGI(TAG, "setup portal ready — join Wi-Fi '%s' and open http://192.168.4.1", ap_ssid);

    // Stay in setup mode; on_portal_save persists the config and reboots into station mode.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
