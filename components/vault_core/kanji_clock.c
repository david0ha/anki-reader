/*
 * kanji_clock.c — the anchor arithmetic, and the span wording.
 *
 * Everything here is integer. The board has no FPU worth using and this file
 * is on the path of every card drawn offline, but that is not the reason: the
 * reason is that the reference implementation of the wording rounds halves in a
 * specific direction (JavaScript's Math.round, half away from zero) and doing
 * that in doubles on a 32-bit target invites exactly the kind of last-bit
 * disagreement that makes one card say "30일 뒤" on the panel and "1개월 뒤" in
 * the browser. Halves land on these boundaries constantly — 90 seconds is a
 * minute and a half, 12 hours is half a day — so they are not a corner case.
 */
#include "kanji_clock.h"

#include "kanji_model.h"   /* for the KANJI_LABEL_MAX assertion below only */

#include <stdio.h>
#include <string.h>

/* The wording is written into kanji_t's label fields, so it has to fit one.
 * Asserted rather than commented, because the day this stops holding is the day
 * somebody widens a tier's units and the span silently arrives truncated. */
_Static_assert(KANJI_RELATIVE_DUE_MAX <= KANJI_LABEL_MAX,
               "a worded span must fit the model field that carries it");

/* --- the clock ------------------------------------------------------------- */

static bool epoch_plausible(int64_t epoch)
{
    return epoch >= KANJI_CLOCK_EPOCH_MIN && epoch < KANJI_CLOCK_EPOCH_MAX;
}

/* Saturating, because the inputs are not all ours. The uptime reading comes
 * from the caller's timebase and the epoch from flash or from a network packet,
 * and signed overflow in C is not "wraps to a big negative" — it is undefined,
 * which in practice means the optimiser is entitled to delete the range check
 * written right after it. Saturation gives a wrong-but-bounded answer that the
 * tier already warns the UI about. */
static int64_t sat_sub(int64_t a, int64_t b)
{
    if (b < 0) { if (a > INT64_MAX + b) return INT64_MAX; }
    else       { if (a < INT64_MIN + b) return INT64_MIN; }
    return a - b;
}

static int64_t sat_add(int64_t a, int64_t b)
{
    if (b > 0) { if (a > INT64_MAX - b) return INT64_MAX; }
    else       { if (a < INT64_MIN - b) return INT64_MIN; }
    return a + b;
}

void kanji_clock_reset(kanji_clock_t *c)
{
    if (!c) return;
    c->tier = KANJI_CLOCK_UNKNOWN;
    c->epoch_at_anchor = 0;
    c->uptime_at_anchor = 0;
}

void kanji_clock_sync(kanji_clock_t *c, int64_t epoch, int64_t uptime_s)
{
    if (!c || !epoch_plausible(epoch)) return;

    /* No comparison against the existing anchor. A sync is the most-informed
     * thing this module ever receives, so it replaces what is there even when
     * that moves the reading backwards — see the header for why refusing the
     * correction would be the worse failure. */
    c->epoch_at_anchor = epoch;
    c->uptime_at_anchor = uptime_s;
    c->tier = KANJI_CLOCK_TRUSTED;
}

void kanji_clock_restore(kanji_clock_t *c, int64_t epoch)
{
    if (!c || c->tier != KANJI_CLOCK_UNKNOWN || !epoch_plausible(epoch)) return;

    /* Uptime zero, not "now": the persisted value describes a moment before the
     * board lost power, so charging this boot's whole uptime against it is the
     * closest thing to correct available — and errs early, which is the safe
     * direction. */
    c->epoch_at_anchor = epoch;
    c->uptime_at_anchor = 0;
    c->tier = KANJI_CLOCK_APPROXIMATE;
}

bool kanji_clock_now(const kanji_clock_t *c, int64_t uptime_s, int64_t *out_epoch)
{
    if (!c || !out_epoch || c->tier == KANJI_CLOCK_UNKNOWN) return false;

    int64_t elapsed = sat_sub(uptime_s, c->uptime_at_anchor);
    if (elapsed < 0) elapsed = 0;   /* a source that ran backwards; see header */

    *out_epoch = sat_add(c->epoch_at_anchor, elapsed);
    return true;
}

kanji_clock_tier_t kanji_clock_tier(const kanji_clock_t *c)
{
    return c ? c->tier : KANJI_CLOCK_UNKNOWN;
}

/* --- the span wording ------------------------------------------------------
 *
 * These six literals are the whole vocabulary, and they are here rather than in
 * ui_strings.h on purpose. ui_strings.h is the board's *copy* — the words a
 * designer may reword — and these are not that: they are a wire-format
 * agreement with tools/kanji_server.py and with kanjis-front, and rewording one
 * without rewording all three is the desync this module exists to prevent.
 *
 * The font question that normally forces a string into ui_strings.h does not
 * arise: 곧 분 시간 일 개월 년 뒤 are all 완성형 syllables and the digits and
 * space are in S_COMPOSED_CHARS, so every character here is already in the
 * 9,242-glyph body faces. Verified, not assumed:
 *
 *   python3 -c "import sys; sys.path.insert(0,'tools'); import gen_fonts; \
 *               s=gen_fonts.symbol_set(); print(all(c in s for c in '곧분시간일개월년뒤0123456789 '))"
 *
 * If the faces are ever subset below 완성형, that command is what will say so.
 */
#define S_SPAN_SOON     "곧"
#define S_SPAN_MINUTES  "분 뒤"
#define S_SPAN_HOURS    "시간 뒤"
#define S_SPAN_DAYS     "일 뒤"
#define S_SPAN_MONTHS   "개월 뒤"
#define S_SPAN_YEARS    "년 뒤"

#define SPAN_SOON_S    INT64_C(45)
#define SPAN_MIN_S     INT64_C(60)
#define SPAN_HOUR_S    INT64_C(3600)
#define SPAN_DAY_S     INT64_C(86400)
#define SPAN_MONTH_S   (30  * SPAN_DAY_S)    /* the reference's month: 30 days */
#define SPAN_YEAR_S    (365 * SPAN_DAY_S)    /* and its year: 365              */

/* JavaScript's Math.round in integers: floor(n/d + 0.5) for a non-negative n.
 *
 * Written as a remainder comparison rather than (n + d/2)/d because the year
 * branch can be handed most of an int64 and the addition would overflow. r is
 * strictly less than d, and the largest d here is 31,536,000, so r * 2 cannot.
 *
 * Only ever called with n >= SPAN_SOON_S, so the negative half of Math.round
 * (which rounds -0.5 to -0, i.e. toward +infinity, not away from zero) is
 * unreachable and deliberately not implemented — every negative input has
 * already been answered with 곧. */
static int64_t round_half_up(int64_t n, int64_t d)
{
    int64_t q = n / d;
    int64_t r = n % d;
    return (r * 2 >= d) ? q + 1 : q;
}

size_t kanji_relative_due(char *dst, size_t dst_size, int64_t seconds_from_now)
{
    char tmp[KANJI_RELATIVE_DUE_MAX];
    int written;

    /* The tier is chosen from the raw seconds and only then is the number
     * rounded, which is what produces the reference's two surprising answers:
     * 3599 s is "60분 뒤" rather than "1시간 뒤", and 86399 s is "24시간 뒤".
     * Both are what the browser prints, so both are what the panel prints. */
    if (seconds_from_now < SPAN_SOON_S) {
        written = snprintf(tmp, sizeof tmp, "%s", S_SPAN_SOON);
    } else if (seconds_from_now < SPAN_HOUR_S) {
        written = snprintf(tmp, sizeof tmp, "%lld%s",
                           (long long)round_half_up(seconds_from_now, SPAN_MIN_S),
                           S_SPAN_MINUTES);
    } else if (seconds_from_now < SPAN_DAY_S) {
        written = snprintf(tmp, sizeof tmp, "%lld%s",
                           (long long)round_half_up(seconds_from_now, SPAN_HOUR_S),
                           S_SPAN_HOURS);
    } else if (seconds_from_now < SPAN_MONTH_S) {
        written = snprintf(tmp, sizeof tmp, "%lld%s",
                           (long long)round_half_up(seconds_from_now, SPAN_DAY_S),
                           S_SPAN_DAYS);
    } else if (seconds_from_now < SPAN_YEAR_S) {
        written = snprintf(tmp, sizeof tmp, "%lld%s",
                           (long long)round_half_up(seconds_from_now, SPAN_MONTH_S),
                           S_SPAN_MONTHS);
    } else {
        written = snprintf(tmp, sizeof tmp, "%lld%s",
                           (long long)round_half_up(seconds_from_now, SPAN_YEAR_S),
                           S_SPAN_YEARS);
    }

    /* Unreachable: the widest answer is INT64_MAX seconds, "292471208678년 뒤",
     * 19 bytes and a terminator. Handled anyway because the alternative to
     * checking is trusting strlen() on a buffer snprintf may have truncated,
     * and a truncated UTF-8 tail draws as a tofu box in the middle of a date. */
    if (written < 0 || (size_t)written >= sizeof tmp) {
        if (dst && dst_size) dst[0] = '\0';
        return 0;
    }

    size_t len = (size_t)written;
    if (dst && dst_size) {
        if (len < dst_size) {
            memcpy(dst, tmp, len + 1);
        } else {
            /* Whole answer or nothing — never half of one. "" is already the
             * contract's spelling for "no date to show", so this degrades into
             * a case every caller must already render. */
            dst[0] = '\0';
        }
    }
    return len;
}
