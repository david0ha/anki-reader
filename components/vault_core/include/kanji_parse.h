/*
 * kanji_parse.h — the wire payload -> kanji_t.
 *
 * The one place that understands the JSON contract in docs/kanji-contract.md.
 * Everything downstream sees only the clamped, validated struct.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "kanji_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse `len` bytes of JSON into *out.
 *
 * Returns true and overwrites *out only on success. On failure *out is left
 * untouched, so the caller keeps whatever card is already on the glass —
 * preserving the last good image beats replacing it with a blank one, and is
 * why the header can honestly badge a stale card 오래됨 instead of blanking.
 *
 * Failure means: not JSON, not an object, truncated, invalid UTF-8, or an
 * object carrying neither a session nor a card (which is what an error envelope
 * or a captive-portal login page parses down to). Individual bad fields are NOT
 * failures — they clamp to a default. A payload with a session and no card is a
 * SUCCESS: it is how the proxy says "today is done". */
bool kanji_parse(const char *json, size_t len, kanji_t *out);

/* Stack-bounded variant for persistent runtimes. `workspace` must point to a
 * separate kanji_t owned by the caller; NULL and `workspace == out` are
 * rejected. The workspace is scratch and may change on failure, while *out
 * retains the same all-or-nothing contract as kanji_parse(). */
bool kanji_parse_with_workspace(const char *json, size_t len, kanji_t *out,
                                kanji_t *workspace);

#ifdef __cplusplus
}
#endif
