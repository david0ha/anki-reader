// Public API: Wi-Fi provisioning with a captive-portal fallback.
//
// Flow on boot:
//   1. Load saved config from NVS.
//   2. If a network is saved, try to join it (bounded by sta_connect_timeout_ms).
//   3. On success: return true with the active config (caller runs its app).
//   4. On failure, or if nothing is saved: bring up a SoftAP + captive portal where the
//      user enters Wi-Fi credentials and a weather location. The submission is saved to
//      NVS and the device reboots, so on the next boot step 2 connects automatically.
//
// This header is intentionally free of ESP-IDF types so callers stay decoupled.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "prov_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROV_EVENT_STA_CONNECTING,  // trying the saved network; info = SSID
    PROV_EVENT_STA_CONNECTED,   // joined the saved network; info = SSID
    PROV_EVENT_PORTAL_STARTED,  // captive portal is up; info = AP SSID to join
    PROV_EVENT_CONFIG_SAVED,    // user submitted new config; device will reboot; info = SSID
} prov_event_t;

typedef void (*prov_event_cb_t)(prov_event_t event, const char *info, void *user);

typedef struct {
    char            ap_ssid_prefix[24];     // AP shows as "<prefix>-XXXX" (XXXX = MAC suffix)
    uint32_t        sta_connect_timeout_ms; // give up on the saved network after this long
    prov_event_cb_t event_cb;               // optional status callback (may be NULL)
    void           *user;                   // passed back to event_cb
} prov_options_t;

// Populate `opts` with sensible defaults (prefix "Obsidian Board", 15 s timeout, no callback).
void provisioning_default_options(prov_options_t *opts);

// Run the flow described above. Returns true when connected to Wi-Fi, with *out holding the
// active SSID and saved location. In the captive-portal path the call blocks until the user
// submits a config, after which the device reboots (so it does not return in that path).
bool provisioning_run(const prov_options_t *opts, prov_config_t *out);

#ifdef __cplusplus
}
#endif
