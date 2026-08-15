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

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call before any http_get(), from a single thread, before the fetch tasks
 * start. Returns false when the device TLS-connect gate cannot be allocated.
 * Deinit is only safe after every fetch task has stopped. Both are no-ops for
 * a port that needs no gate (the simulator). */
bool http_port_init(void);
void http_port_deinit(void);

char *http_get(const char *url, int *out_status);

#ifdef __cplusplus
}
#endif
