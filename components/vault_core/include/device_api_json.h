/*
 * device_api_json.h — pure serializers for the companion-app HTTP API.
 *
 * Separated from device_api.c (which owns httpd/mDNS/esp_netif) so the exact
 * bytes the phone receives are covered by host tests. Both write a NUL-
 * terminated document into `out` and return its length, or -1 if it would not
 * fit — in which case out[0] is set to '\0' rather than left half-written.
 */
#pragma once

#include <stddef.h>

#include "device_api_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GET /api/info -> {"deviceId","model","fw","ip"}
 *
 * The shape is fixed by the companion app's discovery probe (it fetches this
 * on every candidate host and reads `ip` to pick the best one), so do not
 * rename or drop these four fields without updating app/src/lib/discovery.ts.
 */
int device_api_json_info(char *out, size_t out_size,
                         const char *device_id, const char *model,
                         const char *fw, const char *ip);

/* GET /api/state -> the whole device_state_t. */
int device_api_json_state(const device_state_t *st, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
