/*
 * kanji_service.c — see kanji_service.h.
 */
#include "kanji_service.h"

#include <stdlib.h>
#include <string.h>

#include "http_port.h"
#include "kanji_parse.h"

/* --- the grading URL ------------------------------------------------------ */

/* Whether `id` can go into a query string as-is.
 *
 * The unreserved set of RFC 3986, which a UUID is comfortably inside. Anything
 * else — a &, a #, a space — would change where the proxy thinks one parameter
 * ends and the next begins, so such an id is dropped rather than escaped: the
 * board has no percent-encoder, and this is a check that only ever fires on a
 * producer bug. */
static bool id_is_url_safe(const char *id)
{
    for (const char *p = id; *p; p++) {
        const char c = *p;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '-' || c == '_' || c == '.' || c == '~';
        if (!ok) return false;
    }
    return true;
}

bool kanji_service_grade_url(const char *base, kanji_grade_t grade,
                             const char *card_id, char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!base || !base[0]) return false;

    const char *word = kanji_grade_wire(grade);
    if (!word[0]) return false;

    const char sep = strchr(base, '?') ? '&' : '?';

    const size_t base_len = strlen(base);
    const size_t word_len = strlen(word);
    const size_t need = base_len + 1 /* sep */ + 6 /* "grade=" */ + word_len;
    /* Refuse rather than truncate. A cut URL does not fail loudly — it reaches
     * some other path on the proxy with the grade silently dropped, and the
     * damage lands in a review history nothing on the board will ever show. */
    if (need + 1 > out_size) return false;

    memcpy(out, base, base_len);
    out[base_len] = sep;
    memcpy(out + base_len + 1, "grade=", 6);
    memcpy(out + base_len + 7, word, word_len + 1);

    /* The id rides along only if all of it fits. Unlike the grade, a missing id
     * costs a safety check rather than the request, so every reason to leave it
     * out ends here rather than failing the call. */
    if (card_id && card_id[0] && id_is_url_safe(card_id)) {
        const size_t id_len = strlen(card_id);
        if (need + 6 /* "&card=" */ + id_len + 1 <= out_size) {
            memcpy(out + need, "&card=", 6);
            memcpy(out + need + 6, card_id, id_len + 1);
        }
    }
    return true;
}

/* --- fetching ------------------------------------------------------------- */

/* The whole of both public calls: everything either of them does differently is
 * which URL it hands to the port. */
static kanji_fetch_result_t fetch_url(const char *url, kanji_t *out)
{
    int status = 0;
    char *body = http_get(url, &status);
    if (!body) {
        return KANJI_FETCH_TRANSPORT;
    }

    /* Status is checked before the body is parsed, not after. A 404 page, a
     * captive-portal redirect and the proxy's 409 "I am serving a different
     * card now" are all perfectly good documents that happen not to be a study
     * payload, and distinguishing "your URL is wrong" from "your JSON is wrong"
     * is the difference between a fixable and an unfixable message in the log. */
    if (status < 200 || status >= 300) {
        free(body);
        return KANJI_FETCH_HTTP_STATUS;
    }

    bool ok = kanji_parse(body, strlen(body), out);
    free(body);
    return ok ? KANJI_FETCH_OK : KANJI_FETCH_BAD_PAYLOAD;
}

kanji_fetch_result_t kanji_service_fetch(const char *url, kanji_t *out)
{
    if (!url || !url[0] || !out) {
        return KANJI_FETCH_NO_URL;
    }
    return fetch_url(url, out);
}

kanji_fetch_result_t kanji_service_grade(const char *url, kanji_grade_t grade,
                                         const char *card_id, kanji_t *out)
{
    if (!out) return KANJI_FETCH_NO_URL;

    char graded[KANJI_URL_MAX];
    if (!kanji_service_grade_url(url, grade, card_id, graded, sizeof graded)) {
        /* Nothing was sent, so nothing was graded — which is exactly what the
         * caller needs to know to leave the answer on the glass. */
        return KANJI_FETCH_NO_URL;
    }
    return fetch_url(graded, out);
}

const char *kanji_fetch_result_name(kanji_fetch_result_t r)
{
    switch (r) {
    case KANJI_FETCH_OK:          return "ok";
    case KANJI_FETCH_NO_URL:      return "no_url";
    case KANJI_FETCH_TRANSPORT:   return "transport";
    case KANJI_FETCH_HTTP_STATUS: return "http_status";
    case KANJI_FETCH_BAD_PAYLOAD: return "bad_payload";
    default:                      return "unknown";
    }
}
