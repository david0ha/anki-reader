/*
 * kanji_model.h — everything the board knows about the study session, in one
 * struct.
 *
 * This is the seam of the whole device. `kanji_t` is produced by exactly two
 * things — kanji_parse.c (from the JSON tools/kanji_server.py serves) and
 * kanji_mock.c (the built-in demo card) — and consumed by exactly two — the UI
 * screens and the companion-app JSON. Nothing else reads the network payload,
 * so a change to the wire format lands in one file.
 *
 * Every array is fixed-size and every count is clamped by the parser. The
 * struct is therefore bounded, copyable, and safe to snapshot under a mutex and
 * hand to the UI task without any ownership question.
 *
 * Nothing here is a date, a number needing a locale, or a nested JSON string.
 * The proxy renders "9일 뒤" and "복습" and flattens the card's `back`/`hint`
 * columns, because the board has no RTC to compute a span against and no reason
 * to run a second cJSON pass per card. See docs/kanji-contract.md.
 *
 * Portable: no LVGL, no ESP-IDF. The host tests build this directly.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- capacities -----------------------------------------------------------
 * Display capacities, not protocol limits: the panel is 648x480 and fits three
 * senses, three examples, three shape parts and three comments before anything
 * becomes unreadable. The parser drops the overflow rather than failing, so a
 * card with nine senses still renders its first three.
 *
 * The byte sizes assume UTF-8: a Hangul syllable and a kanji are both 3 bytes,
 * a kana is 3. KANJI_FRONT_MAX of 40 is ten CJK characters plus a NUL — the
 * longest headword in the shipped catalog is ten. */
#define KANJI_FRONT_MAX        40   /* the headword: 10 CJK characters        */
#define KANJI_READING_MAX      64   /* かな reading, possibly ・-joined       */
#define KANJI_SENSE_MAX        48   /* one Korean gloss                       */
#define KANJI_LABEL_MAX        24   /* "N5", "복습", "9일 뒤", a deck name    */
#define KANJI_DECK_MAX         40
#define KANJI_ID_MAX           40   /* a UUID plus NUL                        */
/* One paragraph of explanation. 480 bytes is 160 Hangul syllables, which is
 * what the 설명 sheet's shape block holds: four wrapped lines at 16 px across
 * 620 px. Sized to the rectangle rather than to a round number, because a cap
 * below the box leaves white space the catalog had text for, and one above it
 * clips mid-sentence. */
#define KANJI_BODY_MAX        480
#define KANJI_AUTHOR_MAX       32
#define KANJI_COMMENT_MAX     240

#define KANJI_SENSES_MAX        3
#define KANJI_EXAMPLES_MAX      3
#define KANJI_PARTS_MAX         3
#define KANJI_COMMENTS_MAX      3

/* --- pieces --------------------------------------------------------------- */

/* The four FSRS ratings, in the order the grade dock shows them. The values are
 * the wire values the proxy forwards to the backend (py-fsrs Rating.Again == 1),
 * so the enum can be sent as-is rather than mapped at the call site. */
typedef enum {
    KANJI_GRADE_AGAIN = 1,
    KANJI_GRADE_HARD  = 2,
    KANJI_GRADE_GOOD  = 3,
    KANJI_GRADE_EASY  = 4,
} kanji_grade_t;

#define KANJI_GRADE_COUNT 4

/* One 예문: the Japanese text, its reading, and the Korean gloss. */
typedef struct {
    char text[KANJI_FRONT_MAX];
    char reading[KANJI_READING_MAX];
    char gloss[KANJI_SENSE_MAX];
} kanji_example_t;

/* One component of the headword — the `hint.shapes[]` of the wire card. */
typedef struct {
    char glyph[KANJI_FRONT_MAX];
    char meaning[KANJI_SENSE_MAX];
    char reading[KANJI_READING_MAX];
} kanji_part_t;

/* One comment under the card. Replies are flattened away by the proxy: three
 * rows is all the panel has, and a thread indent inside three rows reads as a
 * rendering bug rather than a conversation. */
typedef struct {
    char author[KANJI_AUTHOR_MAX];
    char body[KANJI_COMMENT_MAX];
    int  likes;
} kanji_comment_t;

/* The learner's FSRS state for this card, already worded by the proxy.
 *
 * `stability_days` and `difficulty_pct` are integers because the panel prints
 * them as integers and because the board has no reason to carry a float it
 * cannot round without libm. Both are -1 when the scheduler has no value yet
 * (a new card), which the UI renders as "—" rather than as zero: a card whose
 * stability is genuinely unknown and one whose stability is zero days are
 * different things, and only one of them is worth showing a number for. */
typedef struct {
    char state[KANJI_LABEL_MAX];       /* wire word: "new"/"learning"/...  */
    char state_label[KANJI_LABEL_MAX]; /* Korean: "새 카드"/"복습"/...      */
    char due[KANJI_LABEL_MAX];         /* "9일 뒤", "곧", "" when never due */
    int  reps;
    int  lapses;
    int  stability_days;               /* -1 = not scheduled yet */
    int  difficulty_pct;               /* -1 = not scheduled yet */
} kanji_fsrs_t;

/* What each of the four ratings would schedule, as the proxy already rendered
 * it against the SERVER clock: "10분 뒤", "4일 뒤", "9일 뒤", "21일 뒤".
 * Indexed by kanji_grade_t - 1. */
typedef struct {
    char span[KANJI_GRADE_COUNT][KANJI_LABEL_MAX];
} kanji_preview_t;

/* --- the card ------------------------------------------------------------- */

typedef struct {
    bool valid;                        /* false = the session served no card */

    char id[KANJI_ID_MAX];
    char front[KANJI_FRONT_MAX];       /* 会う — the hero */
    char reading[KANJI_READING_MAX];   /* あう */
    char level[KANJI_LABEL_MAX];       /* N5 */

    char senses[KANJI_SENSES_MAX][KANJI_SENSE_MAX];
    int  sense_count;

    kanji_example_t examples[KANJI_EXAMPLES_MAX];
    int             example_count;

    char description[KANJI_BODY_MAX];  /* back.shape_explanation */
    char hook_title[KANJI_LABEL_MAX];  /* hint.principle, default "기억 힌트" */
    char hook_body[KANJI_BODY_MAX];    /* hint.reason */

    kanji_part_t parts[KANJI_PARTS_MAX];
    int          part_count;

    kanji_comment_t comments[KANJI_COMMENTS_MAX];
    int             comment_count;
    int             comment_total;     /* server's real count; >= comment_count */

    kanji_fsrs_t    fsrs;
    kanji_preview_t preview;
} kanji_card_t;

/* --- the session ---------------------------------------------------------- */

typedef struct {
    char deck[KANJI_DECK_MAX];         /* "JLPT N5 Vocabulary" — the "channel" */
    char level[KANJI_LABEL_MAX];       /* the active level filter, or "전체"   */
    int  streak;                       /* 연속 일수    */
    int  reviewed_today;               /* 오늘 복습    */
    int  left_new;
    int  left_review;
    int  retry;
    int  track;                        /* 1-based position in today's queue */
    int  track_total;
    bool complete;                     /* the session served its last card */
} kanji_session_t;

/* --- the snapshot --------------------------------------------------------- */

typedef struct {
    bool valid;                 /* false = nothing has ever been loaded */
    bool demo;                  /* true = kanji_mock, no kanji_url configured */

    kanji_session_t session;
    kanji_card_t    card;
} kanji_t;

/* --- helpers (pure, shared by the UI, the API and the tests) -------------- */

/* Copy a UTF-8 string into a fixed buffer, truncating on a character boundary.
 *
 * strlcpy would happily cut a 3-byte kanji in half, and a lone continuation
 * byte does not render as "the sense was long" — it renders as a tofu box, or
 * worse, sends LVGL's decoder past the NUL. Every string that enters kanji_t
 * from the network goes through here. Always NUL-terminates. Returns the number
 * of bytes written (excluding the NUL). */
size_t kanji_str_copy(char *dst, size_t dst_size, const char *src);

/* How many UTF-8 characters (not bytes) `s` holds. The hero label picks its
 * face from this: a two-character headword gets the 56 px face, a nine-character
 * one would run off the panel and gets the 28 px one instead. */
int kanji_utf8_len(const char *s);

/* Bytes in the UTF-8 sequence beginning with `c`, or 1 for a byte that cannot
 * begin one (a stray continuation byte, or 0xF8..0xFF). Never 0 — a zero stride
 * would loop forever on the first bad byte off the network.
 *
 * Exported because the board must have exactly ONE answer to "where does the
 * next character start". Two agree until they don't: the length check that
 * decides whether a headword FITS the 56 px hero face and the coverage check
 * that decides whether that face can DRAW it are two halves of one decision,
 * and a byte of disagreement between them puts a tofu box at 56 px in the
 * middle of the card. */
size_t kanji_utf8_seq_len(unsigned char c);

/* "AGAIN" / "HARD" / "GOOD" / "EASY" — never NULL, even for a bad enum. */
const char *kanji_grade_name(kanji_grade_t g);

/* The wire word the proxy expects on ?grade= — "again"/"hard"/"good"/"easy". */
const char *kanji_grade_wire(kanji_grade_t g);

/* The Korean label the grade dock prints. Never NULL. */
const char *kanji_grade_label(kanji_grade_t g);

/* What `g` would schedule, as the proxy rendered it. "" when unknown. */
const char *kanji_preview_span(const kanji_t *k, kanji_grade_t g);

/* A fingerprint of everything that is drawn. Two snapshots with the same
 * fingerprint produce the same pixels, so the poller can skip a panel refresh
 * entirely — which on e-Paper is the difference between a silent board and one
 * that flashes every five minutes for no reason.
 *
 * Deliberately excludes nothing that reaches the glass, including `demo`, and
 * deliberately excludes the card id, which does not. */
uint32_t kanji_hash(const kanji_t *k);

#ifdef __cplusplus
}
#endif
