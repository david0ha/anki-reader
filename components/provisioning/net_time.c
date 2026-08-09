/*
 * net_time.c — one-shot SNTP sync. See net_time.h.
 *
 * The station/SoftAP lifecycle lives in the `provisioning` component; by the
 * time this runs we already have an IP, so we only kick SNTP and wait.
 */
#include "net_time.h"

#include "freertos/FreeRTOS.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"

static const char *TAG = "net_time";

void net_time_sync(int timeout_ms) {
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed");
        return;
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) != ESP_OK)
        ESP_LOGW(TAG, "sntp sync timeout (news dates may be off)");
    else
        ESP_LOGI(TAG, "time synced");

    // One-shot: tear the client down so it doesn't keep a socket + poll timer
    // alive for the device's whole uptime. RTC drift over our cadence is negligible.
    esp_netif_sntp_deinit();
}
