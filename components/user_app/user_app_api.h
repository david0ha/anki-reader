/*
 * user_app_api.h — thread-safe control bridge for the companion-app HTTP server.
 *
 * The device's state (which page is shown, the drawn fortune) is normally
 * driven by the physical USER/BOOT buttons on the UI task. This bridge lets the
 * STA-mode HTTP server (components/device_api) drive the SAME actions and read
 * a snapshot, without ever touching LVGL or the e-Paper panel directly:
 *
 *   - reads  (user_app_snapshot)  take the app's state lock and copy out plain data.
 *   - writes (draw/page/location/display_test) validate cheaply, then post a
 *     command onto the app's queue; the UI task applies it via the same code
 *     path as a button press.
 *
 * That single-owner rule matters more here than it did on an LCD: a panel
 * refresh takes up to two seconds and cannot be interleaved, so exactly one
 * task is allowed to start one.
 *
 * All functions are safe to call from the HTTP server task, and are no-ops
 * (returning false / an empty snapshot) until UserApp_TaskInit has run.
 */
#pragma once

#include <stdbool.h>

#include "device_api_model.h"

/* Identity reported to the app. The model string and the SoftAP prefix are
 * still "Ticker Board": app/src/app/onboarding/turn-on.tsx tells the user to
 * look for that SSID and app/src/lib/discovery.ts resolves tickerboard.local,
 * both hardcoded. Renaming them is an app-side change, not a firmware one. */
#define DEVICE_MODEL  "Ticker Board"
#define DEVICE_FW     "0.2.0"

#ifdef __cplusplus
extern "C" {
#endif

/* Fill `out` with a snapshot of the running device. Leaves out->device_id and
 * out->ip empty for the caller (device_api owns esp_netif/esp_mac). */
void user_app_snapshot(device_state_t *out);

/* Draw a new fortune. Triggers a full panel refresh, so it is rate-limited to
 * one in flight; returns false if the queue is full or the app is not up. */
bool user_app_draw_fortune(void);

/* Switch page (0 = omikuji, 1 = home). False if out of range. */
bool user_app_set_page(int page);

/* Set the weather location (free text, e.g. "Seoul"), persisted to NVS and
 * applied live — the device re-geocodes without a reboot. Empty turns the
 * weather block off. */
bool user_app_set_location(const char *place);

/* Run the e-Paper self-test pattern sweep. Blocks the UI task for ~10s once it
 * starts, so this only enqueues it. */
bool user_app_display_test(void);

#ifdef __cplusplus
}
#endif
