// Thin Wi-Fi layer for provisioning: station connect with bounded retry, SoftAP, and scan.
// Built on ESP-IDF v6 esp_wifi/esp_netif. Hardware-coupled; verified by build + on-device.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;  // true unless the network is open (no password)
} prov_ap_t;

// One-time init: TCP/IP stack, default event loop, STA+AP netifs, Wi-Fi driver, handlers.
void prov_wifi_init(void);

// Try to join `ssid`/`password` as a station, waiting up to `timeout_ms` for an IP.
// Returns true on success. Safe to call after a previous failed attempt. Switches the radio to
// station-only mode (tearing down any running SoftAP) — used on the boot path.
bool prov_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);

// Like prov_wifi_connect, but leaves the current radio mode untouched so a running SoftAP keeps
// serving the captive portal during the join. Used by the app-driven credential check (POST
// /api/provision): the phone stays associated to the AP and polls GET /api/status. On FAILURE
// the SoftAP channel is restored so the phone can re-reach the portal and read the result.
bool prov_wifi_connect_keep_ap(const char *ssid, const char *password, uint32_t timeout_ms);

// Bring up an open SoftAP named `ap_ssid` (mode becomes APSTA).
void prov_wifi_start_ap(const char *ap_ssid);

// Begin periodic, NON-BLOCKING background scanning (every ~10s) that publishes results into an
// internal cache via WIFI_EVENT_SCAN_DONE. Call once after the SoftAP is up. The captive portal
// must serve the cache (prov_wifi_scan_cached), never scan live inside a request — a blocking
// scan there would leave the AP channel and reset the connected client's TCP session.
void prov_wifi_start_scanning(void);

// Copy up to `max` cached scan entries into `out`; returns the count. Non-disruptive (no radio
// activity) — safe to call from the HTTP handler while serving the captive portal.
size_t prov_wifi_scan_cached(prov_ap_t *out, size_t max);

// Write the 4-hex-digit MAC suffix (e.g. "9F3A") into `out` (needs >= 5 bytes).
void prov_wifi_mac_suffix(char *out);

#ifdef __cplusplus
}
#endif
