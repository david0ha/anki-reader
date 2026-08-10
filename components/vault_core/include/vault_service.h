/*
 * vault_service.h — one fetch of the vault snapshot.
 *
 * Deliberately not a task and not a scheduler: this is http_get() plus
 * vault_parse(), and nothing else. The polling loop, the retry policy and the
 * decision to refresh the panel live in user_app, where the rest of the timing
 * lives. That split is what lets the simulator call the identical fetch path
 * against the identical URL and render the identical pixels — which is how a
 * change to the contract gets caught on a laptop instead of on the glass.
 */
#pragma once

#include <stdbool.h>

#include "vault_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VAULT_FETCH_OK = 0,
    VAULT_FETCH_NO_URL,      /* nothing configured — the caller shows the demo */
    VAULT_FETCH_TRANSPORT,   /* DNS, connect, TLS or timeout                   */
    VAULT_FETCH_HTTP_STATUS, /* the server answered, but not with a 2xx        */
    VAULT_FETCH_BAD_PAYLOAD, /* 2xx, but not a vault snapshot                  */
} vault_fetch_result_t;

/* Fetch and parse `url` into *out.
 *
 * *out is written only on VAULT_FETCH_OK. Every other result leaves it
 * untouched, so the caller can keep displaying the previous snapshot and badge
 * it stale rather than blanking the panel on one dropped packet. */
vault_fetch_result_t vault_service_fetch(const char *url, vault_t *out);

/* A short, stable string for logs and the companion-app JSON. Never NULL. */
const char *vault_fetch_result_name(vault_fetch_result_t r);

#ifdef __cplusplus
}
#endif
