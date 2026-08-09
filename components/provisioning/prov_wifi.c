#include "prov_wifi.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_log.h"

#define CONNECTED_BIT BIT0
#define FAIL_BIT      BIT1
#define MAX_RETRY     6
#define SCAN_PERIOD_US (20 * 1000 * 1000)   // re-scan every 20s while the portal is up
#define PROV_AP_CHANNEL 1                    // SoftAP channel (single radio shares it with STA)

static const char *TAG = "prov_wifi";

static EventGroupHandle_t s_events;
// s_connecting: a bounded initial join (prov_wifi_connect) is in progress.
// s_online:     we have held an IP at least once, so later drops must reconnect forever.
// Both are touched by the Wi-Fi event task and the caller; volatile keeps reads/writes
// visible across them, as does s_retries.
static volatile bool s_connecting;
static volatile bool s_online;
static volatile int  s_retries;

// Background-scan cache. The captive portal's /scan returns this cache instead of scanning
// live: on a single radio a blocking scan inside the request would leave the AP channel and
// reset the connected client's TCP session (the "Scanning…" hang). A non-blocking scan runs
// on a timer, and WIFI_EVENT_SCAN_DONE publishes results here under s_scan_mtx — the same
// pattern xiaozhi's esp-wifi-connect uses. Written by the Wi-Fi event task, read by httpd.
#define SCAN_CACHE_MAX 24
static prov_ap_t          s_scan_cache[SCAN_CACHE_MAX];
static size_t             s_scan_cache_count;
static SemaphoreHandle_t  s_scan_mtx;
static esp_timer_handle_t s_scan_timer;
static volatile bool      s_scanning;       // periodic background scan active (portal mode)
static volatile bool      s_scan_inflight;  // a non-blocking scan is awaiting SCAN_DONE
static volatile int       s_ap_clients;     // stations currently joined to our SoftAP

// Pull the just-completed scan's records into the cache. Runs in the Wi-Fi
// event task, whose stack is only ~2.3 KB — a prov_ap_t[SCAN_CACHE_MAX] local
// here once overflowed it and rebooted the board mid-provisioning. So the
// records go to the heap and the cache is filled in place under the mutex;
// the critical section is two dozen strlcpys, not a driver call.
static void collect_scan_results(void)
{
    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);

    wifi_ap_record_t *recs = NULL;
    uint16_t got = 0;
    if (found > 0) {
        recs = calloc(found, sizeof(*recs));
        if (recs) {
            got = found;
            esp_wifi_scan_get_ap_records(&got, recs);
        } else {
            esp_wifi_clear_ap_list();  // OOM: still free the driver's internal list
        }
    } else {
        esp_wifi_clear_ap_list();
    }

    size_t n = 0;
    if (s_scan_mtx && xSemaphoreTake(s_scan_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (uint16_t i = 0; i < got && n < SCAN_CACHE_MAX; i++) {
            if (recs[i].ssid[0] == '\0') {
                continue;  // hidden / empty SSID
            }
            strlcpy(s_scan_cache[n].ssid, (const char *)recs[i].ssid, sizeof(s_scan_cache[n].ssid));
            s_scan_cache[n].rssi = recs[i].rssi;
            s_scan_cache[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
            n++;
        }
        s_scan_cache_count = n;
        xSemaphoreGive(s_scan_mtx);
    }
    free(recs);
    ESP_LOGI(TAG, "scan complete: %u network(s) cached", (unsigned)n);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base != WIFI_EVENT) {
        return;
    }
    if (id == WIFI_EVENT_SCAN_DONE) {
        // Only react to our own async sweeps; the blocking prime collects inline.
        if (s_scan_inflight) {
            s_scan_inflight = false;
            collect_scan_results();
            if (s_scanning) {
                esp_timer_start_once(s_scan_timer, SCAN_PERIOD_US);  // schedule the next sweep
            }
        }
        return;
    }
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
        ESP_LOGI(TAG, "AP client joined (%d connected) — pausing background scans", s_ap_clients);
        return;
    }
    if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) s_ap_clients--;
        ESP_LOGI(TAG, "AP client left (%d connected)", s_ap_clients);
        return;
    }
    if (id != WIFI_EVENT_STA_DISCONNECTED) {
        return;
    }
    if (s_connecting) {
        // Initial join: retry a bounded number of times, then give up so the
        // caller can fall back to the setup portal.
        if (s_retries < MAX_RETRY) {
            s_retries++;
            ESP_LOGI(TAG, "retry %d/%d", s_retries, MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, FAIL_BIT);
        }
    } else if (s_online) {
        // A drop after we were connected: reconnect indefinitely. This restores
        // the always-reconnect behaviour the app relies on for unattended uptime.
        ESP_LOGW(TAG, "link dropped, reconnecting");
        esp_wifi_connect();
    }
    // Otherwise we are idle / in AP-only setup mode: ignore.
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_connecting = false;  // the bounded initial attempt is satisfied
        s_online = true;       // from now on, drops trigger persistent reconnect
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

void prov_wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // We persist credentials ourselves in NVS, so keep the driver's own copy in RAM only.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Create the event group + scan mutex before registering handlers so an early
    // event can never dereference them while NULL.
    s_events = xEventGroupCreate();
    s_scan_mtx = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL));

    // Start in station mode; configs are applied per-call below.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Shared join logic. When `keep_ap` is false the radio is switched to station-only mode (the
// boot path). When true the current mode is left as-is so a running SoftAP survives the join
// (the app-driven credential check); on failure the SoftAP channel is restored for a retry.
static bool connect_impl(const char *ssid, const char *password, uint32_t timeout_ms,
                         bool keep_ap)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, password ? password : "", sizeof(wc.sta.password));
    // Accept any network from open upward (threshold is a *minimum* authmode).
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;

    if (!keep_ap) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    s_retries = 0;
    s_connecting = true;
    xEventGroupClearBits(s_events, CONNECTED_BIT | FAIL_BIT);

    ESP_LOGI(TAG, "connecting to '%s'%s", ssid, keep_ap ? " (SoftAP kept up)" : "");
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        s_events, CONNECTED_BIT | FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    bool ok = (bits & CONNECTED_BIT) != 0;
    // A GOT_IP can race in right as the wait times out; re-read before giving up
    // so we don't tear down a link that just succeeded.
    if (!ok && (xEventGroupGetBits(s_events) & CONNECTED_BIT)) {
        ok = true;
    }
    // Channel asymmetry (single radio): on SUCCESS the SoftAP has followed the target AP onto its
    // channel and we deliberately do NOT pull it back — re-pinning PROV_AP_CHANNEL would break the
    // just-established STA link. So a phone parked on channel 1 may lose the SoftAP and never read
    // the "connected" status; the companion app treats post-202 AP loss as probable success and
    // re-confirms over the LAN (mDNS). Only the FAILURE path below restores channel 1, because
    // there the link is being torn down anyway and the phone must read "failed" to retry.
    if (!ok) {
        s_connecting = false;
        esp_wifi_disconnect();
        if (keep_ap) {
            // The failed join dragged the shared radio to the target AP's channel. Pull the
            // SoftAP back to its own channel so the phone (still trying to reach the portal on
            // PROV_AP_CHANNEL) can re-associate and read the "failed" status.
            esp_err_t cerr = esp_wifi_set_channel(PROV_AP_CHANNEL, WIFI_SECOND_CHAN_NONE);
            if (cerr != ESP_OK) {
                ESP_LOGW(TAG, "could not restore SoftAP channel: %s", esp_err_to_name(cerr));
            }
        }
        ESP_LOGW(TAG, "connect to '%s' failed", ssid);
    }
    return ok;
}

bool prov_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    return connect_impl(ssid, password, timeout_ms, false);
}

bool prov_wifi_connect_keep_ap(const char *ssid, const char *password, uint32_t timeout_ms)
{
    return connect_impl(ssid, password, timeout_ms, true);
}

void prov_wifi_start_ap(const char *ap_ssid)
{
    s_connecting = false;  // stop the bounded initial-join retries
    s_online = false;      // and don't auto-reconnect the station while in setup mode

    wifi_config_t wc = {0};
    strlcpy((char *)wc.ap.ssid, ap_ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(ap_ssid);
    wc.ap.channel = PROV_AP_CHANNEL;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_OPEN;  // open network for easy join

    // APSTA so the station interface remains available for scanning.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_LOGI(TAG, "SoftAP '%s' up at 192.168.4.1", ap_ssid);
}

// Blocking scan that fills the cache synchronously. Call this BEFORE the SoftAP serves any
// client (no one to disrupt), so the network list is ready the instant the portal opens.
static void scan_prime_blocking(void)
{
    wifi_scan_config_t sc = {0};
    sc.show_hidden = false;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    ESP_LOGI(TAG, "priming initial network scan…");
    esp_err_t e = esp_wifi_scan_start(&sc, true);   // blocks until the sweep completes
    if (e == ESP_OK) {
        collect_scan_results();   // records are available now
    } else {
        ESP_LOGW(TAG, "initial scan_start failed: %s", esp_err_to_name(e));
    }
}

// Kick a NON-blocking sweep; results arrive via WIFI_EVENT_SCAN_DONE (periodic refresh).
static void start_async_scan(void)
{
    wifi_scan_config_t sc = {0};
    sc.show_hidden = false;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    s_scan_inflight = true;
    esp_err_t e = esp_wifi_scan_start(&sc, false);
    if (e != ESP_OK) {
        s_scan_inflight = false;
        ESP_LOGW(TAG, "async scan_start failed: %s", esp_err_to_name(e));
        if (s_scanning) {
            esp_timer_start_once(s_scan_timer, SCAN_PERIOD_US);  // retry next period
        }
    }
}

static void scan_timer_cb(void *arg)
{
    (void)arg;
    // Never scan while a phone is using the portal: an off-channel scan resets its TCP
    // session. The cache keeps the last results; refresh resumes once the client leaves.
    if (s_ap_clients > 0) {
        if (s_scanning) {
            esp_timer_start_once(s_scan_timer, SCAN_PERIOD_US);
        }
        return;
    }
    start_async_scan();
}

void prov_wifi_start_scanning(void)
{
    scan_prime_blocking();   // synchronous first fill so the list shows immediately

    if (!s_scan_timer) {
        const esp_timer_create_args_t args = { .callback = scan_timer_cb, .name = "prov_scan" };
        if (esp_timer_create(&args, &s_scan_timer) != ESP_OK) {
            ESP_LOGW(TAG, "scan timer create failed; network list will not auto-refresh");
            return;
        }
    }
    s_scanning = true;
    esp_timer_start_once(s_scan_timer, SCAN_PERIOD_US);  // periodic refresh after the prime
}

size_t prov_wifi_scan_cached(prov_ap_t *out, size_t max)
{
    size_t n = 0;
    if (s_scan_mtx && xSemaphoreTake(s_scan_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        n = s_scan_cache_count < max ? s_scan_cache_count : max;
        for (size_t i = 0; i < n; i++) {
            out[i] = s_scan_cache[i];
        }
        xSemaphoreGive(s_scan_mtx);
    }
    return n;
}

void prov_wifi_mac_suffix(char *out)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    static const char hex[] = "0123456789ABCDEF";
    out[0] = hex[(mac[4] >> 4) & 0xF];
    out[1] = hex[mac[4] & 0xF];
    out[2] = hex[(mac[5] >> 4) & 0xF];
    out[3] = hex[mac[5] & 0xF];
    out[4] = '\0';
}
