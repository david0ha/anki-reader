/*
 * user_app_api.h — thread-safe control bridge for the companion-app HTTP server.
 *
 * The device is normally driven by the physical KEY0/1/2 buttons on the UI
 * task. This bridge lets the STA-mode HTTP server (components/device_api) drive
 * the SAME actions and read a snapshot, without ever touching LVGL or the
 * e-Paper panel directly:
 *
 *   - reads  (user_app_snapshot)  take the app's state lock and copy out plain data.
 *   - writes (screen/refresh/url/display_test) validate cheaply, then post a
 *     command onto the app's queue; the UI task applies it via the same code
 *     path as a button press.
 *
 * That single-owner rule matters more on this board than on an LCD: a full
 * refresh of a 648x480 panel takes seconds and cannot be interleaved with
 * another, so exactly one task is allowed to start one.
 *
 * All functions are safe to call from the HTTP server task, and are no-ops
 * (returning false / an empty snapshot) until UserApp_TaskInit has run.
 */
#pragma once

#include <stdbool.h>

#include "device_api_model.h"

/* The human-readable identity: reported by GET /api/info and advertised as the
 * mDNS service instance name. The mDNS HOSTNAME is a separate string and does
 * not follow this one — device_api.c says why.
 *
 * Not the fortune board's "Ticker Board" either. That name and
 * `tickerboard.local` are hardcoded in another project's shipped companion app,
 * so reusing them would make two different devices answer one discovery probe
 * on the same LAN. */
#define DEVICE_MODEL  "Kanjis Board"
#define DEVICE_FW     "0.1.0"

#ifdef __cplusplus
extern "C" {
#endif

/* Fill `out` with a snapshot of the running device. Leaves out->device_id and
 * out->ip empty for the caller (device_api owns esp_netif/esp_mac). */
void user_app_snapshot(device_state_t *out);

/* Put the board on `screen`, one of kanji_screen_t's 0..KANJI_SCREEN_COUNT-1.
 *
 * It drives the same nav state a button press does rather than a parallel one,
 * so a phone cannot leave the board on a screen the buttons cannot get it out
 * of. Returns false for an out-of-range screen. */
bool user_app_set_screen(int screen);

/* Poll the study source now instead of waiting out the interval. The panel is
 * only refreshed if the fetched card differs from what is on the glass. */
bool user_app_refresh_now(void);

/* Point the device at a different study URL. Validated, persisted to NVS and
 * applied live — no reboot. Empty restores the catalog, with the built-in
 * demo card only when no valid catalog snapshot exists.
 * Returns false if the URL is not usable (see prov_validate_study_url). */
bool user_app_set_study_url(const char *url);

/* Run the e-Paper self-test pattern sweep. Blocks the UI task for tens of
 * seconds once it starts, so this only enqueues it. */
bool user_app_display_test(void);

#ifdef __cplusplus
}
#endif
