/*
 * vault_parse.h — the wire payload -> vault_t.
 *
 * The one place that understands the JSON contract in
 * docs/specs/2026-08-10-obsidian-board-design.md. Everything downstream sees
 * only the clamped, validated struct.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "vault_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse `len` bytes of JSON into *out.
 *
 * Returns true and overwrites *out only on success. On failure *out is left
 * untouched, so the caller keeps whatever snapshot is already on the glass —
 * preserving the last good image beats replacing it with a blank one.
 *
 * Failure means: not JSON, not an object, truncated, or an object carrying no
 * vault content whatsoever (which is what an error envelope or a captive-portal
 * login page parses down to). Individual bad fields are NOT failures — they
 * clamp to a default. */
bool vault_parse(const char *json, size_t len, vault_t *out);

#ifdef __cplusplus
}
#endif
