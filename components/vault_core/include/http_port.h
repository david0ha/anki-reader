/*
 * http_port.h — the one platform seam.
 *
 * http_get() performs a blocking HTTPS GET and returns the response body as a
 * freshly malloc'd, NUL-terminated string (caller frees), or NULL on transport
 * failure. *out_status receives the HTTP status code (0 if unknown).
 *
 * Two implementations exist and are linked per build:
 *   - http_port_curl.c   (desktop simulator + nothing else)   -> libcurl
 *   - http_port_esp.c     (device firmware)                    -> esp_http_client
 *
 * Everything above this seam (weather_service, weather_parse, the UI) is shared
 * verbatim, so the simulator exercises the real fetch+parse+render pipeline.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once before any http_get(), from a single thread, before the fetch tasks
 * start. Creates the global TLS-connect gate (device port) so concurrent first
 * handshakes serialize; a no-op where the port needs no gate (simulator). */
void http_port_init(void);

char *http_get(const char *url, int *out_status);

#ifdef __cplusplus
}
#endif
