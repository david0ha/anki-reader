// Captive portal: HTTP config server + DNS hijack that serves the setup page and accepts
// the user's Wi-Fi credentials and weather location.
#pragma once

#include "prov_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called when the user submits a valid configuration from the portal. `cfg` holds the
// parsed SSID/password/location and is ONLY valid for the duration of the call — it points at
// the HTTP handler's stack. Copy what you need (e.g. via prov_store_save); do not store the
// pointer or hand it to another task. Invoked from the HTTP server task.
//
// `on_save` backs the browser HTML form (POST /save) and applies credentials optimistically.
// `on_provision` backs the app JSON API (POST /api/provision): it should kick off an
// asynchronous credential check (returning promptly so the HTTP response can flush) and drive
// the reported status via prov_portal_set_status() so the app can poll GET /api/status.
typedef void (*prov_portal_save_cb_t)(const prov_config_t *cfg, void *user);
typedef void (*prov_portal_provision_cb_t)(const prov_config_t *cfg, void *user);

// State reported by GET /api/status. Mirrors the connect-test lifecycle.
typedef enum {
    PROV_PORTAL_IDLE = 0,
    PROV_PORTAL_CONNECTING,
    PROV_PORTAL_CONNECTED,
    PROV_PORTAL_FAILED,
} prov_portal_state_t;

// Device identity served by GET /api/info so the companion app can register the device. Any
// field may be NULL (served as ""). The pointed-to strings are copied during prov_portal_start.
typedef struct {
    const char *device_id;  // stable per-device id (e.g. the MAC suffix)
    const char *model;      // e.g. "Obsidian Board"
    const char *ap_ssid;    // the SoftAP SSID currently broadcast
} prov_portal_info_t;

// Start the HTTP server (port 80) and DNS responder (port 53). `current` is shown on the
// page to pre-fill the saved SSID and location (pass NULL for none). `on_save` backs the HTML
// form; `on_provision` backs the JSON API (either may be NULL). `info` (may be NULL) is served
// by GET /api/info.
void prov_portal_start(const prov_config_t *current,
                       prov_portal_save_cb_t on_save,
                       prov_portal_provision_cb_t on_provision,
                       const prov_portal_info_t *info,
                       void *user);

// Update the status reported by GET /api/status (thread-safe; callable from any task).
// `ssid`/`reason` may be NULL/"" to clear them.
void prov_portal_set_status(prov_portal_state_t state, const char *ssid, const char *reason);

#ifdef __cplusplus
}
#endif
