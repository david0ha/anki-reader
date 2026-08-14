/*
 * kanji_model.c — the pure helpers declared in kanji_model.h.
 *
 * No LVGL, no ESP-IDF, no allocation. Everything here is exercised directly by
 * test_kanji_model.c and indirectly by every screen.
 */
#include "kanji_model.h"

#include <string.h>

/* --- UTF-8-safe copy ------------------------------------------------------ */

/* Treating junk as width 1 makes truncation degrade to a byte copy for invalid
 * input rather than looping or over-reading. Public (see kanji_model.h) because
 * the UI's font-coverage walk has to agree with this one exactly. */
size_t kanji_utf8_seq_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t kanji_str_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }

    size_t out = 0;
    size_t i = 0;
    for (;;) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\0') break;

        size_t n = kanji_utf8_seq_len(c);
        /* A sequence the source truncates is dropped whole: copying its first
         * two bytes would hand LVGL's decoder a codepoint that is not there. */
        for (size_t k = 1; k < n; k++) {
            if (src[i + k] == '\0') { n = 0; break; }
        }
        if (n == 0) break;

        if (out + n >= dst_size) break;      /* no room for this glyph + NUL */
        memcpy(dst + out, src + i, n);
        out += n;
        i   += n;
    }
    dst[out] = '\0';
    return out;
}

int kanji_utf8_len(const char *s)
{
    if (!s) return 0;
    int n = 0;
    for (size_t i = 0; s[i] != '\0'; ) {
        i += kanji_utf8_seq_len((unsigned char)s[i]);
        n++;
    }
    return n;
}

/* --- the grade vocabulary ------------------------------------------------- */

/* One table, three columns. The dock, the wire and the log all read the same
 * row, so a rating cannot mean AGAIN on the glass and "hard" on the wire — the
 * one bug in this file that would silently corrupt somebody's review history. */
static const struct {
    kanji_grade_t g;
    const char   *name;
    const char   *wire;
    const char   *label;
} GRADES[KANJI_GRADE_COUNT] = {
    { KANJI_GRADE_AGAIN, "AGAIN", "again", "다시"   },
    { KANJI_GRADE_HARD,  "HARD",  "hard",  "어려움" },
    { KANJI_GRADE_GOOD,  "GOOD",  "good",  "보통"   },
    { KANJI_GRADE_EASY,  "EASY",  "easy",  "쉬움"   },
};

/* Index into GRADES[], or -1. The enum's values are the backend's 1..4, so the
 * mapping is arithmetic — but it is checked rather than assumed, because the
 * one caller that can pass a bad value is the one reading a button. */
static int grade_index(kanji_grade_t g)
{
    const int i = (int)g - 1;
    if (i < 0 || i >= KANJI_GRADE_COUNT) return -1;
    return i;
}

const char *kanji_grade_name(kanji_grade_t g)
{
    const int i = grade_index(g);
    return i < 0 ? "" : GRADES[i].name;
}

const char *kanji_grade_wire(kanji_grade_t g)
{
    const int i = grade_index(g);
    return i < 0 ? "" : GRADES[i].wire;
}

const char *kanji_grade_label(kanji_grade_t g)
{
    const int i = grade_index(g);
    return i < 0 ? "" : GRADES[i].label;
}

const char *kanji_preview_span(const kanji_t *k, kanji_grade_t g)
{
    const int i = grade_index(g);
    if (!k || i < 0) return "";
    return k->card.preview.span[i];
}

/* --- fingerprint ---------------------------------------------------------- */

/* FNV-1a, fed field by field rather than over the struct: struct padding is
 * never initialised, so hashing the raw bytes would make the fingerprint depend
 * on whatever was on the stack — and the whole point is that identical content
 * must hash identically, every boot, on device and in the simulator. */
#define FNV_OFFSET 2166136261u
#define FNV_PRIME  16777619u

static void h_bytes(uint32_t *h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= FNV_PRIME;
    }
}

static void h_str(uint32_t *h, const char *s)
{
    h_bytes(h, s, strlen(s));
    h_bytes(h, "\0", 1);        /* separator: "ab"+"c" must differ from "a"+"bc" */
}

static void h_int(uint32_t *h, int v)
{
    int32_t x = (int32_t)v;
    h_bytes(h, &x, sizeof(x));
}

static void h_card(uint32_t *h, const kanji_card_t *c)
{
    h_int(h, c->valid);
    /* `id` is deliberately absent: it is routing, not pixels. The same card
     * re-served under a new study_card_id must not flash the panel. */
    h_str(h, c->front);
    h_str(h, c->reading);
    h_str(h, c->level);

    h_int(h, c->sense_count);
    for (int i = 0; i < c->sense_count; i++) h_str(h, c->senses[i]);

    h_int(h, c->example_count);
    for (int i = 0; i < c->example_count; i++) {
        h_str(h, c->examples[i].text);
        h_str(h, c->examples[i].reading);
        h_str(h, c->examples[i].gloss);
    }

    h_str(h, c->description);
    h_str(h, c->hook_title);
    h_str(h, c->hook_body);

    h_int(h, c->part_count);
    for (int i = 0; i < c->part_count; i++) {
        h_str(h, c->parts[i].glyph);
        h_str(h, c->parts[i].meaning);
        h_str(h, c->parts[i].reading);
    }

    h_int(h, c->comment_count);
    h_int(h, c->comment_total);
    for (int i = 0; i < c->comment_count; i++) {
        h_str(h, c->comments[i].author);
        h_str(h, c->comments[i].body);
        h_int(h, c->comments[i].likes);
    }

    h_str(h, c->fsrs.state);
    h_str(h, c->fsrs.state_label);
    h_str(h, c->fsrs.due);
    h_int(h, c->fsrs.reps);
    h_int(h, c->fsrs.lapses);
    h_int(h, c->fsrs.stability_days);
    h_int(h, c->fsrs.difficulty_pct);

    for (int i = 0; i < KANJI_GRADE_COUNT; i++) h_str(h, c->preview.span[i]);
}

uint32_t kanji_hash(const kanji_t *k)
{
    if (!k) return 0;

    uint32_t h = FNV_OFFSET;
    h_int(&h, k->valid);
    h_int(&h, k->demo);

    h_str(&h, k->session.deck);
    h_str(&h, k->session.level);
    h_int(&h, k->session.streak);
    h_int(&h, k->session.reviewed_today);
    h_int(&h, k->session.left_new);
    h_int(&h, k->session.left_review);
    h_int(&h, k->session.retry);
    h_int(&h, k->session.track);
    h_int(&h, k->session.track_total);
    h_int(&h, k->session.complete);

    h_card(&h, &k->card);
    return h;
}
