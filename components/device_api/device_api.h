/*
 * device_api.h — the STA-mode control server the companion app talks to.
 *
 * Brings up an HTTP/JSON server on port 80 and advertises it over mDNS as
 * `obsidianboard.local`. Call once, after Wi-Fi is connected.
 *
 *   GET  /api/info            { deviceId, model, fw, ip }   — discovery probe
 *   GET  /api/state           the full device snapshot, including the measured
 *                             panel refresh timings
 *   POST /api/refresh         poll the vault source now
 *   POST /api/page            { page: 0..3 }
 *   POST /api/vault           { url: "http://host/vault.json" }  ("" = demo)
 *   POST /api/display/test    run the e-Paper self-test sweep
 *
 * Local-network only: no auth, no TLS, no cloud. That is a deliberate scope
 * choice, not an oversight — the device holds no credentials worth stealing and
 * the only actions are "show a different page" and "fetch from a different URL
 * on this LAN".
 *
 * The hostname is `obsidianboard`, NOT the `tickerboard` of the fortune board
 * this project forked from: that name is hardcoded in the other project's
 * shipped app, and two devices answering one discovery probe on the same LAN is
 * a fault nobody can diagnose from the phone side.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void device_api_start(void);

#ifdef __cplusplus
}
#endif
