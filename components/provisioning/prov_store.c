#include "prov_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define PROV_NVS_NS "prov"

static const char *TAG = "prov_store";

// Keys written by the stock-ticker firmware this device may have been running
// before. "fh_key" and "fmp_key" held live API secrets, so they are erased on
// the next save rather than left sitting in flash for a feature that no longer
// exists. Harmless to erase when absent.
static const char *const kObsoleteKeys[] = { "tickers", "fh_key", "fmp_key", "econ_url" };

bool prov_store_load(prov_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;  // namespace not created yet -> nothing saved
    }

    size_t len = sizeof(cfg->ssid);
    nvs_get_str(h, "ssid", cfg->ssid, &len);

    len = sizeof(cfg->password);
    nvs_get_str(h, "pass", cfg->password, &len);

    len = sizeof(cfg->study_url);
    nvs_get_str(h, "vurl", cfg->study_url, &len);

    nvs_close(h);
    return cfg->ssid[0] != '\0';
}

bool prov_store_save(const prov_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PROV_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    nvs_set_str(h, "ssid", cfg->ssid);
    nvs_set_str(h, "pass", cfg->password);
    nvs_set_str(h, "vurl", cfg->study_url);

    for (size_t i = 0; i < sizeof(kObsoleteKeys) / sizeof(kObsoleteKeys[0]); i++) {
        nvs_erase_key(h, kObsoleteKeys[i]);   // ESP_ERR_NVS_NOT_FOUND is fine
    }

    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "saved ssid='%s' study_url='%s'", cfg->ssid, cfg->study_url);
    return true;
}

void prov_store_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

void prov_store_set_force_portal(void)
{
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "force_ap", 1);
    nvs_commit(h);
    nvs_close(h);
}

bool prov_store_take_force_portal(void)
{
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    nvs_get_u8(h, "force_ap", &v);
    if (v != 0) {
        nvs_erase_key(h, "force_ap");  // one-shot: consume so the next reboot connects normally
        nvs_commit(h);
    }
    nvs_close(h);
    return v != 0;
}
