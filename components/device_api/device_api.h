/*
 * device_api.h — the STA-mode control server the companion app talks to.
 *
 * Brings up an HTTP/JSON server on port 80 and advertises it over mDNS as
 * `ankireader.local`. Call once, after Wi-Fi is connected.
 *
 *   GET  /api/info            { deviceId, model, fw, ip }   — discovery probe
 *   GET  /api/state           the device summary: the card on the glass, the
 *                             session counters, the source, the battery, and the
 *                             measured panel refresh timings
 *   POST /api/refresh         poll the study source now
 *   POST /api/screen          { screen: 0..4 }  — kanji_screen_t
 *   POST /api/study           { url: "http://host/kanji.json" }  ("" = demo)
 *   POST /api/display/test    run the e-Paper self-test sweep
 *
 * docs/app-control.md documents every field of every one of them.
 *
 * Local-network only: no auth, no TLS, no cloud. That is a deliberate scope
 * choice, not an oversight — the device holds no credentials worth stealing
 * (the kanjis.ai session lives in the proxy, never here) and the only actions
 * are "show a different screen" and "fetch from a different URL on this LAN".
 *
 * The mDNS hostname is `ankireader` — the same word as the setup AP prefix and
 * DEVICE_MODEL, so the board answers to one name everywhere. NOT the
 * `tickerboard` of the fortune board this project forked from: that name is
 * hardcoded in the other project's shipped app, and two devices answering one
 * discovery probe on the same LAN is a fault nobody can diagnose from the phone
 * side. Boards flashed before the rename still advertise `obsidianboard`.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void device_api_start(void);

#ifdef __cplusplus
}
#endif
