/*
 * kanji_service.h — one fetch of the study card, and one grade.
 *
 * Deliberately not a task and not a scheduler: this is http_get() plus
 * kanji_parse(), and nothing else. The polling loop, the retry policy and the
 * decision to refresh the panel live in user_app, where the rest of the timing
 * lives. That split is what lets the simulator call the identical fetch path
 * against the identical URL and render the identical pixels — which is how a
 * change to the contract gets caught on a laptop instead of on the glass.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "kanji_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Long enough for a LAN URL plus "?grade=again". The NVS-backed setting is
 * capped well below this by the provisioning form. */
#define KANJI_URL_MAX 320

typedef enum {
    KANJI_FETCH_OK = 0,
    KANJI_FETCH_NO_URL,      /* nothing configured — the caller shows the demo */
    KANJI_FETCH_TRANSPORT,   /* DNS, connect, TLS or timeout                   */
    KANJI_FETCH_HTTP_STATUS, /* the server answered, but not with a 2xx        */
    KANJI_FETCH_BAD_PAYLOAD, /* 2xx, but not a study payload                   */
} kanji_fetch_result_t;

/* Fetch and parse `url` into *out.
 *
 * *out is written only on KANJI_FETCH_OK. Every other result leaves it
 * untouched, so the caller can keep displaying the previous card and badge it
 * stale rather than blanking the panel on one dropped packet. */
kanji_fetch_result_t kanji_service_fetch(const char *url, kanji_t *out);

/* Grade `card_id` — the card currently on the glass — then fetch the next one.
 *
 * Same contract as kanji_service_fetch: *out is written only on OK, so a grade
 * that the proxy rejects (409 — it is serving a different card now) leaves the
 * learner looking at the answer they already read rather than at a blank.
 *
 * Naming the card is what makes that 409 possible. The proxy advances whenever
 * anything grades — including the web app in another tab — so a rating sent
 * without an id is applied to whatever the proxy happens to be serving, which
 * after a session has moved on is not the card the learner read. `card_id` may
 * be NULL or empty (the demo card has no id); the grade still lands, just
 * without the guard.
 *
 * This is a GET with a query parameter rather than a POST because http_port.h
 * has exactly one function and three implementations; see docs/kanji-contract.md
 * for why that trade is the right one on a single-client LAN service. */
kanji_fetch_result_t kanji_service_grade(const char *url, kanji_grade_t grade,
                                         const char *card_id, kanji_t *out);

/* Build the grading URL for `base`: appends "?grade=good", or "&grade=good" if
 * `base` already carries a query string, then "&card=<card_id>".
 *
 * Returns false — and writes an empty string — if the result would not fit, if
 * the grade is not one of the four, or if `base` is empty. A truncated URL is
 * strictly worse than no request: it would reach some other path on the proxy
 * with a grade silently dropped, and the learner's review history would take
 * the hit.
 *
 * The card id is best-effort, and deliberately so: it is dropped (leaving a
 * valid grade URL) if it is empty, if it would not fit, or if it holds anything
 * but [A-Za-z0-9._~-]. A real id is a UUID, but an id carrying a & or a # would
 * split the query and change which parameters the proxy reads — and losing the
 * guard on a malformed id is a far smaller harm than losing every rating the
 * learner gives from then on. Exposed for the host test; kanji_service_grade
 * uses it. */
bool kanji_service_grade_url(const char *base, kanji_grade_t grade,
                             const char *card_id, char *out, size_t out_size);

/* A short, stable string for logs and the companion-app JSON. Never NULL. */
const char *kanji_fetch_result_name(kanji_fetch_result_t r);

#ifdef __cplusplus
}
#endif
