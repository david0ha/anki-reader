/*
 * vault_service.c — see vault_service.h.
 */
#include "vault_service.h"

#include <stdlib.h>
#include <string.h>

#include "http_port.h"
#include "vault_parse.h"

vault_fetch_result_t vault_service_fetch(const char *url, vault_t *out)
{
    if (!url || !url[0] || !out) {
        return VAULT_FETCH_NO_URL;
    }

    int status = 0;
    char *body = http_get(url, &status);
    if (!body) {
        return VAULT_FETCH_TRANSPORT;
    }

    /* Status is checked before the body is parsed, not after. A 404 page and a
     * captive-portal redirect are both perfectly good documents that happen not
     * to be a vault snapshot, and distinguishing "your URL is wrong" from "your
     * JSON is wrong" is the difference between a fixable and an unfixable
     * message in the log. */
    if (status < 200 || status >= 300) {
        free(body);
        return VAULT_FETCH_HTTP_STATUS;
    }

    bool ok = vault_parse(body, strlen(body), out);
    free(body);
    return ok ? VAULT_FETCH_OK : VAULT_FETCH_BAD_PAYLOAD;
}

const char *vault_fetch_result_name(vault_fetch_result_t r)
{
    switch (r) {
    case VAULT_FETCH_OK:          return "ok";
    case VAULT_FETCH_NO_URL:      return "no_url";
    case VAULT_FETCH_TRANSPORT:   return "transport";
    case VAULT_FETCH_HTTP_STATUS: return "http_status";
    case VAULT_FETCH_BAD_PAYLOAD: return "bad_payload";
    default:                      return "unknown";
    }
}
