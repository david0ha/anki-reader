// Pure (host-testable) parsing for application/x-www-form-urlencoded request bodies.
// No ESP-IDF dependency. The ESP HTTP server's httpd_query_key_value() does not URL-decode
// values nor handle '+' as space, so we do our own decoding here and unit-test it.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decode x-www-form-urlencoded escapes from `in` into `out`: '+' becomes a space and "%XX"
// becomes the corresponding byte. Malformed "%" sequences (not followed by two hex digits)
// are copied through literally. `out` is always NUL-terminated. Returns the decoded length
// (excluding the NUL), or -1 if the result would not fit in `out_size` or contains an
// embedded NUL ("%00") — which cannot appear in a valid SSID/password.
int prov_url_decode(const char *in, char *out, size_t out_size);

// Look up form field `key` in `body` ("k1=v1&k2=v2&...") and URL-decode the first match's
// value into `out` (NUL-terminated, capacity out_size). Keys are matched whole (a search
// for "ssid" will not match "ssid_manual"). Returns true when found, false otherwise; on
// false (or decode overflow) `out` is set to "".
bool prov_form_get_field(const char *body, const char *key, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
