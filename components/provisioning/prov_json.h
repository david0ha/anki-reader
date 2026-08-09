// Pure (host-testable) JSON serializers for the provisioning HTTP/JSON API.
//
// The captive portal serves a browser-facing HTML page, but the companion mobile app drives
// provisioning over a small JSON API (GET /api/info, GET /api/scan, GET /api/status,
// POST /api/provision). These builders produce the response bodies. They are deliberately free
// of ESP-IDF dependencies so they can be unit-tested on the host, mirroring prov_config.c and
// form_parse.c. Every builder NUL-terminates `out` and returns the written length (excluding
// the NUL), or -1 if the result would not fit in `out_size` (in which case `out` is set to "").
#pragma once

#include <stddef.h>

#include "prov_wifi.h"  // prov_ap_t (host-safe: no ESP-IDF types)

#ifdef __cplusplus
extern "C" {
#endif

// Escape `in` as the contents of a JSON string (no surrounding quotes): ", \\ and the C0
// control range (U+0000..U+001F) are escaped (\" \\ \b \f \n \r \t or \u00XX); all other bytes
// (including UTF-8 multibyte sequences) pass through unchanged. Always NUL-terminates `out`.
// Returns the escaped length, or -1 on overflow (with `out` set to "").
int prov_json_escape(const char *in, char *out, size_t out_size);

// Build {"deviceId":"...","model":"...","apSsid":"..."}. NULL fields are emitted as "".
int prov_json_info(char *out, size_t out_size,
                   const char *device_id, const char *model, const char *ap_ssid);

// Build {"state":"<state>"[,"ssid":"..."][,"reason":"..."]}. `state` is required; `ssid` and
// `reason` are omitted entirely when NULL or empty.
int prov_json_status(char *out, size_t out_size,
                     const char *state, const char *ssid, const char *reason);

// Build {"networks":[{"ssid":"...","rssi":<int>,"secure":<bool>},...]} from the scan cache.
// Entries are appended in order and any trailing entry that would overflow `out_size` is
// dropped (the array is still valid JSON); returns -1 only if even the empty envelope
// {"networks":[]} does not fit.
int prov_json_networks(const prov_ap_t *aps, size_t count, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
