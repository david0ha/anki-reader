/*
 * device_api.h — the STA-mode control server the companion app talks to.
 *
 * Brings up an HTTP/JSON server on port 80 and advertises it over mDNS as
 * `tickerboard.local`. Call once, after Wi-Fi is connected.
 *
 *   GET  /api/info            { deviceId, model, fw, ip }   — discovery probe
 *   GET  /api/state           the full device snapshot
 *   POST /api/fortune/draw    draw a new omikuji
 *   POST /api/page            { page: 0|1 }
 *   POST /api/location        { location: "Seoul" }  ("" hides the weather)
 *   POST /api/display/test    run the e-Paper self-test sweep
 *
 * Local-network only: no auth, no TLS, no cloud. That is a deliberate scope
 * choice, not an oversight — the device holds no credentials worth stealing and
 * the only actions are "show a different fortune" and "change the city".
 *
 * The hostname and `/api/info` shape are fixed by the shipped app
 * (app/src/lib/discovery.ts) and must not change without an app release.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void device_api_start(void);

#ifdef __cplusplus
}
#endif
