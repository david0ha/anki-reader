/*
 * test_kanji_fsrs.c — the offline scheduler, pinned to the backend's py-fsrs.
 *
 * Every stability, difficulty and due-offset literal below was PRINTED by the
 * backend's own interpreter driving the real py-fsrs 6.3.1 Scheduler. None of
 * them was computed by hand, and none should ever be edited to make a change
 * pass: if C and the goldens disagree, C is wrong, or py-fsrs moved and the
 * goldens must be regenerated from the script at the bottom of this file. A
 * scheduler that is subtly wrong is worse than one that is obviously broken —
 * it produces a plausible number for years, and the learner's review history
 * silently diverges from the server's the first time the board goes offline.
 *
 * WHY THE TOLERANCE IS WHAT IT IS. These are transcriptions of the same double
 * arithmetic in the same order on the same libm, so the expected disagreement
 * is a few ulps, not a few percent. 1e-9 relative is seven orders of magnitude
 * looser than that — tight enough that a mistyped weight or a reordered
 * multiplication cannot hide, loose enough that a libm's last bit cannot cause
 * a spurious failure.
 *
 * REGENERATING THE GOLDENS: see the script at the end of this file.
 */
#include <math.h>
#include <stdint.h>

#include "kanji_fsrs.h"
#include "th.h"

/* 2024-01-01T00:00:00Z — the T0 the generating script uses. Every `due` literal
 * below is an offset in seconds from this instant, exactly as printed. */
#define T0 ((int64_t)1704067200)

#define MINS(n)  ((int64_t)(n) * 60)
#define HOURS(n) ((int64_t)(n) * 3600)
#define DAYS(n)  ((int64_t)(n) * 86400)

/* ------------------------------------------------------------------------- *
 * Assertions. th.h has no double comparison — these ride its counters so the
 * failure total and TH_REPORT stay honest — and it has no way to say which of
 * a dozen identical-looking rows failed, which is what g_ctx is for.
 * ------------------------------------------------------------------------- */
static const char *g_ctx = "";

#define NEAR(got, want) do { \
    g_total++; \
    double _g = (got), _w = (want); \
    double _tol = 1e-9 * (fabs(_w) > 1.0 ? fabs(_w) : 1.0); \
    if (!(fabs(_g - _w) <= _tol)) { g_fail++; \
        printf("  FAIL %s:%d  [%s] %s: want %.17g got %.17g\n", \
               __FILE__, __LINE__, g_ctx, #got, _w, _g); } \
} while (0)

#define EQI(got, want) do { \
    g_total++; \
    long long _g = (long long)(got), _w = (long long)(want); \
    if (_g != _w) { g_fail++; \
        printf("  FAIL %s:%d  [%s] %s: want %lld got %lld\n", \
               __FILE__, __LINE__, g_ctx, #got, _w, _g); } \
} while (0)

/* One golden row: everything py-fsrs and the backend adapter say about the card
 * after a review. `due` is an offset from T0 because that is how the generating
 * script prints it, and re-basing it here by hand is exactly the transcription
 * error the goldens exist to catch. */
typedef struct {
    double             s;
    double             d;
    kanji_fsrs_state_t state;
    int                step;
    int64_t            due;      /* seconds after T0 */
    int                reps;
    int                lapses;
} want_t;

static void expect(const char *label, const kanji_fsrs_card_t *c, want_t w)
{
    g_ctx = label;
    NEAR(c->stability, w.s);
    NEAR(c->difficulty, w.d);
    EQI(c->state, w.state);
    EQI(c->step, w.step);
    EQI(c->due_epoch - T0, w.due);
    EQI(c->reps, w.reps);
    EQI(c->lapses, w.lapses);
    EQI(c->scheduled, 1);
    g_ctx = "";
}

/* A card mid-life, built by hand rather than by replaying reviews. The clamp
 * cases need starting points no ordinary sequence reaches (S already at the
 * floor, D a hair under the ceiling, S at 1e9), and the generating script
 * constructs the py-fsrs Card the same way. */
static kanji_fsrs_card_t at(double s, double d, kanji_fsrs_state_t st,
                            int step, int64_t last_review)
{
    kanji_fsrs_card_t c;
    kanji_fsrs_init(&c);
    c.stability         = s;
    c.difficulty        = d;
    c.state             = st;
    c.step              = step;
    c.reps              = 0;
    c.lapses            = 0;
    c.last_review_epoch = last_review;
    c.due_epoch         = last_review;
    c.scheduled         = true;
    return c;
}

/* ------------------------------------------------------------------------- *
 * A: each of the four grades on a brand-new card.
 *
 * These are the initial-stability and initial-difficulty formulas alone —
 * S0(g) = w[g-1], D0(g) = w4 - e^(w5*(g-1)) + 1 — plus the learning-step
 * scheduling. Easy's D0 comes out at -4.75 and lands on 1.0, which is the D
 * clamp firing on the very first review a learner can perform.
 * ------------------------------------------------------------------------- */
static void test_new_card_each_grade(void)
{
    kanji_fsrs_card_t c;

    /* Again on a new card: still Learning, back in 10 minutes, and NOT a lapse.
     * A card you have never graduated cannot be relapsed. */
    kanji_fsrs_init(&c);
    kanji_fsrs_review(&c, KANJI_GRADE_AGAIN, T0);
    expect("A/again", &c, (want_t){ 0.212, 6.4133,
           KANJI_FSRS_LEARNING, 0, MINS(10), 1, 0 });

    /* Hard on the only learning step is that step times 1.5 — 15 minutes.
     * (py-fsrs: `learning_steps[0] * 1.5` when there is exactly one step.) */
    kanji_fsrs_init(&c);
    kanji_fsrs_review(&c, KANJI_GRADE_HARD, T0);
    expect("A/hard", &c, (want_t){ 1.2931, 5.112170705601056,
           KANJI_FSRS_LEARNING, 0, MINS(15), 1, 0 });

    /* Good on the last (only) learning step graduates straight to Review. */
    kanji_fsrs_init(&c);
    kanji_fsrs_review(&c, KANJI_GRADE_GOOD, T0);
    expect("A/good", &c, (want_t){ 2.3065, 2.118103970459016,
           KANJI_FSRS_REVIEW, 0, DAYS(2), 1, 0 });

    /* Easy always graduates, and its D0 is clamped up from -4.75 to 1.0. */
    kanji_fsrs_init(&c);
    kanji_fsrs_review(&c, KANJI_GRADE_EASY, T0);
    expect("A/easy", &c, (want_t){ 8.2956, 1.0,
           KANJI_FSRS_REVIEW, 0, DAYS(8), 1, 0 });
}

/* ------------------------------------------------------------------------- *
 * B: the whole life of one card — new, learning, graduation, lapse, recovery.
 *
 * This is the sequence that catches an engine which gets each formula right in
 * isolation and then feeds the wrong one into the next step: every review here
 * takes a different branch, and each one's inputs are the previous one's
 * outputs. The Again at +6d is the ONLY lapse in the sequence.
 * ------------------------------------------------------------------------- */
static void test_life_of_a_card(void)
{
    kanji_fsrs_card_t c;
    kanji_fsrs_init(&c);

    kanji_fsrs_review(&c, KANJI_GRADE_AGAIN, T0);
    expect("B/1 again@T0", &c, (want_t){ 0.212, 6.4133,
           KANJI_FSRS_LEARNING, 0, MINS(10), 1, 0 });

    /* Ten minutes later: same calendar day, so the SHORT-TERM stability path,
     * then Good on the last learning step graduates. Stability is 0.2467, which
     * rounds to a zero-day interval — and the interval floor turns it into one
     * day. A due date in the past is not a schedule. */
    kanji_fsrs_review(&c, KANJI_GRADE_GOOD, T0 + MINS(10));
    expect("B/2 good@+10m", &c, (want_t){ 0.24668918777567272, 6.402115069296838,
           KANJI_FSRS_REVIEW, 0, MINS(10) + DAYS(1), 2, 0 });

    /* Three days on: Review, elapsed >= 1 day, so the long-term recall path
     * with a real retrievability. */
    kanji_fsrs_review(&c, KANJI_GRADE_HARD, T0 + DAYS(3));
    expect("B/3 hard@+3d", &c, (want_t){ 1.682653906544443, 7.596784690858309,
           KANJI_FSRS_REVIEW, 0, DAYS(5), 3, 0 });

    /* Again FROM REVIEW. This is a lapse, and the only one in the sequence:
     * relearning-step scheduling (+10m) and the forget-stability formula. */
    kanji_fsrs_review(&c, KANJI_GRADE_AGAIN, T0 + DAYS(6));
    expect("B/4 again@+6d (LAPSE)", &c, (want_t){ 0.4921627427722112,
           9.195307839046066,
           KANJI_FSRS_RELEARNING, 0, DAYS(6) + MINS(10), 4, 1 });

    /* Good on the last relearning step puts it back into Review. */
    kanji_fsrs_review(&c, KANJI_GRADE_GOOD, T0 + DAYS(6) + MINS(10));
    expect("B/5 good@+6d10m", &c, (want_t){ 0.5418201262703002,
           9.181340900503857,
           KANJI_FSRS_REVIEW, 0, DAYS(6) + MINS(10) + DAYS(1), 5, 1 });
}

/* ------------------------------------------------------------------------- *
 * C: a second review on the same day (the short-term stability path).
 *
 * The tell is that Good's short-term increase is clamped up to 1.0, so the
 * stability does NOT move at all while the difficulty does. An engine that
 * routed this through the recall formula instead would grow the stability and
 * look entirely reasonable doing it.
 * ------------------------------------------------------------------------- */
static void test_same_day_second_review(void)
{
    kanji_fsrs_card_t c;
    kanji_fsrs_init(&c);

    kanji_fsrs_review(&c, KANJI_GRADE_GOOD, T0);
    expect("C/1 good@T0", &c, (want_t){ 2.3065, 2.118103970459016,
           KANJI_FSRS_REVIEW, 0, DAYS(2), 1, 0 });

    kanji_fsrs_review(&c, KANJI_GRADE_GOOD, T0 + HOURS(1));
    expect("C/2 good@+1h (stability unmoved)", &c,
           (want_t){ 2.3065, 2.111214235785395,
           KANJI_FSRS_REVIEW, 0, HOURS(1) + DAYS(2), 2, 0 });

    kanji_fsrs_review(&c, KANJI_GRADE_AGAIN, T0 + HOURS(2));
    expect("C/3 again@+2h (LAPSE)", &c,
           (want_t){ 0.7750839828558984, 7.392238132342694,
           KANJI_FSRS_RELEARNING, 0, HOURS(2) + MINS(10), 3, 1 });
}

/* ------------------------------------------------------------------------- *
 * D: a long gap, where retrievability is well below 1.
 *
 * A year after a 2.3-day stability the card is at R = 0.4589. That number is
 * the whole point of the exercise: it multiplies into every one of the four
 * outcomes below, and an engine that mishandled the DECAY sign or the integer
 * day count would still produce four plausible-looking intervals.
 * ------------------------------------------------------------------------- */
static void test_long_gap(void)
{
    kanji_fsrs_card_t base;
    kanji_fsrs_init(&base);
    kanji_fsrs_review(&base, KANJI_GRADE_GOOD, T0);

    const int64_t later = T0 + DAYS(365);
    kanji_fsrs_card_t c;

    c = base; kanji_fsrs_review(&c, KANJI_GRADE_AGAIN, later);
    expect("D/again@+365d", &c, (want_t){ 1.2768129429226878,
           7.394502741279718,
           KANJI_FSRS_RELEARNING, 0, DAYS(365) + MINS(10), 2, 1 });

    c = base; kanji_fsrs_review(&c, KANJI_GRADE_HARD, later);
    expect("D/hard@+365d", &c, (want_t){ 39.82687216297327,
           4.752858488532557,
           KANJI_FSRS_REVIEW, 0, DAYS(365) + DAYS(40), 2, 0 });

    c = base; kanji_fsrs_review(&c, KANJI_GRADE_GOOD, later);
    expect("D/good@+365d", &c, (want_t){ 64.69488071661665,
           2.111214235785395,
           KANJI_FSRS_REVIEW, 0, DAYS(365) + DAYS(65), 2, 0 });

    c = base; kanji_fsrs_review(&c, KANJI_GRADE_EASY, later);
    expect("D/easy@+365d", &c, (want_t){ 119.15369824415133, 1.0,
           KANJI_FSRS_REVIEW, 0, DAYS(365) + DAYS(119), 2, 0 });
}

/* ------------------------------------------------------------------------- *
 * E: Again from Learning is not a lapse, however many times it happens.
 *
 * The counterpart to B/4. `lapses` is what the FSRS sheet prints as 실패, and
 * counting a card the learner has never graduated would inflate that number on
 * exactly the cards they are already struggling with. The rule comes from the
 * backend adapter, not from py-fsrs, which does not track lapses at all.
 * ------------------------------------------------------------------------- */
static void test_again_from_learning_is_not_a_lapse(void)
{
    kanji_fsrs_card_t c;
    kanji_fsrs_init(&c);

    kanji_fsrs_review(&c, KANJI_GRADE_AGAIN, T0);
    expect("E/1 again", &c, (want_t){ 0.212, 6.4133,
           KANJI_FSRS_LEARNING, 0, MINS(10), 1, 0 });

    kanji_fsrs_review(&c, KANJI_GRADE_AGAIN, T0 + MINS(10));
    expect("E/2 again", &c, (want_t){ 0.08335671711031604, 8.806304468856837,
           KANJI_FSRS_LEARNING, 0, MINS(20), 2, 0 });

    /* Hard inside Learning keeps the step and schedules 1.5 steps out. */
    kanji_fsrs_review(&c, KANJI_GRADE_HARD, T0 + MINS(20));
    expect("E/3 hard", &c, (want_t){ 0.05995495866943587, 9.192797649512254,
           KANJI_FSRS_LEARNING, 0, MINS(35), 3, 0 });
}

/* ------------------------------------------------------------------------- *
 * F: inside Relearning — Again holds step 0, Hard is 1.5 steps, Easy graduates.
 *
 * None of these is a lapse: the card was already out of Review when they
 * happened.
 * ------------------------------------------------------------------------- */
static void test_relearning_steps(void)
{
    kanji_fsrs_card_t c;
    kanji_fsrs_init(&c);

    kanji_fsrs_review(&c, KANJI_GRADE_EASY, T0);
    expect("F/1 easy@T0", &c, (want_t){ 8.2956, 1.0,
           KANJI_FSRS_REVIEW, 0, DAYS(8), 1, 0 });

    kanji_fsrs_review(&c, KANJI_GRADE_AGAIN, T0 + DAYS(8));
    expect("F/2 again@+8d (LAPSE)", &c, (want_t){ 1.3886324609821161,
           7.0269895692968385,
           KANJI_FSRS_RELEARNING, 0, DAYS(8) + MINS(10), 2, 1 });

    kanji_fsrs_review(&c, KANJI_GRADE_AGAIN, T0 + DAYS(8) + MINS(10));
    expect("F/3 again@+8d10m (not a lapse)", &c,
           (want_t){ 0.4824837699764583, 9.008020057195637,
           KANJI_FSRS_RELEARNING, 0, DAYS(8) + MINS(20), 3, 1 });

    kanji_fsrs_review(&c, KANJI_GRADE_HARD, T0 + DAYS(8) + MINS(20));
    expect("F/4 hard@+8d20m", &c,
           (want_t){ 0.30916615443673906, 9.326705856997966,
           KANJI_FSRS_RELEARNING, 0, DAYS(8) + MINS(35), 4, 1 });

    kanji_fsrs_review(&c, KANJI_GRADE_EASY, T0 + DAYS(8) + MINS(35));
    expect("F/5 easy@+8d35m", &c,
           (want_t){ 0.6037111880495071, 9.086950699210588,
           KANJI_FSRS_REVIEW, 0, DAYS(8) + MINS(35) + DAYS(1), 5, 1 });
}

/* ------------------------------------------------------------------------- *
 * G: the clamps.
 *
 * S floor 0.001, D floor 1 and ceiling 10, interval floor 1 day and ceiling
 * 36500. Each row starts from a hand-built card, because no ordinary sequence
 * reaches these edges — which is precisely why they are worth pinning: the code
 * that enforces them runs approximately never, so nothing else would notice it
 * being deleted.
 * ------------------------------------------------------------------------- */
static void test_clamps(void)
{
    kanji_fsrs_card_t c;

    /* Stability floor: a card already at 0.001, failed a month later. Both the
     * long-term and the short-term forget formulas come out under the floor. */
    c = at(0.001, 10.0, KANJI_FSRS_REVIEW, 0, T0);
    kanji_fsrs_review(&c, KANJI_GRADE_AGAIN, T0 + DAYS(30));
    expect("G/S floor", &c, (want_t){ 0.001, 9.985228369296838,
           KANJI_FSRS_RELEARNING, 0, DAYS(30) + MINS(10), 1, 1 });

    /* Difficulty ceiling: Again on D = 9.99 stays inside [1,10]. The linear
     * damping term is (10 - D)*delta/9, so difficulty approaches 10 and never
     * crosses it — the clamp is the belt to that braces. */
    c = at(5.0, 9.99, KANJI_FSRS_REVIEW, 0, T0);
    kanji_fsrs_review(&c, KANJI_GRADE_AGAIN, T0 + DAYS(1));
    expect("G/D ceiling", &c, (want_t){ 0.8105376868224645, 9.981941437296838,
           KANJI_FSRS_RELEARNING, 0, DAYS(1) + MINS(10), 1, 1 });

    /* Difficulty floor: Easy on D = 1.01 computes -2.008 and lands on 1.0. */
    c = at(5.0, 1.01, KANJI_FSRS_REVIEW, 0, T0);
    kanji_fsrs_review(&c, KANJI_GRADE_EASY, T0 + DAYS(1));
    expect("G/D floor", &c, (want_t){ 15.194614432776234, 1.0,
           KANJI_FSRS_REVIEW, 0, DAYS(1) + DAYS(15), 1, 0 });

    /* Interval ceiling: a stability of a billion days is still 36500 days out,
     * and stability_days saturates at the proxy's own 99999 rather than
     * overflowing the integer the panel prints. */
    c = at(1.0e9, 1.0, KANJI_FSRS_REVIEW, 0, T0);
    kanji_fsrs_review(&c, KANJI_GRADE_EASY, T0 + DAYS(1));
    expect("G/interval ceiling", &c, (want_t){ 1000000000.4640639, 1.0,
           KANJI_FSRS_REVIEW, 0, DAYS(1) + DAYS(36500), 1, 0 });
    g_ctx = "G/interval ceiling";
    EQI(kanji_fsrs_stability_days(&c), 99999);
    g_ctx = "";

    /* Interval floor is already pinned by B/2, where a stability of 0.2467
     * rounds to zero days and is scheduled one day out instead. */
}

/* ------------------------------------------------------------------------- *
 * H: the two roundings are different roundings, and the difference is visible.
 *
 * The INTERVAL uses Python's round(), which is round-half-to-EVEN: a stability
 * of exactly 2.5 days schedules 2 days, and 3.5 schedules 4. The number the
 * PANEL prints uses Math.round (half up), because that is what
 * tools/kanji_server.py's js_round() does to the same stability before sending
 * it — so the same card reads "3일" on the glass while being due in 2 days, and
 * that is correct rather than a bug. Getting these the same way round is a
 * one-line change that nothing else in the system would notice.
 *
 * Reaching an exactly-2.5 stability takes a same-day Good, whose short-term
 * increase clamps to 1.0 and therefore leaves the stability untouched.
 * ------------------------------------------------------------------------- */
static void test_interval_rounding_is_half_to_even(void)
{
    kanji_fsrs_card_t c;

    c = at(2.5, 5.0, KANJI_FSRS_REVIEW, 0, T0);
    kanji_fsrs_review(&c, KANJI_GRADE_GOOD, T0 + HOURS(6));
    expect("H/S=2.5 -> 2 days", &c, (want_t){ 2.5, 4.9902283692968386,
           KANJI_FSRS_REVIEW, 0, HOURS(6) + DAYS(2), 1, 0 });
    g_ctx = "H/S=2.5";
    EQI(kanji_fsrs_stability_days(&c), 3);   /* js_round(2.5) == 3, not 2 */
    g_ctx = "";

    c = at(3.5, 5.0, KANJI_FSRS_REVIEW, 0, T0);
    kanji_fsrs_review(&c, KANJI_GRADE_GOOD, T0 + HOURS(6));
    expect("H/S=3.5 -> 4 days", &c, (want_t){ 3.5, 4.9902283692968386,
           KANJI_FSRS_REVIEW, 0, HOURS(6) + DAYS(4), 1, 0 });
    g_ctx = "H/S=3.5";
    EQI(kanji_fsrs_stability_days(&c), 4);
    g_ctx = "";
}

/* ------------------------------------------------------------------------- *
 * I: "same day" is a floor of whole days, not a wall-clock date and not hours.
 *
 * 23 hours later is still the short-term path (stability frozen); 25 hours
 * later is the long-term one. And a review timestamped BEFORE the last one —
 * which a board whose SNTP just corrected a drifting clock will produce — must
 * floor to a negative day count and take the short-term path, not compute a
 * retrievability from a negative elapsed time.
 * ------------------------------------------------------------------------- */
static void test_elapsed_days_floor(void)
{
    const kanji_fsrs_card_t base = at(10.0, 5.0, KANJI_FSRS_REVIEW, 0, T0);
    kanji_fsrs_card_t c;

    c = base; kanji_fsrs_review(&c, KANJI_GRADE_GOOD, T0 + HOURS(23));
    expect("I/+23h is same-day", &c, (want_t){ 10.0, 4.9902283692968386,
           KANJI_FSRS_REVIEW, 0, HOURS(23) + DAYS(10), 1, 0 });

    c = base; kanji_fsrs_review(&c, KANJI_GRADE_GOOD, T0 + HOURS(25));
    expect("I/+25h is not", &c, (want_t){ 13.047176010567354,
           4.9902283692968386,
           KANJI_FSRS_REVIEW, 0, HOURS(25) + DAYS(13), 1, 0 });

    c = base; kanji_fsrs_review(&c, KANJI_GRADE_GOOD, T0 - HOURS(1));
    expect("I/clock went backwards", &c, (want_t){ 10.0, 4.9902283692968386,
           KANJI_FSRS_REVIEW, 0, -HOURS(1) + DAYS(10), 1, 0 });
}

/* ------------------------------------------------------------------------- *
 * J: preview is the commit, without the commit.
 *
 * The four spans on the grade dock are read BEFORE the learner presses, so a
 * preview that disagreed with the review it previews would make the board lie
 * about what a button does. Asserting the identity is worth more than
 * re-pinning the same goldens: it is the property that must hold for every
 * card, and it is what makes the no-fuzzing decision observable.
 * ------------------------------------------------------------------------- */
static void test_preview_matches_review(void)
{
    kanji_fsrs_card_t base;
    kanji_fsrs_init(&base);
    kanji_fsrs_review(&base, KANJI_GRADE_GOOD, T0);
    kanji_fsrs_review(&base, KANJI_GRADE_GOOD, T0 + DAYS(2));
    expect("J/base", &base, (want_t){ 10.964332335820698, 2.111214235785395,
           KANJI_FSRS_REVIEW, 0, DAYS(2) + DAYS(11), 2, 0 });

    /* The goldens for the four previews, printed by the same script. */
    const int64_t at9 = T0 + DAYS(9);
    const struct { kanji_grade_t g; int64_t due; } want[] = {
        { KANJI_GRADE_AGAIN, DAYS(9) + MINS(10) },
        { KANJI_GRADE_HARD,  DAYS(9) + DAYS(26) },
        { KANJI_GRADE_GOOD,  DAYS(9) + DAYS(36) },
        { KANJI_GRADE_EASY,  DAYS(9) + DAYS(58) },
    };

    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        g_ctx = "J/preview";
        const kanji_fsrs_card_t before = base;
        const int64_t got = kanji_fsrs_preview(&base, want[i].g, at9);
        EQI(got, T0 + want[i].due);

        /* Preview must not have touched the card. */
        NEAR(base.stability, before.stability);
        NEAR(base.difficulty, before.difficulty);
        EQI(base.due_epoch, before.due_epoch);
        EQI(base.reps, before.reps);
        EQI(base.lapses, before.lapses);

        /* ...and committing the same grade must land on the same second. */
        kanji_fsrs_card_t c = base;
        kanji_fsrs_review(&c, want[i].g, at9);
        EQI(c.due_epoch, got);
        g_ctx = "";
    }
}

/* ------------------------------------------------------------------------- *
 * K: the two integers the panel actually prints, and the -1 that is not zero.
 *
 * kanji_model.h and docs/kanji-contract.md both insist that stability_days: 0
 * and stability_days: -1 are different facts — a card due today versus a card
 * the scheduler has never seen — and the FSRS sheet prints them as 0일 and —.
 * An offline scheduler that reported 0 for a new card would make the board
 * claim to know something it does not.
 * ------------------------------------------------------------------------- */
static void test_printed_integers(void)
{
    kanji_fsrs_card_t c;
    g_ctx = "K";

    kanji_fsrs_init(&c);
    EQI(kanji_fsrs_stability_days(&c), -1);
    EQI(kanji_fsrs_difficulty_pct(&c), -1);
    EQI(c.state, KANJI_FSRS_LEARNING);
    EQI(c.step, 0);
    EQI(c.reps, 0);
    EQI(c.lapses, 0);
    EQI(c.scheduled, 0);

    /* One Again, and both become real numbers. S = 0.212 rounds to 0 days —
     * which is a legitimate answer, and NOT the same as "unscheduled". */
    kanji_fsrs_review(&c, KANJI_GRADE_AGAIN, T0);
    EQI(kanji_fsrs_stability_days(&c), 0);
    EQI(kanji_fsrs_difficulty_pct(&c), 60);   /* (6.4133-1)/9*100 = 60.15 */

    /* D = 1.0 is the floor and prints as 0%, not as "unknown". */
    kanji_fsrs_init(&c);
    kanji_fsrs_review(&c, KANJI_GRADE_EASY, T0);
    EQI(kanji_fsrs_stability_days(&c), 8);    /* js_round(8.2956) */
    EQI(kanji_fsrs_difficulty_pct(&c), 0);

    /* D = 10 is the ceiling and prints as 100%. */
    c = at(4.0, 10.0, KANJI_FSRS_REVIEW, 0, T0);
    EQI(kanji_fsrs_difficulty_pct(&c), 100);
    EQI(kanji_fsrs_stability_days(&c), 4);

    /* Math.round, not banker's: 2.5 days prints 3. (See H.) */
    c = at(2.5, 5.5, KANJI_FSRS_REVIEW, 0, T0);
    EQI(kanji_fsrs_stability_days(&c), 3);
    EQI(kanji_fsrs_difficulty_pct(&c), 50);

    g_ctx = "";
}

/* ------------------------------------------------------------------------- *
 * L: nothing here dereferences NULL.
 *
 * UiTask calls this from a button handler. A crash there is a board that has to
 * be power-cycled with the learner's rating lost, which is a worse outcome than
 * any wrong interval.
 * ------------------------------------------------------------------------- */
static void test_null_safety(void)
{
    g_ctx = "L";
    kanji_fsrs_init(NULL);
    kanji_fsrs_review(NULL, KANJI_GRADE_GOOD, T0);
    EQI(kanji_fsrs_preview(NULL, KANJI_GRADE_GOOD, T0), T0);
    EQI(kanji_fsrs_stability_days(NULL), -1);
    EQI(kanji_fsrs_difficulty_pct(NULL), -1);

    /* An out-of-range grade must not index past the weight table. */
    kanji_fsrs_card_t c;
    kanji_fsrs_init(&c);
    kanji_fsrs_review(&c, (kanji_grade_t)0, T0);
    EQI(c.reps, 0);
    EQI(c.scheduled, 0);
    kanji_fsrs_review(&c, (kanji_grade_t)9, T0);
    EQI(c.reps, 0);
    EQI(c.scheduled, 0);
    g_ctx = "";
}

int main(void)
{
    test_new_card_each_grade();
    test_life_of_a_card();
    test_same_day_second_review();
    test_long_gap();
    test_again_from_learning_is_not_a_lapse();
    test_relearning_steps();
    test_clamps();
    test_interval_rounding_is_half_to_even();
    test_elapsed_days_floor();
    test_preview_matches_review();
    test_printed_integers();
    test_null_safety();
    TH_REPORT("test_kanji_fsrs");
}

/* ===========================================================================
 * REGENERATING THE GOLDENS
 *
 * Run this against the backend's own virtualenv — the point is that the numbers
 * come from the py-fsrs the server will actually replay these ratings into, not
 * from a pip-installed copy that may be a different release:
 *
 *   cd /Users/ggrrm/Documents/kanjis-backend && .venv/bin/python - <<'PY'
 *   from datetime import datetime, timedelta, timezone
 *   from fsrs import Card, Rating, Scheduler, State
 *
 *   T0 = datetime(2024, 1, 1, tzinfo=timezone.utc); E0 = int(T0.timestamp())
 *   # The three overrides from app/core/config.py + app/services/scheduler.py.
 *   # relearning_steps and maximum_interval are py-fsrs defaults, spelled out
 *   # so this script does not silently follow the library if they ever move.
 *   S = Scheduler(desired_retention=0.9, enable_fuzzing=False,
 *                 learning_steps=(timedelta(minutes=10),),
 *                 relearning_steps=(timedelta(minutes=10),),
 *                 maximum_interval=36500)
 *   ST = {State.Learning: "LEARNING", State.Review: "REVIEW",
 *         State.Relearning: "RELEARNING"}
 *   NM = {Rating.Again: "AGAIN", Rating.Hard: "HARD",
 *         Rating.Good: "GOOD", Rating.Easy: "EASY"}
 *
 *   def show(label, c):
 *       print("  %-38s S=%-22r D=%-22r %-10s step=%-5s due=+%d"
 *             % (label, c.stability, c.difficulty, ST[c.state], c.step,
 *                int(c.due.timestamp()) - E0))
 *
 *   def fresh():
 *       return Card(card_id=0)          # Learning, step 0, S/D None
 *
 *   print("A: each grade on a new card")
 *   for r in (Rating.Again, Rating.Hard, Rating.Good, Rating.Easy):
 *       show("new + " + NM[r], S.review_card(fresh(), r, T0)[0])
 *
 *   print("B: new -> learning -> review -> lapse -> recovery")
 *   c = fresh()
 *   for r, w in [(Rating.Again, T0),
 *                (Rating.Good,  T0 + timedelta(minutes=10)),
 *                (Rating.Hard,  T0 + timedelta(days=3)),
 *                (Rating.Again, T0 + timedelta(days=6)),
 *                (Rating.Good,  T0 + timedelta(days=6, minutes=10))]:
 *       c, _ = S.review_card(c, r, w); show("B %s @ %s" % (NM[r], w - T0), c)
 *
 *   print("C: same-day second review (short-term stability)")
 *   c = fresh()
 *   c, _ = S.review_card(c, Rating.Good, T0);                    show("C good@T0", c)
 *   c, _ = S.review_card(c, Rating.Good, T0 + timedelta(hours=1)); show("C good@+1h", c)
 *   d, _ = S.review_card(c, Rating.Again, T0 + timedelta(hours=2)); show("C again@+2h", d)
 *
 *   print("D: a long gap")
 *   base = S.review_card(fresh(), Rating.Good, T0)[0]
 *   for r in (Rating.Again, Rating.Hard, Rating.Good, Rating.Easy):
 *       show("D " + NM[r], S.review_card(base, r, T0 + timedelta(days=365))[0])
 *   print("  R(+365d) =", S.get_card_retrievability(base, T0 + timedelta(days=365)))
 *
 *   print("E: Again from Learning, twice, then Hard")
 *   c = fresh()
 *   for r, w in [(Rating.Again, T0), (Rating.Again, T0 + timedelta(minutes=10)),
 *                (Rating.Hard, T0 + timedelta(minutes=20))]:
 *       c, _ = S.review_card(c, r, w); show("E " + NM[r], c)
 *
 *   print("F: inside Relearning")
 *   c = fresh()
 *   for r, w in [(Rating.Easy,  T0),
 *                (Rating.Again, T0 + timedelta(days=8)),
 *                (Rating.Again, T0 + timedelta(days=8, minutes=10)),
 *                (Rating.Hard,  T0 + timedelta(days=8, minutes=20)),
 *                (Rating.Easy,  T0 + timedelta(days=8, minutes=35))]:
 *       c, _ = S.review_card(c, r, w); show("F " + NM[r], c)
 *
 *   print("G: clamps")
 *   def hand(s, d):
 *       return Card(card_id=0, state=State.Review, step=None, stability=s,
 *                   difficulty=d, due=T0, last_review=T0)
 *   show("G S floor",   S.review_card(hand(0.001, 10.0), Rating.Again, T0 + timedelta(days=30))[0])
 *   show("G D ceiling", S.review_card(hand(5.0, 9.99),   Rating.Again, T0 + timedelta(days=1))[0])
 *   show("G D floor",   S.review_card(hand(5.0, 1.01),   Rating.Easy,  T0 + timedelta(days=1))[0])
 *   show("G ivl ceil",  S.review_card(hand(1e9, 1.0),    Rating.Easy,  T0 + timedelta(days=1))[0])
 *
 *   print("H: interval rounding is round-half-to-even")
 *   for s in (2.5, 3.5):
 *       show("H S=%r" % s, S.review_card(hand(s, 5.0), Rating.Good, T0 + timedelta(hours=6))[0])
 *
 *   print("I: elapsed days floor")
 *   for h in (23, 25, -1):
 *       show("I +%dh" % h, S.review_card(hand(10.0, 5.0), Rating.Good, T0 + timedelta(hours=h))[0])
 *
 *   print("J: previews on a mid-life card")
 *   b = S.review_card(fresh(), Rating.Good, T0)[0]
 *   b, _ = S.review_card(b, Rating.Good, T0 + timedelta(days=2)); show("J base", b)
 *   for r in (Rating.Again, Rating.Hard, Rating.Good, Rating.Easy):
 *       show("J " + NM[r], S.review_card(b, r, T0 + timedelta(days=9))[0])
 *   PY
 * ======================================================================== */
