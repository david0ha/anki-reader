/*
 * kanji_parse.c — the wire payload -> kanji_t.
 *
 * The producer is tools/kanji_server.py, relaying a REST API over the open
 * internet. It will send a float where an int belongs, a null where a string
 * belongs, an empty array, a 900-entry array, an example row with nothing in
 * it, and — the day the laptop sleeps — half a response, cut in the middle of a
 * three-byte kanji. None of that may take the board down, and none of it may
 * leave a half-written card on the glass.
 *
 * So: parse into a local, validate and clamp every field, and only copy into
 * the caller's struct on success. A rejected payload leaves the previous card
 * exactly as it was, which is why the header can honestly badge it 오래됨
 * rather than going blank.
 *
 * Portable: cJSON only. test_kanji_parse.c builds this file directly.
 */
#include "kanji_parse.h"

#include <limits.h>
#include <string.h>

#include "cJSON.h"

/* --- defensive accessors --------------------------------------------------
 * Every one of these takes "the key is missing" and "the key holds the wrong
 * type" to the same place: the default. That is the entire error policy for
 * individual fields, and it is why the field code below has no branches. */

static int jint(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v)) return def;
    double d = cJSON_GetNumberValue(v);
    if (d < -2147483000.0) return -2147483000;
    if (d >  2147483000.0) return  2147483000;
    return (int)d;
}

/* A counter, clamped into [0, hi]. Counters that go backwards are a producer
 * bug, and a nine-digit streak would run off the header. */
static int jcount(const cJSON *o, const char *key, int hi)
{
    int v = jint(o, key, 0);
    if (v < 0) return 0;
    return v > hi ? hi : v;
}

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static bool jbool(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : false;
}

static const cJSON *jarr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsArray(v) ? v : NULL;
}

static const cJSON *jobj(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsObject(v) ? v : NULL;
}

/* --- input hygiene --------------------------------------------------------
 * cJSON is happy to parse a body whose string bytes are not valid UTF-8, and
 * every one of those bytes would go straight to LVGL's decoder. So the bytes
 * are checked before anything else looks at them: the board draws what the
 * network sends, and a lone continuation byte does not render as a mistake, it
 * renders as the decoder walking past the end of the buffer. */

static bool valid_json_utf8(const char *json, size_t len)
{
    const unsigned char *s = (const unsigned char *)json;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < len;) {
        unsigned char c = s[i++];
        if (c < 0x20) {
            /* JSON permits space/tab/CR/LF between tokens, but every control
             * inside a string must be escaped. A raw NUL must never silently
             * terminate a bounded response. */
            if (in_string || (c != '\t' && c != '\n' && c != '\r')) return false;
            continue;
        }
        if (c < 0x80) {
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
            } else if (c == '"') {
                in_string = true;
            }
            continue;
        }

        size_t continuation = 0;
        uint32_t codepoint = 0;
        uint32_t minimum = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            continuation = 1;
            codepoint = (uint32_t)(c & 0x1F);
            minimum = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            continuation = 2;
            codepoint = (uint32_t)(c & 0x0F);
            minimum = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            continuation = 3;
            codepoint = (uint32_t)(c & 0x07);
            minimum = 0x10000;
        } else {
            return false;
        }
        if (continuation > len - i) return false;
        for (size_t k = 0; k < continuation; k++) {
            unsigned char tail = s[i++];
            if ((tail & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (uint32_t)(tail & 0x3F);
        }
        if (codepoint < minimum || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
    }
    return true;
}

static bool only_json_whitespace(const char *p, const char *end)
{
    while (p < end) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return false;
        p++;
    }
    return true;
}

/* --- blankness ------------------------------------------------------------
 * A row whose every field is empty or whitespace draws as a gap in a list,
 * which reads as a rendering bug rather than as missing data. The producer
 * should not send them; the parser drops them either way. */

static bool blank(const char *s)
{
    for (; *s; s++) {
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') return false;
    }
    return true;
}

/* --- the session ---------------------------------------------------------- */

/* Nine thousand nine hundred and ninety-nine of anything is already more than
 * the header has room for; past that the number is noise and the digits would
 * push the chip into the brand mark. */
#define COUNT_MAX   9999
#define REPS_MAX    99999

static void parse_session(const cJSON *root, kanji_t *k)
{
    const cJSON *s = jobj(root, "session");
    if (!s) return;

    kanji_str_copy(k->session.deck, sizeof k->session.deck, jstr(s, "deck"));
    kanji_str_copy(k->session.level, sizeof k->session.level, jstr(s, "level"));

    k->session.streak         = jcount(s, "streak", COUNT_MAX);
    k->session.reviewed_today = jcount(s, "reviewed_today", COUNT_MAX);
    k->session.left_new       = jcount(s, "left_new", COUNT_MAX);
    k->session.left_review    = jcount(s, "left_review", COUNT_MAX);
    k->session.retry          = jcount(s, "retry", COUNT_MAX);
    k->session.track_total    = jcount(s, "track_total", COUNT_MAX);
    k->session.track          = jcount(s, "track", COUNT_MAX);
    k->session.complete       = jbool(s, "complete");

    /* "TRK 99/10" on the glass is a producer bug printed at 14 px. */
    if (k->session.track > k->session.track_total) {
        k->session.track = k->session.track_total;
    }
}

/* --- the card's lists ----------------------------------------------------- */

static void parse_senses(const cJSON *c, kanji_card_t *card)
{
    const cJSON *arr = jarr(c, "senses");
    if (!arr) return;

    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (card->sense_count >= KANJI_SENSES_MAX) break;
        if (!cJSON_IsString(it) || !it->valuestring) continue;
        if (blank(it->valuestring)) continue;
        kanji_str_copy(card->senses[card->sense_count], KANJI_SENSE_MAX,
                       it->valuestring);
        card->sense_count++;
    }
}

static void parse_examples(const cJSON *c, kanji_card_t *card)
{
    const cJSON *arr = jarr(c, "examples");
    if (!arr) return;

    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (card->example_count >= KANJI_EXAMPLES_MAX) break;
        if (!cJSON_IsObject(it)) continue;

        const char *text = jstr(it, "text");
        if (blank(text)) continue;      /* the Japanese is the row; no text, no row */

        kanji_example_t *e = &card->examples[card->example_count];
        kanji_str_copy(e->text, sizeof e->text, text);
        kanji_str_copy(e->reading, sizeof e->reading, jstr(it, "reading"));
        kanji_str_copy(e->gloss, sizeof e->gloss, jstr(it, "gloss"));
        card->example_count++;
    }
}

static void parse_parts(const cJSON *c, kanji_card_t *card)
{
    const cJSON *arr = jarr(c, "parts");
    if (!arr) return;

    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (card->part_count >= KANJI_PARTS_MAX) break;
        if (!cJSON_IsObject(it)) continue;

        const char *glyph = jstr(it, "glyph");
        if (blank(glyph)) continue;

        kanji_part_t *p = &card->parts[card->part_count];
        kanji_str_copy(p->glyph, sizeof p->glyph, glyph);
        kanji_str_copy(p->meaning, sizeof p->meaning, jstr(it, "meaning"));
        kanji_str_copy(p->reading, sizeof p->reading, jstr(it, "reading"));
        card->part_count++;
    }
}

static void parse_comments(const cJSON *c, kanji_card_t *card)
{
    const cJSON *arr = jarr(c, "comments");
    if (arr) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, arr) {
            if (card->comment_count >= KANJI_COMMENTS_MAX) break;
            if (!cJSON_IsObject(it)) continue;

            const char *body = jstr(it, "body");
            if (blank(body)) continue;

            kanji_comment_t *m = &card->comments[card->comment_count];
            kanji_str_copy(m->author, sizeof m->author, jstr(it, "author"));
            kanji_str_copy(m->body, sizeof m->body, body);
            m->likes = jcount(it, "likes", COUNT_MAX);
            card->comment_count++;
        }
    }

    card->comment_total = jcount(c, "comment_total", 999999);
    /* "댓글 3개 중 0개" is arithmetic nobody should have to read past. */
    if (card->comment_total < card->comment_count) {
        card->comment_total = card->comment_count;
    }
}

/* --- the card's FSRS state ------------------------------------------------
 * stability_days and difficulty_pct default to -1, not 0. A new card has no
 * stability; a card with a same-day interval has one that rounds to zero. The
 * sheet prints the first as — and the second as 0일, and conflating them makes
 * the board claim to know something it does not. */

static void parse_fsrs(const cJSON *c, kanji_card_t *card)
{
    card->fsrs.stability_days = -1;
    card->fsrs.difficulty_pct = -1;

    const cJSON *f = jobj(c, "fsrs");
    if (!f) return;

    kanji_str_copy(card->fsrs.state, sizeof card->fsrs.state, jstr(f, "state"));
    kanji_str_copy(card->fsrs.state_label, sizeof card->fsrs.state_label,
                   jstr(f, "state_label"));
    kanji_str_copy(card->fsrs.due, sizeof card->fsrs.due, jstr(f, "due"));

    card->fsrs.reps   = jcount(f, "reps", REPS_MAX);
    card->fsrs.lapses = jcount(f, "lapses", REPS_MAX);

    const int stability = jint(f, "stability_days", -1);
    card->fsrs.stability_days = stability < 0 ? -1 : stability;

    const int difficulty = jint(f, "difficulty_pct", -1);
    card->fsrs.difficulty_pct =
        difficulty < 0 ? -1 : (difficulty > 100 ? 100 : difficulty);
}

static void parse_preview(const cJSON *c, kanji_card_t *card)
{
    const cJSON *p = jobj(c, "preview");
    if (!p) return;

    /* Ordered by kanji_grade_t, which is the backend's own 1..4. */
    static const char *KEYS[KANJI_GRADE_COUNT] = { "again", "hard", "good", "easy" };
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        kanji_str_copy(card->preview.span[i], KANJI_LABEL_MAX, jstr(p, KEYS[i]));
    }
}

static void parse_card(const cJSON *root, kanji_t *k)
{
    kanji_card_t *card = &k->card;
    parse_fsrs(NULL, card);          /* the -1 defaults, even with no card */

    const cJSON *c = jobj(root, "card");
    if (!c) return;

    const char *front = jstr(c, "front");
    if (blank(front)) {
        /* A card with no headword is not a card. An empty hero over a real
         * session reads worse than the completion screen. */
        return;
    }

    kanji_str_copy(card->id, sizeof card->id, jstr(c, "id"));
    kanji_str_copy(card->front, sizeof card->front, front);
    kanji_str_copy(card->reading, sizeof card->reading, jstr(c, "reading"));
    kanji_str_copy(card->level, sizeof card->level, jstr(c, "level"));
    kanji_str_copy(card->description, sizeof card->description,
                   jstr(c, "description"));
    kanji_str_copy(card->hook_title, sizeof card->hook_title, jstr(c, "hook_title"));
    kanji_str_copy(card->hook_body, sizeof card->hook_body, jstr(c, "hook_body"));

    parse_senses(c, card);
    parse_examples(c, card);
    parse_parts(c, card);
    parse_comments(c, card);
    parse_fsrs(c, card);
    parse_preview(c, card);

    card->valid = true;
}

/* --- public --------------------------------------------------------------- */

bool kanji_parse(const char *json, size_t len, kanji_t *out)
{
    if (!json || !out || len == 0) return false;
    if (!valid_json_utf8(json, len)) return false;

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &parse_end, false);
    if (!root) return false;                 /* truncated or not JSON at all */
    if (!parse_end || parse_end < json || parse_end > json + len ||
        !only_json_whitespace(parse_end, json + len)) {
        cJSON_Delete(root);
        return false;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    kanji_t k;
    memset(&k, 0, sizeof k);

    const bool has_session = jobj(root, "session") != NULL;
    parse_session(root, &k);
    parse_card(root, &k);

    cJSON_Delete(root);

    /* An object carrying neither a session nor a card is an error envelope or
     * a captive-portal login page, not a payload. Refusing it here is what
     * keeps the last good card on the glass. */
    if (!has_session && !k.card.valid) return false;

    k.valid = true;
    k.demo  = false;
    *out = k;
    return true;
}
