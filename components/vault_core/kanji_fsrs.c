/*
 * kanji_fsrs.c — FSRS-6, transcribed from py-fsrs 6.3.1. See kanji_fsrs.h for
 * why the board carries its own copy at all.
 *
 * THIS FILE IS A TRANSCRIPTION. Every formula below is the same expression, in
 * the same order, with the same parentheses, as its counterpart in
 *   .venv/lib/python3.12/site-packages/fsrs/scheduler.py
 * of the kanjis-backend checkout, and the state machine is the same match/case.
 * That is not stylistic fussiness: FSRS's stability recurrence feeds its own
 * output back in for the life of the card, so a reassociated product that is
 * one ulp off today is visibly off after twenty reviews, and the divergence
 * appears as a schedule that is merely a bit different rather than as anything
 * that looks like a bug. Reorder nothing to make it read better.
 *
 * The one place the two intentionally differ is fuzzing, which this file does
 * not have. kanji_fsrs.h explains why at length.
 *
 * Doubles are allowed here and nowhere else in this component (CLAUDE.md), for
 * the reason the exception exists: FSRS is defined over the reals, and the
 * board's job is to agree with a server that computes it in float64.
 */
#include "kanji_fsrs.h"

#include <math.h>

/* No fused multiply-add, where the compiler will let us say so.
 *
 * Every formula below is of the shape `a + b*c`, which a compiler is free to
 * contract into a single FMA — one rounding instead of two. That is normally a
 * gift, and here it is the only thing standing between this file and bit-for-
 * bit agreement with the server. Measured: a randomized 4,629-review
 * differential run against the backend's own py-fsrs left 270 reviews differing
 * in the last few bits of stability with contraction on, and zero — every
 * review identical to the bit — with it off.
 *
 * The drift is ~1e-15 relative, which cannot move a rounded day count in any
 * realistic lifetime of a card, and test_kanji_fsrs.c passes either way. So
 * this is a belt, not a brace, and it is deliberately not worth a build error
 * to have: GCC does not implement the standard pragma and rejects it outright
 * under -Werror=unknown-pragmas, which is how the ESP-IDF cross compiler
 * (xtensa-esp-elf-gcc 14.2) sees it. Omitting it there costs nothing real
 * anyway — the ESP32-S3's FPU is single-precision, so every double here is
 * already software-emulated and there is no double FMA to contract into. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

/* --------------------------------------------------------------------------
 * The model.
 *
 * DEFAULT_PARAMETERS from fsrs/scheduler.py, verbatim and in order. The backend
 * never overrides them — app/services/scheduler.py passes only
 * desired_retention, enable_fuzzing and learning_steps — so these twenty-one
 * numbers are what the server is running.
 *
 * A single mistyped digit here is the failure mode this whole file is written
 * to avoid: it would not crash, would not look wrong, and would quietly put the
 * board on a different schedule from the server forever. test_kanji_fsrs.c pins
 * all of them against golden vectors printed by the backend's own interpreter.
 * -------------------------------------------------------------------------- */
#define W_COUNT 21
static const double W[W_COUNT] = {
    0.212,   /*  0  S0(Again)                                     */
    1.2931,  /*  1  S0(Hard)                                      */
    2.3065,  /*  2  S0(Good)                                      */
    8.2956,  /*  3  S0(Easy)                                      */
    6.4133,  /*  4  D0 base                                       */
    0.8334,  /*  5  D0 exponent                                   */
    3.0194,  /*  6  difficulty delta per rating step              */
    0.001,   /*  7  mean-reversion weight                         */
    1.8722,  /*  8  recall stability: scale                       */
    0.1666,  /*  9  recall stability: stability exponent          */
    0.796,   /* 10  recall stability: retrievability exponent     */
    1.4835,  /* 11  forget stability: scale                       */
    0.0614,  /* 12  forget stability: difficulty exponent         */
    0.2629,  /* 13  forget stability: stability exponent          */
    1.6483,  /* 14  forget stability: retrievability exponent     */
    0.6014,  /* 15  hard penalty                                  */
    1.8729,  /* 16  easy bonus                                    */
    0.5425,  /* 17  short-term stability: scale                   */
    0.0912,  /* 18  short-term stability: rating offset           */
    0.0658,  /* 19  short-term stability: stability exponent      */
    0.1542,  /* 20  decay (FSRS_DEFAULT_DECAY)                    */
};

/* py-fsrs's own bounds, from the same file. */
#define STABILITY_MIN  0.001
#define MIN_DIFFICULTY 1.0
#define MAX_DIFFICULTY 10.0

/* The backend's three overrides (app/core/config.py, app/services/scheduler.py).
 * relearning_steps and maximum_interval are py-fsrs defaults the backend does
 * not touch; they are spelled out rather than implied so that this file does not
 * silently follow the library if a future release changes its own defaults. */
#define DESIRED_RETENTION 0.9
#define MAXIMUM_INTERVAL  36500

#define SECS_PER_DAY ((int64_t)86400)

/* A single 10-minute learning step and a single 10-minute relearning step. The
 * comment in config.py says what this buys: Good on a new card graduates
 * immediately, while Again re-shows it in ten minutes.
 *
 * These are arrays rather than constants because the step-scheduling code below
 * is a transcription of py-fsrs's, which indexes a list — writing it against
 * one hardcoded value would make it unrecognisable next to the source it has to
 * be checked against. */
static const int64_t LEARNING_STEPS[]   = { 600 };
static const int64_t RELEARNING_STEPS[] = { 600 };
#define N_LEARNING   ((int)(sizeof LEARNING_STEPS / sizeof LEARNING_STEPS[0]))
#define N_RELEARNING ((int)(sizeof RELEARNING_STEPS / sizeof RELEARNING_STEPS[0]))

/* Python's math.e, as the exact double Python holds. See e_pow() below. */
#define M_E_PY 2.718281828459045

/* --------------------------------------------------------------------------
 * Scalar helpers.
 * -------------------------------------------------------------------------- */

/* py-fsrs writes every exponential as `math.e ** x`, which is pow(e, x) and not
 * exp(x). The two agree to within an ulp, and an ulp is below any tolerance a
 * test would use — but the whole value of a transcription is that you can read
 * it against the source without holding a list of "equivalent" substitutions in
 * your head, so this one stays literal too. */
static double e_pow(double x)
{
    return pow(M_E_PY, x);
}

static double decay(void)
{
    return -W[20];
}

/* FACTOR = 0.9 ** (1 / DECAY) - 1. Computed rather than hardcoded because it is
 * derived from W[20], and a decay retuned upstream must not leave a stale
 * constant behind. */
static double factor(void)
{
    return pow(0.9, 1.0 / decay()) - 1.0;
}

static double clamp_stability(double stability)
{
    return stability > STABILITY_MIN ? stability : STABILITY_MIN;
}

static double clamp_difficulty(double difficulty)
{
    if (difficulty < MIN_DIFFICULTY) return MIN_DIFFICULTY;
    if (difficulty > MAX_DIFFICULTY) return MAX_DIFFICULTY;
    return difficulty;
}

/* Python's round() for a float with no ndigits — round-half-to-EVEN, which C's
 * round() is not (it rounds half away from zero). This is CPython's
 * float___round___impl transcribed:
 *
 *     rounded = round(x);
 *     if (fabs(x - rounded) == 0.5) rounded = 2.0 * round(x / 2.0);
 *
 * It matters at exactly the values a scheduler lands on: a stability of 2.5
 * days is a 2-day interval under this rule and a 3-day one under any of the
 * obvious substitutes, and both look equally correct on the glass. The panel's
 * OWN rounding is deliberately different (Math.round, half up) because that is
 * what the proxy does to the same number — see kanji_fsrs_stability_days(). */
static double py_round(double x)
{
    double rounded = round(x);
    if (fabs(x - rounded) == 0.5) rounded = 2.0 * round(x / 2.0);
    return rounded;
}

/* JavaScript's Math.round, which is what tools/kanji_server.py's js_round()
 * applies to stability and difficulty before they go on the wire. */
static long js_round(double x)
{
    return (long)floor(x + 0.5);
}

static int clamp_int(long v, long lo, long hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int)v;
}

/* Python's timedelta.days: a floor, not a truncation. C's `/` truncates toward
 * zero, so a review timestamped before the previous one — which happens the
 * first time SNTP corrects a board that has been running on a guessed epoch —
 * would come out as 0 ("same day") under `/` and as -1 under Python. Both take
 * the short-term branch here, so today the difference is invisible; it stops
 * being invisible the moment anyone adds a `days_since > N` test. */
static int64_t floor_days(int64_t seconds)
{
    int64_t d = seconds / SECS_PER_DAY;
    if (seconds % SECS_PER_DAY != 0 && seconds < 0) d -= 1;
    return d;
}

static bool grade_ok(kanji_grade_t g)
{
    return g >= KANJI_GRADE_AGAIN && g <= KANJI_GRADE_EASY;
}

/* --------------------------------------------------------------------------
 * The FSRS-6 formulas. One function per private method of py-fsrs's Scheduler,
 * same name, same arguments, same order of operations.
 * -------------------------------------------------------------------------- */

/* Scheduler.get_card_retrievability. Note `max(0, ...)` on the elapsed days and
 * that the count is whole days, not seconds: a review nine hours after the last
 * one has an elapsed_days of 0 and therefore a retrievability of exactly 1. */
static double retrievability(const kanji_fsrs_card_t *c, int64_t now_epoch)
{
    if (!c->scheduled) return 0.0;

    int64_t elapsed = floor_days(now_epoch - c->last_review_epoch);
    if (elapsed < 0) elapsed = 0;

    return pow(1.0 + factor() * (double)elapsed / c->stability, decay());
}

/* Scheduler._initial_stability: S0(g) = w[g-1]. */
static double initial_stability(kanji_grade_t g)
{
    return clamp_stability(W[(int)g - 1]);
}

/* Scheduler._initial_difficulty: D0(g) = w4 - e^(w5*(g-1)) + 1.
 *
 * `clamp` is a real parameter upstream and not a convenience: _next_difficulty
 * calls this with Easy and clamp=False to get its mean-reversion target, which
 * is -4.7535 — a value that would be destroyed by clamping to [1,10] and would
 * turn mean reversion into mean attraction toward the difficulty floor. */
static double initial_difficulty(kanji_grade_t g, bool clamp)
{
    double d = W[4] - e_pow(W[5] * ((double)g - 1.0)) + 1.0;
    return clamp ? clamp_difficulty(d) : d;
}

/* Scheduler._next_interval, in whole days, already clamped to [1, 36500].
 *
 * The floor is not cosmetic: a stability under half a day rounds to zero, and a
 * zero-day interval is a card due at the instant it was graded — an infinite
 * loop for a learner working through a session. */
static int next_interval_days(double stability)
{
    double ivl = (stability / factor()) *
                 (pow(DESIRED_RETENTION, 1.0 / decay()) - 1.0);

    ivl = py_round(ivl);

    /* Written as `!(ivl >= 1.0)` so a NaN — which only a corrupted card could
     * produce, but a corrupted card is exactly what a flash-backed schedule can
     * hand us — becomes 1 day rather than sailing through both comparisons. */
    if (!(ivl >= 1.0)) ivl = 1.0;
    if (ivl > (double)MAXIMUM_INTERVAL) ivl = (double)MAXIMUM_INTERVAL;

    return (int)ivl;
}

/* Scheduler._short_term_stability — the same-day path.
 *
 * The clamp of the increase to 1.0 for Good and Easy is why a second correct
 * answer on the same day leaves stability untouched instead of shrinking it:
 * for a stability above about two days the raw increase is below 1. */
static double short_term_stability(double stability, kanji_grade_t g)
{
    double increase = e_pow(W[17] * ((double)g - 3.0 + W[18])) *
                      pow(stability, -W[19]);

    if (g == KANJI_GRADE_GOOD || g == KANJI_GRADE_EASY) {
        if (increase < 1.0) increase = 1.0;
    }

    return clamp_stability(stability * increase);
}

/* Scheduler._next_difficulty: linear damping toward 10, then mean reversion
 * onto D0(Easy). The damping factor (10 - D)/9 is what keeps difficulty from
 * ever crossing 10 on its own; the clamp behind it is defensive. */
static double next_difficulty(double difficulty, kanji_grade_t g)
{
    double arg_1 = initial_difficulty(KANJI_GRADE_EASY, false);

    double delta_difficulty = -(W[6] * ((double)g - 3.0));
    double linear_damping   = (10.0 - difficulty) * delta_difficulty / 9.0;
    double arg_2            = difficulty + linear_damping;

    double next = W[7] * arg_1 + (1.0 - W[7]) * arg_2;

    return clamp_difficulty(next);
}

/* Scheduler._next_forget_stability — Again after a real gap.
 *
 * The min() with the short-term term is the guard that stops a lapse from
 * *increasing* stability on a card that was already very stable. */
static double next_forget_stability(double difficulty, double stability,
                                    double r)
{
    double long_term = W[11] * pow(difficulty, -W[12]) *
                       (pow(stability + 1.0, W[13]) - 1.0) *
                       e_pow((1.0 - r) * W[14]);

    double short_term = stability / e_pow(W[17] * W[18]);

    return long_term < short_term ? long_term : short_term;
}

/* Scheduler._next_recall_stability — Hard, Good or Easy after a real gap. */
static double next_recall_stability(double difficulty, double stability,
                                    double r, kanji_grade_t g)
{
    double hard_penalty = (g == KANJI_GRADE_HARD) ? W[15] : 1.0;
    double easy_bonus   = (g == KANJI_GRADE_EASY) ? W[16] : 1.0;

    return stability * (1.0 + e_pow(W[8]) * (11.0 - difficulty) *
                              pow(stability, -W[9]) *
                              (e_pow((1.0 - r) * W[10]) - 1.0) *
                              hard_penalty * easy_bonus);
}

/* Scheduler._next_stability — the dispatch between the two above. */
static double next_stability(double difficulty, double stability, double r,
                             kanji_grade_t g)
{
    double s = (g == KANJI_GRADE_AGAIN)
                   ? next_forget_stability(difficulty, stability, r)
                   : next_recall_stability(difficulty, stability, r, g);
    return clamp_stability(s);
}

/* --------------------------------------------------------------------------
 * The state machine.
 * -------------------------------------------------------------------------- */

/* The Learning and Relearning arms of Scheduler.review_card are the same code
 * over a different step list, so they are one function here — which also makes
 * it possible to read this against the source and see that it IS the same code,
 * rather than checking two near-copies against each other.
 *
 * Returns the interval in seconds and updates *state / *step in place. */
static int64_t schedule_steps(kanji_fsrs_state_t *state, int *step,
                              kanji_grade_t g, double stability,
                              const int64_t *steps, int n_steps)
{
    /* py-fsrs's first clause covers a card scheduled by a scheduler with MORE
     * steps than this one has — a real migration case for Anki, and for us the
     * case of a card that came off the wire from a proxy configured differently
     * from this file. Graduate it rather than indexing past the end. */
    if (n_steps == 0 ||
        (*step >= n_steps && (g == KANJI_GRADE_HARD || g == KANJI_GRADE_GOOD ||
                              g == KANJI_GRADE_EASY))) {
        *state = KANJI_FSRS_REVIEW;
        *step  = 0;
        return (int64_t)next_interval_days(stability) * SECS_PER_DAY;
    }

    switch (g) {
    case KANJI_GRADE_AGAIN:
        *step = 0;
        return steps[0];

    case KANJI_GRADE_HARD:
        /* The step does not advance. With a single step, Hard is 1.5 steps out
         * — the board's 10-minute step becomes 15 minutes — because there is no
         * next step to average with.
         *
         * py-fsrs does these two in timedelta arithmetic, which keeps
         * microseconds; this does them in whole seconds, because due_epoch is
         * whole seconds and there is nowhere for a fraction to go. The two
         * agree exactly for any step that is a whole number of even seconds,
         * which every Anki-style step written in minutes is. A step list edited
         * to an odd second count would land the board up to half a second
         * before the server — harmless, and noted here so nobody has to
         * rediscover it by staring at the two implementations. */
        if (*step == 0 && n_steps == 1) return (steps[0] * 3) / 2;
        if (*step == 0)                 return (steps[0] + steps[1]) / 2;
        return steps[*step];

    case KANJI_GRADE_GOOD:
        if (*step + 1 == n_steps) {          /* the last step: graduate */
            *state = KANJI_FSRS_REVIEW;
            *step  = 0;
            return (int64_t)next_interval_days(stability) * SECS_PER_DAY;
        }
        *step += 1;
        return steps[*step];

    case KANJI_GRADE_EASY:
    default:
        *state = KANJI_FSRS_REVIEW;
        *step  = 0;
        return (int64_t)next_interval_days(stability) * SECS_PER_DAY;
    }
}

/* One review, as a pure function of (card, grade, time) -> (card, interval).
 * kanji_fsrs_review() commits it; kanji_fsrs_preview() throws away everything
 * but the due date. Sharing this is what makes the grade dock's four spans a
 * promise rather than an estimate. */
static int64_t apply_review(kanji_fsrs_card_t *c, kanji_grade_t g,
                            int64_t now_epoch)
{
    /* A card claiming Review or Relearning with no stability cannot be
     * scheduled: py-fsrs asserts, and the backend adapter raises ValueError.
     * Neither is available on a board holding a rating the learner just gave,
     * and a dropped press is a worse outcome than a card restarted from
     * scratch — which is also what py-fsrs itself would do with this card,
     * since State.Learning is the one arm that tolerates an unset stability. */
    if (!c->scheduled) {
        c->state = KANJI_FSRS_LEARNING;
        c->step  = 0;
    }

    const bool    have_last  = c->scheduled;
    const int64_t days_since = have_last
                                   ? floor_days(now_epoch - c->last_review_epoch)
                                   : 0;
    const bool    same_day   = have_last && days_since < 1;

    int64_t interval;

    switch (c->state) {
    case KANJI_FSRS_LEARNING:
        if (!c->scheduled) {
            c->stability  = initial_stability(g);
            c->difficulty = initial_difficulty(g, true);
        } else if (same_day) {
            c->stability  = short_term_stability(c->stability, g);
            c->difficulty = next_difficulty(c->difficulty, g);
        } else {
            c->stability  = next_stability(c->difficulty, c->stability,
                                           retrievability(c, now_epoch), g);
            c->difficulty = next_difficulty(c->difficulty, g);
        }
        interval = schedule_steps(&c->state, &c->step, g, c->stability,
                                  LEARNING_STEPS, N_LEARNING);
        break;

    case KANJI_FSRS_RELEARNING:
        if (same_day) {
            c->stability  = short_term_stability(c->stability, g);
            c->difficulty = next_difficulty(c->difficulty, g);
        } else {
            c->stability  = next_stability(c->difficulty, c->stability,
                                           retrievability(c, now_epoch), g);
            c->difficulty = next_difficulty(c->difficulty, g);
        }
        interval = schedule_steps(&c->state, &c->step, g, c->stability,
                                  RELEARNING_STEPS, N_RELEARNING);
        break;

    case KANJI_FSRS_REVIEW:
    default:
        /* Note the asymmetry with the two arms above, which is py-fsrs's and
         * not a slip: in Review the difficulty update is outside the branch,
         * because both paths apply it identically. */
        if (same_day) {
            c->stability = short_term_stability(c->stability, g);
        } else {
            c->stability = next_stability(c->difficulty, c->stability,
                                          retrievability(c, now_epoch), g);
        }
        c->difficulty = next_difficulty(c->difficulty, g);

        if (g == KANJI_GRADE_AGAIN && N_RELEARNING > 0) {
            c->state = KANJI_FSRS_RELEARNING;
            c->step  = 0;
            interval = RELEARNING_STEPS[0];
        } else {
            c->state = KANJI_FSRS_REVIEW;
            c->step  = 0;
            interval = (int64_t)next_interval_days(c->stability) * SECS_PER_DAY;
        }
        break;
    }

    c->scheduled         = true;
    c->due_epoch         = now_epoch + interval;
    c->last_review_epoch = now_epoch;

    return c->due_epoch;
}

/* --------------------------------------------------------------------------
 * Public API.
 * -------------------------------------------------------------------------- */

void kanji_fsrs_init(kanji_fsrs_card_t *c)
{
    if (!c) return;

    c->stability         = 0.0;
    c->difficulty        = 0.0;
    c->state             = KANJI_FSRS_LEARNING;
    c->step              = 0;
    c->reps              = 0;
    c->lapses            = 0;
    c->due_epoch         = 0;
    c->last_review_epoch = 0;
    c->scheduled         = false;
}

void kanji_fsrs_restore(kanji_fsrs_card_t *c, double stability,
                        double difficulty, kanji_grade_t last_grade,
                        int reps, int lapses, int64_t due_epoch)
{
    if (!c) return;

    kanji_fsrs_init(c);
    if (reps > 0)   c->reps = reps;
    if (lapses > 0) c->lapses = lapses;

    /* Written as `!(x > 0.0)` so a NaN off a corrupted record lands on the
     * unscheduled branch rather than being carried into the recurrence. */
    if (!(stability > 0.0) || !(difficulty > 0.0) || !grade_ok(last_grade)) {
        return;
    }

    c->stability  = clamp_stability(stability);
    c->difficulty = clamp_difficulty(difficulty);
    c->scheduled  = true;
    c->due_epoch  = due_epoch;
    c->step       = 0;

    const bool first_review = reps <= 1;
    const bool graduated = last_grade == KANJI_GRADE_GOOD ||
                           last_grade == KANJI_GRADE_EASY ||
                           (!first_review && last_grade == KANJI_GRADE_HARD);

    int64_t interval;
    if (graduated) {
        c->state = KANJI_FSRS_REVIEW;
        interval = (int64_t)next_interval_days(c->stability) * SECS_PER_DAY;
    } else {
        c->state = (!first_review && last_grade == KANJI_GRADE_AGAIN)
                       ? KANJI_FSRS_RELEARNING
                       : KANJI_FSRS_LEARNING;

        /* Through schedule_steps() rather than a literal 600, so the interval
         * this inverts is by construction the one the forward path produced. */
        const bool relearning = c->state == KANJI_FSRS_RELEARNING;
        kanji_fsrs_state_t landed = c->state;
        int step = 0;
        interval = schedule_steps(&landed, &step, last_grade, c->stability,
                                  relearning ? RELEARNING_STEPS : LEARNING_STEPS,
                                  relearning ? N_RELEARNING : N_LEARNING);
    }

    c->last_review_epoch = due_epoch - interval;
}

void kanji_fsrs_review(kanji_fsrs_card_t *c, kanji_grade_t g, int64_t now_epoch)
{
    if (!c || !grade_ok(g)) return;

    /* Captured before apply_review moves the card out of Review. The backend
     * adapter's rule verbatim: a lapse is Again *from Review*, so failing a
     * card still in Learning or already in Relearning is not one. */
    const bool is_lapse = (g == KANJI_GRADE_AGAIN) &&
                          c->scheduled && c->state == KANJI_FSRS_REVIEW;

    (void)apply_review(c, g, now_epoch);

    c->reps++;
    if (is_lapse) c->lapses++;
}

int64_t kanji_fsrs_preview(const kanji_fsrs_card_t *c, kanji_grade_t g,
                           int64_t now_epoch)
{
    /* With no card and no valid grade there is nothing to schedule, and "now"
     * is the only honest answer — the dock will word it as 곧 rather than
     * printing a date derived from nothing. */
    if (!c || !grade_ok(g)) return now_epoch;

    /* The struct is a few dozen bytes, so the copy is free and the alternative
     * — an apply_review that promises not to write through its pointer — is a
     * promise the compiler cannot check. Nowhere near the 2048-byte frame
     * budget every production TU here is built against. */
    kanji_fsrs_card_t scratch = *c;
    return apply_review(&scratch, g, now_epoch);
}

int kanji_fsrs_stability_days(const kanji_fsrs_card_t *c)
{
    if (!c || !c->scheduled) return -1;

    /* Math.round and the 99999 ceiling both come from tools/kanji_server.py's
     * card_fsrs(): the same card may be seen through the proxy one day and off
     * flash the next, and two different numbers for the same stability would
     * read as the board having lost track of the card. */
    return clamp_int(js_round(c->stability), 0, 99999);
}

int kanji_fsrs_difficulty_pct(const kanji_fsrs_card_t *c)
{
    if (!c || !c->scheduled) return -1;

    return clamp_int(js_round((c->difficulty - MIN_DIFFICULTY) /
                              (MAX_DIFFICULTY - MIN_DIFFICULTY) * 100.0),
                     0, 100);
}
