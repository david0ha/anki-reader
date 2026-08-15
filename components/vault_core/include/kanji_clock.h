/*
 * kanji_clock.h — an honest wall clock for a board that has no RTC, and the
 * Korean wording for a span of time.
 *
 * Two things live here because offline study needs both and neither can be
 * bought from the hardware:
 *
 *   1. A three-tier clock. The EE04 has no RTC and no coin cell (docs/pinout.md
 *      — there is not even an I2C bus to hang one from), so the only time the
 *      board can ever know is time somebody hands it. Online that somebody is
 *      the proxy, which words every span against the *server's* clock and sends
 *      a string. Offline there is no proxy, so the board has to schedule
 *      against something — and the whole risk of this file is that "something"
 *      quietly becomes 1970.
 *
 *   2. kanji_relative_due(), which words a span exactly as the proxy does.
 *
 * Why this module is pure and injected
 * ------------------------------------
 * No esp_timer, no time(), no SNTP, no NVS. The anchor arrives through
 * kanji_clock_sync()/kanji_clock_restore() and the monotonic reading arrives as
 * an argument to kanji_clock_now(). The ESP glue owns all three sources; this
 * file owns the arithmetic and the honesty rules, which is the part worth
 * testing on a laptop where a decade can pass between two lines.
 *
 * The three tiers, and why the middle one is not a lie
 * ---------------------------------------------------
 * KANJI_CLOCK_UNKNOWN is a real state the UI renders, not a zero standing in
 * for a date. A board that has never been told the time and schedules anyway is
 * scheduling from the epoch, which makes *every* card in the 9,956-card catalog
 * overdue by half a century — the learner is handed an infinite review queue in
 * the exact order the flash happens to store it, and nothing on the glass says
 * why. Printing "시간 정보 없음" and refusing to compute a due date is the
 * smaller failure by an enormous margin, so kanji_clock_now() returns false and
 * writes nothing at all rather than hand back a plausible-looking int64.
 *
 * KANJI_CLOCK_APPROXIMATE is a persisted epoch plus however long this boot has
 * been running. Its error is the time the board spent powered off, which is
 * hours to days and always *behind* the truth. That is usable — not because the
 * error is small, but because of what FSRS actually asks: the intervals that
 * carry a study session are days and months wide, and a card due in 9 days does
 * not care whether the board thinks it is Tuesday morning or Tuesday night. The
 * intervals drift can genuinely spoil are the sub-day ones (다시 is ten
 * minutes), and the cost there is bounded and self-correcting: one relearning
 * card surfaces early or late by the drift, the learner grades it, and the
 * schedule moves on. Compare that with an UNKNOWN board pretending: unbounded,
 * silent, and wrong about every card at once.
 *
 * KANJI_CLOCK_TRUSTED is SNTP having answered this boot. Nothing here believes
 * a tier survives a power cut — a cold boot always starts at UNKNOWN and can
 * only climb back to APPROXIMATE from flash, because the drift a board
 * accumulates while it is *off* is unknowable by construction.
 *
 * The tier is the caller's to render and the caller's to reason about. This
 * module never blends them: an APPROXIMATE reading is not a worse TRUSTED
 * reading, it is a different claim, and the UI is expected to say so.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KANJI_CLOCK_UNKNOWN = 0,   /* never synced: no due dates, ever          */
    KANJI_CLOCK_APPROXIMATE,   /* a persisted anchor + uptime since boot    */
    KANJI_CLOCK_TRUSTED        /* SNTP succeeded this boot                  */
} kanji_clock_tier_t;

/* The window an epoch has to land in to be believed at all.
 *
 * SNTP's failure mode is not silence — it is a callback that fires with the
 * system clock still sitting at 1970, or with a packet whose transmit timestamp
 * decoded to garbage. A journal record read back from a half-erased flash
 * sector is the same shape of problem. Either one, believed, promotes the board
 * to TRUSTED with a wrong anchor, and TRUSTED is precisely the tier the UI stops
 * hedging about. So an epoch outside this window is not clamped and not
 * averaged: it is dropped, and the clock keeps whatever it already had.
 *
 * The floor is deliberately later than any plausible build of this firmware and
 * the ceiling deliberately absurd; the window has to reject 1970 and 2^40, not
 * adjudicate whether the board's idea of Tuesday is right. */
#define KANJI_CLOCK_EPOCH_MIN  INT64_C(1735689600)  /* 2025-01-01T00:00:00Z */
#define KANJI_CLOCK_EPOCH_MAX  INT64_C(4102444800)  /* 2100-01-01T00:00:00Z */

/* Caller-allocated; all fields are private. Two int64s and an enum, so it costs
 * nothing to keep one beside the session state and nothing to copy under a
 * mutex — which matters, because kanji_t is 4 KB and cannot be a local on a
 * 2 KB-frame budget, and this deliberately is not that. */
typedef struct {
    kanji_clock_tier_t tier;
    int64_t epoch_at_anchor;   /* wall-clock seconds observed at the anchor  */
    int64_t uptime_at_anchor;  /* the monotonic reading at that same instant */
} kanji_clock_t;

/* Back to UNKNOWN. This is the cold-boot state, and it is also the only way
 * back down: nothing else in this file ever demotes a clock. */
void kanji_clock_reset(kanji_clock_t *c);

/* SNTP answered. `epoch` is UTC seconds, `uptime_s` the monotonic reading taken
 * at the same moment.
 *
 * A sync always wins, including when it moves the clock *backwards*. That is
 * the deliberate choice and it is worth being explicit about: a backwards jump
 * is exactly the case where the old anchor was wrong — a restored floor that
 * undercounted the time the board was off, or an earlier sync against a bad
 * server — and refusing the correction to keep the reading monotonic would pin
 * the board to that lie until the next power cut. Callers holding a due date
 * computed from the old anchor must therefore be prepared for it to move; on
 * this board the only such caller recomputes per card per draw, so there is
 * nothing to invalidate.
 *
 * An epoch outside [KANJI_CLOCK_EPOCH_MIN, KANJI_CLOCK_EPOCH_MAX) leaves the
 * clock completely unchanged, tier included. */
void kanji_clock_sync(kanji_clock_t *c, int64_t epoch, int64_t uptime_s);

/* The persisted epoch, read from flash at cold boot. Anchors at uptime zero.
 *
 * The value is a *floor*, not a reading: it was written before the board lost
 * power and says only "time is at least this". Anchoring it at boot therefore
 * undercounts by the whole powered-off interval, which is the correct direction
 * to be wrong in — a board that thinks it is earlier than it is holds a card
 * back a few hours; a board that thinks it is later declares things due that
 * are not.
 *
 * Ignored unless the clock is UNKNOWN, and ignored for an implausible epoch. A
 * live anchor — even an APPROXIMATE one — has counted real seconds since it was
 * set, and flash by definition has not; letting a later restore overwrite it
 * would drag the reading backwards for no information gained. */
void kanji_clock_restore(kanji_clock_t *c, int64_t epoch);

/* Wall-clock seconds now, given the current monotonic reading.
 *
 * Returns false and writes NOTHING through `out_epoch` when the tier is
 * UNKNOWN (or on a NULL argument). A caller that ignores the return value gets
 * its own variable back untouched rather than a fabricated date, which is why
 * the false path deliberately does not zero the output: 0 *is* a date, and it
 * is the worst one.
 *
 * A monotonic source that goes backwards — a wrapped 32-bit tick counter, a
 * caller that reset its own timebase — contributes zero elapsed rather than
 * negative time. Letting the reading run backwards would un-due a card the
 * learner just saw graded, and the clock would then flap between two answers
 * every draw. The anchor is the honest floor in that case. */
bool kanji_clock_now(const kanji_clock_t *c, int64_t uptime_s, int64_t *out_epoch);

/* UNKNOWN for a NULL clock — an absent clock knows exactly as much as an
 * unsynced one. */
kanji_clock_tier_t kanji_clock_tier(const kanji_clock_t *c);

/* Bytes needed by the longest wording kanji_relative_due() can produce,
 * terminator included. The worst case is the year branch fed INT64_MAX:
 * 292471208678 is 12 digits, "년 뒤" is 7 bytes, so 20 — and this is 24 to
 * match KANJI_LABEL_MAX, the model field these spans are written into. The .c
 * static-asserts that relationship rather than trusting this sentence. */
#define KANJI_RELATIVE_DUE_MAX 24

/* Word a span the way the proxy words it: "곧", "10분 뒤", "9일 뒤", "2년 뒤".
 *
 * This is a port of tools/kanji_server.py's relative_due(), which is itself a
 * port of kanjis-front's relativeDue(), and the table lives in
 * docs/kanji-contract.md under "card.preview". All three must agree to the
 * character, because the board shows both: a card graded offline gets its
 * preview worded here, and the moment Wi-Fi returns the same card gets it
 * worded by the proxy. If the two round differently, a card the learner was
 * told was "30일 뒤" silently becomes "1개월 뒤" — the same instant, described
 * as if the schedule had changed under them. That is a bug report nobody can
 * reproduce, so the rounding is copied exactly rather than improved:
 *
 *   < 45 s -> 곧 | < 1 h -> N분 뒤 | < 1 d -> N시간 뒤
 *   < 30 d -> N일 뒤 | < 365 d -> N개월 뒤 | else N년 뒤
 *
 * Rounding is JavaScript's Math.round — half away from zero at every boundary,
 * never floor and never banker's — so 3599 seconds is "60분 뒤" and not
 * "59분 뒤" or "1시간 뒤". Done in integers; nothing here needs a double.
 *
 * A span already in the past (any negative value) is "곧", which is what the
 * reference does by construction: its first test is `seconds < 45`, and every
 * negative number passes it. An overdue card is due now, and "곧" is the only
 * one of these six spellings that is true of it.
 *
 * Returns the byte length of the wording, excluding the terminator — the number
 * snprintf would return. Unlike snprintf it never writes a partial answer: a
 * buffer too small for the whole span is left holding the empty string, because
 * half of "10분 뒤" is either a different span ("10분") or a severed UTF-8
 * sequence that draws as a tofu box, and "" is already the contract's spelling
 * for "no date to show". `dst` may be NULL (with dst_size 0) to measure. */
size_t kanji_relative_due(char *dst, size_t dst_size, int64_t seconds_from_now);

#ifdef __cplusplus
}
#endif
