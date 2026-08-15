/*
 * kanji_fsrs.h — FSRS-6 on the board, for when the proxy cannot be reached.
 *
 * Everything else in this firmware treats the learner's schedule as somebody
 * else's arithmetic: `kanji_fsrs_t` in kanji_model.h is a bag of *strings and
 * rounded integers* the proxy already computed, and docs/kanji-contract.md is
 * emphatic that the board has no RTC and no business wording a due date. That
 * is the right design while the laptop is awake. It leaves exactly one hole:
 * the learner on a train with the offline catalog in flash and no route to the
 * proxy. Their presses have to schedule something, and "something" cannot be a
 * guess — when the laptop comes back, the board's ratings get replayed into the
 * same py-fsrs the backend runs, and any drift between what the panel promised
 * and what the server records is a silently wrong review history.
 *
 * So this file is a transcription, not an implementation. It reproduces
 * py-fsrs 6.3.1 (FSRS-6) with the backend's three overrides — desired retention
 * 0.9, a single 10-minute learning step, a 10-minute relearning step, maximum
 * interval 36500 — bit-for-bit against the same double arithmetic. The host
 * test pins it to golden vectors printed by the backend's own interpreter, and
 * the generating Python is in the test file so they can be regenerated when
 * py-fsrs or the overrides move.
 *
 * Two deliberate departures from the library, both load-bearing:
 *
 *   - No fuzzing. py-fsrs jitters Review-state intervals by up to 5..15% from
 *     a RNG. The board must not: the four spans on the grade dock are a promise
 *     the learner reads *before* pressing, and a fuzzed commit would schedule
 *     something other than what the dock said. The backend already reaches the
 *     same conclusion for its preview path (see Scheduler._preview_fsrs in
 *     app/services/scheduler.py, "Anki shows the pre-fuzz interval"), the board
 *     has no entropy source worth the name before Wi-Fi is up, and a
 *     deterministic scheduler is one a host test can pin to the exact second.
 *
 *   - No clock. `kanji_fsrs_review()` takes `now_epoch` from the caller and
 *     never reads one. A board with no RTC gets its time from SNTP or from the
 *     proxy or not at all, and a scheduler that quietly called time(NULL) would
 *     do its worst work — scheduling from an epoch of 0 — in exactly the
 *     offline case this file exists for. Making the clock an argument moves
 *     that decision to the one layer that knows whether the time is real.
 *
 * There is no float anywhere in this API's *output*. Doubles live inside the
 * card struct because FSRS is defined in reals and the replayed history has to
 * match the server's to the last digit; everything the panel prints comes back
 * out of kanji_fsrs_stability_days() / kanji_fsrs_difficulty_pct() as the same
 * rounded integers, with the same -1-means-unscheduled convention, that
 * kanji_fsrs_t carries off the wire.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "kanji_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The card's scheduling state, with py-fsrs 6.x's own numbering.
 *
 * There is no NEW here, and that is the library's design rather than an
 * omission: py-fsrs 6.x deleted State.New, and a never-reviewed card is a
 * Learning-state card whose stability and difficulty are simply unset. The
 * backend adapter maps its own CardStateName.NEW onto State.Learning for the
 * same reason. On this side, `kanji_fsrs_card_t.scheduled` is what tells the
 * two apart — see below. */
typedef enum {
    KANJI_FSRS_LEARNING   = 1,
    KANJI_FSRS_REVIEW     = 2,
    KANJI_FSRS_RELEARNING = 3,
} kanji_fsrs_state_t;

/* One card's scheduler state. Small enough to keep by value, unlike kanji_t.
 *
 * `scheduled` is the C stand-in for py-fsrs's `stability is None`. It is false
 * only before the very first review, when there is no stability, no difficulty
 * and no last_review to compute retrievability from; every review sets all
 * three together, so one flag covers all of them. It is also what
 * kanji_fsrs_stability_days() and kanji_fsrs_difficulty_pct() answer -1 for. */
typedef struct {
    double stability;            /* days; >= 0.001 once scheduled            */
    double difficulty;           /* 1..10 once scheduled                     */
    kanji_fsrs_state_t state;
    int    step;                 /* index into the learning/relearning steps */
    int    reps;                 /* every review, including the failed ones  */
    int    lapses;               /* Again from REVIEW only — see below       */
    int64_t due_epoch;           /* UTC seconds; 0 on an unreviewed card     */
    int64_t last_review_epoch;   /* UTC seconds; meaningless when !scheduled */
    bool   scheduled;            /* false = never reviewed (S and D unset)   */
} kanji_fsrs_card_t;

/* A brand-new card: Learning, step 0, nothing scheduled, due immediately.
 *
 * due_epoch is 0 rather than "now" because this function has no clock, and 0 is
 * in the past for every clock the board will ever hold — a new card is due, and
 * the offline session has no reason to ask twice. */
void kanji_fsrs_init(kanji_fsrs_card_t *c);

/* Rebuild the card a persisted schedule describes, ready for its next review.
 *
 * kanji_state.h keeps three numbers and the rating that produced them. It does
 * not keep the state-machine position, the step, or when the review happened,
 * and this is where those come back — here rather than in the caller, because
 * recovering them means inverting this file's own transitions and step lengths,
 * and a second copy of that arithmetic somewhere else would be a copy that
 * drifts.
 *
 * `stability` and `difficulty` are the journal's numbers already converted back
 * to days and points; both zero means the journal has no schedule for the card
 * — never reviewed, or reviewed by a board whose clock was UNKNOWN — and the
 * result is exactly kanji_fsrs_init()'s card, carrying only reps and lapses.
 *
 * The recovered position is exact whenever the history allows it to be:
 *
 *   - reps <= 1 means the card was NEW before that single rating, so the
 *     transition is determined: Again and Hard leave it in Learning on the
 *     10-minute step, Good and Easy graduate it to Review.
 *   - beyond that, Good and Easy always graduate and Again always drops the
 *     card onto a step, so only Hard is genuinely ambiguous — it leaves a card
 *     where it already was. It is resolved as Review, because the two errors
 *     are not the same size: a mature card mistaken for a learning one would
 *     have its next 어려움 collapse from months to fifteen minutes, while a
 *     learning card mistaken for a mature one gets a day instead of fifteen
 *     minutes and one 다시 counted as a lapse.
 *
 * `last_review_epoch` is then the due date minus the interval that position
 * implies, which is the value that makes the card self-consistent: review it
 * again at its due time and the elapsed days are the ones the schedule
 * promised. A `due_epoch` of 0 on a scheduled card — the journal's spelling for
 * a due time that predates 1970 — is taken at face value and simply reads as
 * long overdue. */
void kanji_fsrs_restore(kanji_fsrs_card_t *c, double stability,
                        double difficulty, kanji_grade_t last_grade,
                        int reps, int lapses, int64_t due_epoch);

/* Apply one rating at `now_epoch` (UTC seconds), in place.
 *
 * Pure: no clock, no allocation, no RNG. `reps` always increments. `lapses`
 * increments only for Again *from the Review state*, which is the backend
 * adapter's rule verbatim (`is_lapse = rating == AGAIN and before.state ==
 * REVIEW`): failing a card you have never graduated is not a lapse, it is
 * still learning it, and counting it as one would inflate the number the FSRS
 * sheet prints on the very cards a learner is already struggling with. */
void kanji_fsrs_review(kanji_fsrs_card_t *c, kanji_grade_t g, int64_t now_epoch);

/* The due timestamp `g` would produce, without touching `c`.
 *
 * This is what the grade dock's four spans are worded from, so it must agree
 * with kanji_fsrs_review() to the second — it is literally the same code path
 * against a copy. */
int64_t kanji_fsrs_preview(const kanji_fsrs_card_t *c, kanji_grade_t g,
                           int64_t now_epoch);

/* Stability as the panel prints it: rounded whole days, -1 when unscheduled.
 *
 * The rounding is Math.round (a half goes up), not C's or Python's, because
 * that is what tools/kanji_server.py's js_round() does to the same number
 * before sending it. The board and the proxy have to agree about a card the
 * learner may see through either path on the same day. */
int kanji_fsrs_stability_days(const kanji_fsrs_card_t *c);

/* Difficulty as the panel prints it: py-fsrs's 1..10 rescaled to 0..100,
 * -1 when unscheduled. Same rounding, same reason. */
int kanji_fsrs_difficulty_pct(const kanji_fsrs_card_t *c);

#ifdef __cplusplus
}
#endif
