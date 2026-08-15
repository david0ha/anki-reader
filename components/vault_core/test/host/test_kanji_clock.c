/*
 * test_kanji_clock.c — the board's only source of "what time is it", and the
 * only place it words a span itself.
 *
 * Two properties are worth a test here and neither is visible by inspection:
 *
 *   1. An unsynced clock must refuse to answer. Every other bug in this file
 *      costs a card shown a few hours early; this one silently schedules the
 *      whole catalog against 1970, and it looks like a working board until the
 *      learner notices the queue never empties.
 *
 *   2. The wording must match tools/kanji_server.py's relative_due() — itself a
 *      port of kanjis-front's relativeDue() — to the character. The expected
 *      strings below were produced by running that Python against these exact
 *      inputs, not by reasoning about the table, and the sweeps cross-check the
 *      integer implementation against the reference's own floating-point
 *      formulation (floor(x + 0.5), i.e. JavaScript's Math.round) rather than
 *      restating the integer arithmetic under test.
 */
#include "kanji_clock.h"
#include "th.h"

#include <math.h>
#include <stdint.h>

/* --- the reference, restated the way the reference states it ---------------
 *
 * Deliberately in doubles, because that is what both the Python proxy and the
 * browser do. A test that recomputed (n + d/2)/d in integers would agree with
 * kanji_clock.c by construction even when both are wrong about a half; this
 * disagrees with it unless the integer rounding really is JS's Math.round. It
 * is exact for every input the sweeps use — all well under 2^53 — so any
 * mismatch it reports is a real one. The one Korean-string duplication in this
 * file lives here on purpose: the point is to write the answer down twice, from
 * two different directions. */
static void ref_relative_due(char *dst, size_t n, double seconds)
{
    const double MIN_S = 60.0, HOUR_S = 3600.0, DAY_S = 86400.0;
    if (seconds < 45.0)          snprintf(dst, n, "곧");
    else if (seconds < HOUR_S)   snprintf(dst, n, "%.0f분 뒤",   floor(seconds / MIN_S + 0.5));
    else if (seconds < DAY_S)    snprintf(dst, n, "%.0f시간 뒤", floor(seconds / HOUR_S + 0.5));
    else if (seconds < 30*DAY_S) snprintf(dst, n, "%.0f일 뒤",   floor(seconds / DAY_S + 0.5));
    else if (seconds < 365*DAY_S)snprintf(dst, n, "%.0f개월 뒤", floor(seconds / (30*DAY_S) + 0.5));
    else                         snprintf(dst, n, "%.0f년 뒤",   floor(seconds / (365*DAY_S) + 0.5));
}

/* Well-formed UTF-8, in the narrow sense that matters on this panel: no
 * severed multi-byte sequence. A truncated span does not fail loudly, it draws
 * a tofu box in the middle of a date, so the truncation policy gets asserted
 * rather than assumed. */
static bool utf8_intact(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int need;
        if      (*p < 0x80) need = 0;
        else if ((*p & 0xE0) == 0xC0) need = 1;
        else if ((*p & 0xF0) == 0xE0) need = 2;
        else if ((*p & 0xF8) == 0xF0) need = 3;
        else return false;                       /* stray continuation byte */
        p++;
        for (int i = 0; i < need; i++) {
            if ((*p & 0xC0) != 0x80) return false;
            p++;
        }
    }
    return true;
}

static const char *due(int64_t seconds)
{
    static char buf[KANJI_RELATIVE_DUE_MAX];
    size_t n = kanji_relative_due(buf, sizeof buf, seconds);
    /* Everything this function can say has to fit the field that carries it. */
    CHECK(n < sizeof buf);
    CHECK(utf8_intact(buf));
    CHECK_INT(n, (long)strlen(buf));
    return buf;
}

/* --- part 2: the wording --------------------------------------------------- */

static void test_span_table(void)
{
    /* Everything already due, and everything below the 45-second floor, is
     * "곧". The reference reaches this by its first comparison being
     * `seconds < 45`, which every negative number passes; a board that instead
     * said "3일 지남" would be inventing a sixth spelling the browser has never
     * printed. INT64_MIN is here because a clock that has just been corrected
     * backwards can hand this function an absurd negative. */
    CHECK_STR(due(INT64_MIN),    "곧");
    CHECK_STR(due(-31536000),    "곧");
    CHECK_STR(due(-45),          "곧");
    CHECK_STR(due(-1),           "곧");
    CHECK_STR(due(0),            "곧");
    CHECK_STR(due(1),            "곧");
    CHECK_STR(due(44),           "곧");

    /* 45 is the first second that gets a number, and it rounds UP to one
     * minute rather than down to zero — "0분 뒤" is never printed. */
    CHECK_STR(due(45),           "1분 뒤");
    CHECK_STR(due(59),           "1분 뒤");
    CHECK_STR(due(60),           "1분 뒤");

    /* 90 s is exactly one and a half minutes: the half that separates
     * Math.round (up) from C's truncation (down) and Python's round (to even).
     * Every tier boundary below repeats the same pair for the same reason. */
    CHECK_STR(due(89),           "1분 뒤");
    CHECK_STR(due(90),           "2분 뒤");

    /* The minute tier runs to 3599 s and rounds to SIXTY minutes there — it
     * does not roll over into the hour tier, because the tier is chosen from
     * the raw seconds and only then rounded. "60분 뒤" looks like a bug and is
     * exactly what the browser prints. */
    CHECK_STR(due(3569),         "59분 뒤");
    CHECK_STR(due(3570),         "60분 뒤");
    CHECK_STR(due(3599),         "60분 뒤");
    CHECK_STR(due(3600),         "1시간 뒤");

    CHECK_STR(due(5399),         "1시간 뒤");   /* 89m 59s */
    CHECK_STR(due(5400),         "2시간 뒤");   /* exactly 1.5 h */

    /* Same shape one tier up: 86399 s is "24시간 뒤", not "1일 뒤". */
    CHECK_STR(due(86399),        "24시간 뒤");
    CHECK_STR(due(86400),        "1일 뒤");

    CHECK_STR(due(129599),       "1일 뒤");     /* 1.5 d minus a second */
    CHECK_STR(due(129600),       "2일 뒤");     /* exactly 1.5 d        */

    /* The day tier ends at 30 days, and the month tier calls a month 30 days.
     * So 2591999 s is "30일 뒤" and one second later is "1개월 뒤" — the same
     * instant, two spellings, which is precisely why this function exists
     * rather than each caller rolling its own. */
    CHECK_STR(due(2591999),      "30일 뒤");
    CHECK_STR(due(2592000),      "1개월 뒤");

    CHECK_STR(due(3887999),      "1개월 뒤");   /* 1.5 months minus a second */
    CHECK_STR(due(3888000),      "2개월 뒤");   /* exactly 1.5 months        */

    /* A 365-day year against 30-day months means the last month-tier value is
     * "12개월 뒤" and the first year-tier value is "1년 뒤". */
    CHECK_STR(due(31535999),     "12개월 뒤");
    CHECK_STR(due(31536000),     "1년 뒤");

    CHECK_STR(due(47303999),     "1년 뒤");
    CHECK_STR(due(47304000),     "2년 뒤");     /* exactly 1.5 years */

    /* The widest thing this can print. It is here to pin KANJI_RELATIVE_DUE_MAX
     * to a measured number rather than an estimate: an int64 of seconds is
     * 292471208678 years, twelve digits, and due() above already asserted the
     * whole thing fits the buffer. */
    CHECK_STR(due(INT64_MAX),    "292471208678년 뒤");
}

/* Every second across the tiers where a boundary lives, checked against the
 * double formulation. This is what catches a rounding helper that is right at
 * the boundaries somebody thought to write down and wrong three seconds later.
 * Failures are reported once with the offending input rather than 36,000
 * times. */
static void sweep(const char *what, int64_t from, int64_t to, int64_t step)
{
    char got[KANJI_RELATIVE_DUE_MAX], want[KANJI_RELATIVE_DUE_MAX];
    int64_t bad_at = -1;
    long mismatches = 0;

    for (int64_t s = from; s <= to; s += step) {
        kanji_relative_due(got, sizeof got, s);
        ref_relative_due(want, sizeof want, (double)s);
        if (strcmp(got, want) != 0) {
            if (mismatches == 0) {
                bad_at = s;
                printf("  first divergence in %s: s=%lld got \"%s\" want \"%s\"\n",
                       what, (long long)s, got, want);
            }
            mismatches++;
        }
    }
    (void)bad_at;
    CHECK_INT(mismatches, 0);
}

static void test_span_sweeps(void)
{
    sweep("seconds..hours", -120, 7200, 1);          /* 곧 / 분 / 시간 edges  */
    sweep("hours..days", 0, 3 * 86400, 7);           /* the 시간 -> 일 edge   */
    sweep("days..months", 0, 40 * 86400, 997);       /* the 일 -> 개월 edge   */
    sweep("months..years", 0, 400 * 86400, 60013);   /* the 개월 -> 년 edge   */
    sweep("years", 0, 4000LL * 86400, 601301);       /* nothing above 년      */
}

/* The truncation policy. A caller that hands over a short buffer gets nothing
 * rather than a wrong span, and never gets a severed UTF-8 sequence. */
static void test_span_buffer(void)
{
    char buf[KANJI_RELATIVE_DUE_MAX];

    /* Measuring mode: no buffer at all, just the length. */
    CHECK_INT(kanji_relative_due(NULL, 0, 86400), 8);   /* "1일 뒤"  */
    CHECK_INT(kanji_relative_due(NULL, 0, 0), 3);       /* "곧"      */
    CHECK_INT(kanji_relative_due(NULL, 0, 3600), 11);   /* "1시간 뒤" */

    /* Exactly enough room, counting the terminator. */
    memset(buf, 'x', sizeof buf);
    CHECK_INT(kanji_relative_due(buf, 9, 86400), 8);
    CHECK_STR(buf, "1일 뒤");

    /* One byte short: the empty string, not "1일 " and not a half-drawn 뒤.
     * "" is already the contract's spelling for "no date to show", so this
     * degrades into a case the UI must handle anyway. */
    memset(buf, 'x', sizeof buf);
    CHECK_INT(kanji_relative_due(buf, 8, 86400), 8);
    CHECK_STR(buf, "");

    memset(buf, 'x', sizeof buf);
    CHECK_INT(kanji_relative_due(buf, 1, 86400), 8);
    CHECK_STR(buf, "");

    /* A zero-size buffer must not be written to at all. */
    memset(buf, 'x', sizeof buf);
    CHECK_INT(kanji_relative_due(buf, 0, 86400), 8);
    CHECK_INT(buf[0], 'x');

    /* And nothing is written past the terminator of a short answer. */
    memset(buf, 'x', sizeof buf);
    CHECK_INT(kanji_relative_due(buf, sizeof buf, 0), 3);
    CHECK_STR(buf, "곧");
    CHECK_INT(buf[4], 'x');
}

/* --- part 1: the clock ----------------------------------------------------- */

#define E2026 INT64_C(1786000000)   /* an ordinary 2026 wall clock */

static void test_unknown_refuses(void)
{
    kanji_clock_t c;
    int64_t out = INT64_C(-999);

    kanji_clock_reset(&c);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_UNKNOWN);

    /* The whole point of the tier. A board that has never been told the time
     * does not get to answer, and — just as important — does not get to write
     * a zero into the caller's variable on the way out. A caller that ignores
     * the bool must end up with its own sentinel, not with 1970. */
    CHECK(!kanji_clock_now(&c, 0, &out));
    CHECK_INT(out, -999);
    CHECK(!kanji_clock_now(&c, 86400 * 30, &out));
    CHECK_INT(out, -999);

    /* An absent clock knows exactly as much as an unsynced one, and none of
     * these may fault. */
    CHECK_INT(kanji_clock_tier(NULL), KANJI_CLOCK_UNKNOWN);
    CHECK(!kanji_clock_now(NULL, 0, &out));
    CHECK_INT(out, -999);
    CHECK(!kanji_clock_now(&c, 0, NULL));
    kanji_clock_reset(NULL);
    kanji_clock_sync(NULL, E2026, 0);
    kanji_clock_restore(NULL, E2026);
}

static void test_restore_is_approximate(void)
{
    kanji_clock_t c;
    int64_t out = 0;

    kanji_clock_reset(&c);
    kanji_clock_restore(&c, E2026);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_APPROXIMATE);

    /* Anchored at uptime zero: the persisted value is what time it was when the
     * board came up, as far as the board can tell. */
    CHECK(kanji_clock_now(&c, 0, &out));
    CHECK_INT(out - E2026, 0);
    CHECK(kanji_clock_now(&c, 3600, &out));
    CHECK_INT(out - E2026, 3600);

    /* A month of uptime does not promote it. Confidence comes from where the
     * anchor came from, never from how long it has been counted. */
    CHECK(kanji_clock_now(&c, 30 * 86400, &out));
    CHECK_INT(out - E2026, 30 * 86400);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_APPROXIMATE);
}

static void test_implausible_epochs_are_dropped(void)
{
    kanji_clock_t c;
    int64_t out = INT64_C(-999);

    /* The 1970 case: SNTP's callback firing before the clock is really set.
     * Believed, this would promote the board to TRUSTED with an anchor half a
     * century wrong, which is the failure the tier exists to prevent — so it
     * changes nothing at all, tier included. */
    kanji_clock_reset(&c);
    kanji_clock_sync(&c, 0, 0);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_UNKNOWN);
    CHECK(!kanji_clock_now(&c, 0, &out));
    CHECK_INT(out, -999);

    kanji_clock_reset(&c);
    kanji_clock_restore(&c, 0);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_UNKNOWN);

    kanji_clock_reset(&c);
    kanji_clock_restore(&c, KANJI_CLOCK_EPOCH_MIN - 1);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_UNKNOWN);

    /* Garbage out of a half-erased journal record: 0xFFFF... read as an int64,
     * or any far-future value. */
    kanji_clock_reset(&c);
    kanji_clock_sync(&c, KANJI_CLOCK_EPOCH_MAX, 0);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_UNKNOWN);
    kanji_clock_sync(&c, INT64_MAX, 0);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_UNKNOWN);
    kanji_clock_sync(&c, INT64_MIN, 0);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_UNKNOWN);

    /* The window is inclusive at the floor and exclusive at the ceiling. */
    kanji_clock_reset(&c);
    kanji_clock_sync(&c, KANJI_CLOCK_EPOCH_MIN, 0);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_TRUSTED);
    CHECK(kanji_clock_now(&c, 0, &out));
    CHECK_INT(out, KANJI_CLOCK_EPOCH_MIN);

    kanji_clock_reset(&c);
    kanji_clock_sync(&c, KANJI_CLOCK_EPOCH_MAX - 1, 0);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_TRUSTED);

    /* A bad sync arriving AFTER a good one must not corrupt the good anchor
     * either — this is the case where a single malformed NTP packet would
     * otherwise throw away a working clock. */
    kanji_clock_reset(&c);
    kanji_clock_sync(&c, E2026, 100);
    kanji_clock_sync(&c, 0, 200);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_TRUSTED);
    CHECK(kanji_clock_now(&c, 100, &out));
    CHECK_INT(out, E2026);
}

static void test_sync_outranks_restore(void)
{
    kanji_clock_t c;
    int64_t out = 0;

    /* Cold boot: flash first, SNTP a few seconds later. */
    kanji_clock_reset(&c);
    kanji_clock_restore(&c, E2026);
    kanji_clock_sync(&c, E2026 + 90000, 5);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_TRUSTED);

    /* The sync's anchor replaces the restored one outright — the restored value
     * was a floor and this is a reading, so there is nothing to average. */
    CHECK(kanji_clock_now(&c, 5, &out));
    CHECK_INT(out - E2026, 90000);
    CHECK(kanji_clock_now(&c, 65, &out));
    CHECK_INT(out - E2026, 90060);

    /* A restore arriving after a sync is ignored, and does not demote the tier.
     * The persisted value is older by definition; letting it win would drag a
     * TRUSTED clock backwards and relabel it APPROXIMATE for nothing. */
    kanji_clock_restore(&c, E2026 - 86400);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_TRUSTED);
    CHECK(kanji_clock_now(&c, 5, &out));
    CHECK_INT(out - E2026, 90000);

    /* A second restore over a live APPROXIMATE clock is ignored for the same
     * reason: the running anchor has counted real seconds, flash has not. */
    kanji_clock_reset(&c);
    kanji_clock_restore(&c, E2026);
    kanji_clock_restore(&c, E2026 + 999999);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_APPROXIMATE);
    CHECK(kanji_clock_now(&c, 100, &out));
    CHECK_INT(out - E2026, 100);
}

static void test_backwards_sync(void)
{
    kanji_clock_t c;
    int64_t out = 0;

    /* A later sync that moves the clock BACK. This is allowed on purpose: the
     * board's first anchor is routinely too late (a restored floor, or a sync
     * against a server that was wrong), and a rule that only ever let time move
     * forwards would pin it there until the next power cut. */
    kanji_clock_reset(&c);
    kanji_clock_sync(&c, E2026 + 86400 * 3, 100);
    CHECK(kanji_clock_now(&c, 100, &out));
    CHECK_INT(out - E2026, 86400 * 3);

    kanji_clock_sync(&c, E2026, 200);
    CHECK_INT(kanji_clock_tier(&c), KANJI_CLOCK_TRUSTED);
    CHECK(kanji_clock_now(&c, 200, &out));
    CHECK_INT(out - E2026, 0);

    /* And it keeps counting forwards from the corrected anchor. */
    CHECK(kanji_clock_now(&c, 260, &out));
    CHECK_INT(out - E2026, 60);
}

static void test_uptime_going_backwards(void)
{
    kanji_clock_t c;
    int64_t out = 0;

    /* A monotonic source that is not: a wrapped tick counter, or a caller that
     * reset its own timebase between two reads. Elapsed goes to zero rather
     * than negative — a clock that ran backwards would un-due a card the
     * learner just graded, and then flap between the two answers on every
     * draw. */
    kanji_clock_reset(&c);
    kanji_clock_sync(&c, E2026, 1000);
    CHECK(kanji_clock_now(&c, 999, &out));
    CHECK_INT(out - E2026, 0);
    CHECK(kanji_clock_now(&c, 0, &out));
    CHECK_INT(out - E2026, 0);
    CHECK(kanji_clock_now(&c, -5, &out));
    CHECK_INT(out - E2026, 0);

    /* The 32-bit millisecond wrap, in seconds: 4294967 s of uptime and then
     * back to nearly nothing. ~49.7 days is well inside what this board is
     * expected to sit powered on for. */
    kanji_clock_reset(&c);
    kanji_clock_sync(&c, E2026, 4294967);
    CHECK(kanji_clock_now(&c, 3, &out));
    CHECK_INT(out - E2026, 0);

    /* And it resumes counting once the source passes the anchor again. */
    CHECK(kanji_clock_now(&c, 4294967 + 42, &out));
    CHECK_INT(out - E2026, 42);
}

static void test_no_signed_overflow(void)
{
    kanji_clock_t c;
    int64_t out = 0;

    /* Both arithmetic hazards, with saturation rather than undefined behaviour.
     * These are not hypothetical inputs from the board's own timer, they are
     * what a wrong caller or a corrupted uptime word looks like — and signed
     * overflow in C is not "a big number", it is the compiler being allowed to
     * delete the range check that follows. Run under -DSANITIZE=ON to see the
     * difference. */
    kanji_clock_reset(&c);
    kanji_clock_sync(&c, KANJI_CLOCK_EPOCH_MAX - 1, INT64_MIN);
    CHECK(kanji_clock_now(&c, INT64_MAX, &out));
    CHECK_INT(out, INT64_MAX);

    kanji_clock_reset(&c);
    kanji_clock_sync(&c, KANJI_CLOCK_EPOCH_MAX - 1, 0);
    CHECK(kanji_clock_now(&c, INT64_MAX, &out));
    CHECK_INT(out, INT64_MAX);

    /* Saturated or not, it may never come back BEFORE the anchor. */
    CHECK(out >= KANJI_CLOCK_EPOCH_MAX - 1);
}

/* The two halves in the arrangement the offline scheduler actually uses:
 * a due timestamp from the catalog, the board's own idea of now, and the span
 * the panel prints. */
static void test_clock_feeds_the_wording(void)
{
    kanji_clock_t c;
    int64_t now = 0;

    kanji_clock_reset(&c);
    kanji_clock_sync(&c, E2026, 0);

    CHECK(kanji_clock_now(&c, 0, &now));
    CHECK_STR(due((E2026 + 9 * 86400) - now), "9일 뒤");
    CHECK_STR(due((E2026 + 600) - now), "10분 뒤");
    CHECK_STR(due((E2026 - 86400) - now), "곧");

    /* Half a day of uptime later, the same absolute due date has moved. */
    CHECK(kanji_clock_now(&c, 43200, &now));
    CHECK_STR(due((E2026 + 9 * 86400) - now), "9일 뒤");   /* 8.5 d rounds up */
    CHECK_STR(due((E2026 + 600) - now), "곧");             /* long past       */

    /* An UNKNOWN clock never reaches this code at all: there is no `now` to
     * subtract from, which is the entire reason kanji_clock_now() returns a
     * bool instead of a best guess. */
    kanji_clock_reset(&c);
    now = INT64_C(-999);
    CHECK(!kanji_clock_now(&c, 43200, &now));
    CHECK_INT(now, -999);
}

int main(void)
{
    test_span_table();
    test_span_sweeps();
    test_span_buffer();

    test_unknown_refuses();
    test_restore_is_approximate();
    test_implausible_epochs_are_dropped();
    test_sync_outranks_restore();
    test_backwards_sync();
    test_uptime_going_backwards();
    test_no_signed_overflow();
    test_clock_feeds_the_wording();

    TH_REPORT("kanji_clock");
}
