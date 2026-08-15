#include "catalog_store_core.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "kanji_clock.h"
#include "kanji_fsrs.h"
#include "ui_strings.h"

#define CATALOG_PARTITION_TYPE 0x40u
#define STATE_PARTITION_TYPE 0x41u
#define STORE_PARTITION_SUBTYPE 0x00u
#define CATALOG_PARTITION_OFFSET 0x810000u
#define CATALOG_PARTITION_SIZE 0x770000u
#define STATE_PARTITION_OFFSET 0xF80000u
#define STATE_PARTITION_SIZE 0x080000u

struct catalog_store_runtime {
    catalog_store_ops_t ops;
    catalog_store_partition_t catalog_partition;
    catalog_store_partition_t state_partition;
    void *compressed;
    void *raw;
    kanji_rating_summary_t *summaries;
    kanji_t *current;
    kanji_t *pending;
    kanji_t *decode_workspace;
    kanji_catalog_t catalog;
    kanji_state_t state;
    uint16_t ordinal;
    bool ready;
};

/* --------------------------------------------------------------------------
 * The journal's fixed point and kanji_fsrs.c's doubles.
 *
 * kanji_state.c is integer-only on purpose and kanji_fsrs.c owns the only
 * double arithmetic in the firmware; this component sits between them, so
 * these four conversions are the only doubles anywhere in it and the scale
 * kanji_state.h documents is checked here rather than assumed.
 * -------------------------------------------------------------------------- */

static uint32_t stability_to_fixed(double days)
{
    const double scaled = days * (double)KANJI_STATE_STABILITY_SCALE;
    /* Clamped rather than cast, because kanji_state_append_review() rejects an
     * out-of-range schedule outright — and a rejected append is a rating the
     * learner made and the board threw away. py-fsrs clamps stability to
     * [0.001, 36500] days, which is exactly this range, so the clamp only ever
     * catches a value that arithmetic could not have produced. */
    if (!(scaled > (double)KANJI_STATE_STABILITY_MIN)) {
        return KANJI_STATE_STABILITY_MIN;
    }
    if (scaled >= (double)KANJI_STATE_STABILITY_MAX) {
        return KANJI_STATE_STABILITY_MAX;
    }
    return (uint32_t)llround(scaled);
}

static double stability_from_fixed(uint32_t fixed)
{
    return (double)fixed / (double)KANJI_STATE_STABILITY_SCALE;
}

static uint16_t difficulty_to_fixed(double points)
{
    const double scaled = points * (double)KANJI_STATE_DIFFICULTY_SCALE;
    if (!(scaled > (double)KANJI_STATE_DIFFICULTY_MIN)) {
        return KANJI_STATE_DIFFICULTY_MIN;
    }
    if (scaled >= (double)KANJI_STATE_DIFFICULTY_MAX) {
        return KANJI_STATE_DIFFICULTY_MAX;
    }
    return (uint16_t)llround(scaled);
}

static double difficulty_from_fixed(uint16_t fixed)
{
    return (double)fixed / (double)KANJI_STATE_DIFFICULTY_SCALE;
}

/* The scheduler card one journal summary describes. A summary whose stability
 * and difficulty are both zero is an UNREVIEWED card — kanji_state.h says so
 * explicitly — and kanji_fsrs_restore() maps that onto `scheduled == false`. */
static void card_from_summary(kanji_fsrs_card_t *out,
                              const kanji_rating_summary_t *summary)
{
    if (summary == NULL) {
        kanji_fsrs_init(out);
        return;
    }
    kanji_fsrs_restore(out, stability_from_fixed(summary->stability_milli),
                       difficulty_from_fixed(summary->difficulty_milli),
                       (kanji_grade_t)summary->grade, (int)summary->reps,
                       (int)summary->lapses, summary->due_epoch);
}

static const char *state_wire(const kanji_fsrs_card_t *c)
{
    if (!c->scheduled) return "new";
    switch (c->state) {
    case KANJI_FSRS_RELEARNING: return "relearning";
    case KANJI_FSRS_REVIEW:     return "review";
    case KANJI_FSRS_LEARNING:
    default:                    return "learning";
    }
}

static const char *state_label(const kanji_fsrs_card_t *c)
{
    if (!c->scheduled) return S_STATE_NEW;
    switch (c->state) {
    case KANJI_FSRS_RELEARNING: return S_STATE_RELEARNING;
    case KANJI_FSRS_REVIEW:     return S_STATE_REVIEW;
    case KANJI_FSRS_LEARNING:
    default:                    return S_STATE_LEARNING;
    }
}

void catalog_store_core_project(kanji_t *card,
                                const kanji_rating_summary_t *summary,
                                bool clock_known, int64_t now_epoch)
{
    /* REMOTE STILL WINS — see catalog_store_core.h. */
    if (card == NULL || !card->valid || !card->card.valid ||
        card->source != KANJI_SOURCE_CATALOG) {
        return;
    }

    kanji_fsrs_card_t scheduler;
    card_from_summary(&scheduler, summary);

    kanji_fsrs_t *fsrs = &card->card.fsrs;
    fsrs->reps = summary != NULL ? (int)summary->reps : 0;
    fsrs->lapses = summary != NULL ? (int)summary->lapses : 0;
    fsrs->stability_days = kanji_fsrs_stability_days(&scheduler);
    fsrs->difficulty_pct = kanji_fsrs_difficulty_pct(&scheduler);
    kanji_str_copy(fsrs->state, sizeof fsrs->state, state_wire(&scheduler));
    kanji_str_copy(fsrs->state_label, sizeof fsrs->state_label,
                   state_label(&scheduler));

    /* Cleared before the clock is consulted so the UNKNOWN board leaves them
     * empty by construction rather than by remembering to. */
    fsrs->due[0] = '\0';
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        card->card.preview.span[i][0] = '\0';
    }
    if (!clock_known) return;

    if (scheduler.scheduled) {
        kanji_relative_due(fsrs->due, sizeof fsrs->due,
                           scheduler.due_epoch - now_epoch);
    }
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        const kanji_grade_t grade = (kanji_grade_t)(KANJI_GRADE_AGAIN + i);
        const int64_t due = kanji_fsrs_preview(&scheduler, grade, now_epoch);
        kanji_relative_due(card->card.preview.span[i],
                           sizeof card->card.preview.span[i], due - now_epoch);
    }
}

static bool ops_valid(const catalog_store_ops_t *ops)
{
    return ops != NULL && ops->alloc != NULL && ops->dealloc != NULL &&
           ops->find_partition != NULL && ops->read != NULL &&
           ops->write != NULL && ops->erase != NULL && ops->inflate != NULL &&
           ops->crc32 != NULL && ops->now != NULL &&
           ops->compressed_capacity != 0;
}

static bool catalog_partition_valid(const catalog_store_partition_t *partition)
{
    return partition->context != NULL &&
           partition->type == CATALOG_PARTITION_TYPE &&
           partition->subtype == STORE_PARTITION_SUBTYPE &&
           partition->address == CATALOG_PARTITION_OFFSET &&
           partition->size == CATALOG_PARTITION_SIZE && partition->readonly;
}

static bool state_partition_valid(const catalog_store_partition_t *partition)
{
    return partition->context != NULL &&
           partition->type == STATE_PARTITION_TYPE &&
           partition->subtype == STORE_PARTITION_SUBTYPE &&
           partition->address == STATE_PARTITION_OFFSET &&
           partition->size == STATE_PARTITION_SIZE && !partition->readonly;
}

static void runtime_release(catalog_store_runtime_t *runtime)
{
    if (runtime == NULL) return;
    runtime->ops.dealloc(runtime->ops.context, runtime->summaries);
    runtime->ops.dealloc(runtime->ops.context, runtime->raw);
    runtime->ops.dealloc(runtime->ops.context, runtime->compressed);
    runtime->ops.dealloc(runtime->ops.context, runtime->decode_workspace);
    runtime->ops.dealloc(runtime->ops.context, runtime->pending);
    runtime->ops.dealloc(runtime->ops.context, runtime->current);
    runtime->ops.dealloc(runtime->ops.context, runtime);
}

static catalog_store_runtime_t *runtime_alloc(const catalog_store_ops_t *ops)
{
    catalog_store_runtime_t *runtime = ops->alloc(ops->context, sizeof *runtime);
    if (runtime == NULL) return NULL;
    memset(runtime, 0, sizeof *runtime);
    runtime->ops = *ops;

    runtime->current = ops->alloc(ops->context, sizeof *runtime->current);
    runtime->pending = ops->alloc(ops->context, sizeof *runtime->pending);
    runtime->decode_workspace =
        ops->alloc(ops->context, sizeof *runtime->decode_workspace);
    runtime->compressed = ops->alloc(ops->context, ops->compressed_capacity);
    runtime->raw = ops->alloc(ops->context, KANJI_CATALOG_MAX_RAW_BLOCK);
    if (runtime->current == NULL || runtime->pending == NULL ||
        runtime->decode_workspace == NULL ||
        runtime->compressed == NULL || runtime->raw == NULL) {
        runtime_release(runtime);
        return NULL;
    }
    return runtime;
}

/* The injected clock, as the rest of this file wants to ask it: false means
 * KANJI_CLOCK_UNKNOWN, and `*out_epoch` is then untouched. */
static bool runtime_now(const catalog_store_runtime_t *runtime,
                        int64_t *out_epoch)
{
    return runtime->ops.now(runtime->ops.context, out_epoch);
}

static void runtime_project(catalog_store_runtime_t *runtime, kanji_t *card,
                            uint16_t ordinal)
{
    int64_t now = 0;
    const bool known = runtime_now(runtime, &now);
    catalog_store_core_project(card, kanji_state_summary(&runtime->state, ordinal),
                               known, now);
}

bool catalog_store_core_init(catalog_store_core_t *core,
                             const catalog_store_ops_t *ops)
{
    if (core == NULL || !ops_valid(ops)) return false;

    catalog_store_runtime_t *opened = runtime_alloc(ops);
    if (opened == NULL) return false;

    if (!ops->find_partition(ops->context, CATALOG_STORE_PARTITION_CATALOG,
                             &opened->catalog_partition) ||
        !catalog_partition_valid(&opened->catalog_partition) ||
        !ops->find_partition(ops->context, CATALOG_STORE_PARTITION_STATE,
                             &opened->state_partition) ||
        !state_partition_valid(&opened->state_partition)) {
        runtime_release(opened);
        return false;
    }

    const kanji_catalog_io_t catalog_io = {
        .context = opened->catalog_partition.context,
        .read = ops->read,
        .inflate = ops->inflate,
        .crc32 = ops->crc32,
    };
    if (!kanji_catalog_open(&opened->catalog, &catalog_io,
                            opened->catalog_partition.size,
                            opened->compressed, ops->compressed_capacity,
                            opened->raw, KANJI_CATALOG_MAX_RAW_BLOCK)) {
        runtime_release(opened);
        return false;
    }

    const uint32_t card_count = kanji_catalog_card_count(&opened->catalog);
    if (card_count == 0 || card_count > UINT16_MAX) {
        runtime_release(opened);
        return false;
    }
    opened->summaries = ops->alloc(
        ops->context, (size_t)card_count * sizeof *opened->summaries);
    if (opened->summaries == NULL) {
        runtime_release(opened);
        return false;
    }

    const kanji_state_io_t state_io = {
        .read_at = ops->read,
        .write_at = ops->write,
        .erase_range = ops->erase,
        .ctx = opened->state_partition.context,
    };
    if (!kanji_state_open(&opened->state, &state_io,
                          kanji_catalog_id(&opened->catalog),
                          (uint16_t)card_count, opened->summaries)) {
        runtime_release(opened);
        return false;
    }

    opened->ordinal = kanji_state_current_ordinal(&opened->state);
    if (!kanji_catalog_read_card(&opened->catalog, opened->ordinal,
                                 opened->current, opened->decode_workspace)) {
        runtime_release(opened);
        return false;
    }
    runtime_project(opened, opened->current, opened->ordinal);
    opened->ready = true;

    catalog_store_runtime_t *previous = core->active;
    core->active = opened;
    runtime_release(previous);
    return true;
}

void catalog_store_core_release(catalog_store_core_t *core)
{
    if (core == NULL) return;
    catalog_store_runtime_t *previous = core->active;
    core->active = NULL;
    runtime_release(previous);
}

bool catalog_store_core_available(const catalog_store_core_t *core)
{
    return core != NULL && core->active != NULL && core->active->ready;
}

const kanji_t *catalog_store_core_current(const catalog_store_core_t *core)
{
    return catalog_store_core_available(core) ? core->active->current : NULL;
}

uint16_t catalog_store_core_ordinal(const catalog_store_core_t *core)
{
    return catalog_store_core_available(core) ? core->active->ordinal : 0;
}

bool catalog_store_core_grade(catalog_store_core_t *core, kanji_grade_t grade)
{
    if (!catalog_store_core_available(core)) return false;
    catalog_store_runtime_t *runtime = core->active;
    const uint32_t count = kanji_catalog_card_count(&runtime->catalog);
    if (count == 0 || count > UINT16_MAX) return false;
    const uint16_t next_ordinal =
        (uint16_t)(((uint32_t)runtime->ordinal + 1u) % count);

    if (!kanji_catalog_read_card(&runtime->catalog, next_ordinal,
                                 runtime->pending,
                                 runtime->decode_workspace)) {
        return false;
    }

    /* The schedule this press produces, or none at all.
     *
     * A board whose clock is UNKNOWN has never been told the time, and a due
     * date computed from that is a due date computed from 1970 — every card in
     * the catalog overdue by half a century, with nothing on the glass to say
     * why. kanji_state_append_review() takes NULL for exactly this, and the
     * press is still recorded: the learner's rating is not the thing to throw
     * away when the clock is missing. */
    int64_t now = 0;
    kanji_state_schedule_t schedule;
    const kanji_state_schedule_t *scheduled = NULL;
    if (runtime_now(runtime, &now)) {
        kanji_fsrs_card_t card;
        card_from_summary(&card,
                          kanji_state_summary(&runtime->state, runtime->ordinal));

        /* The lapse rule is FSRS's and is not restated here: the engine bumps
         * `lapses` on Again from Review and on nothing else, so the difference
         * across the call IS the answer. */
        const int lapses_before = card.lapses;
        kanji_fsrs_review(&card, grade, now);

        schedule.due_epoch = card.due_epoch;
        schedule.stability_milli = stability_to_fixed(card.stability);
        schedule.difficulty_milli = difficulty_to_fixed(card.difficulty);
        schedule.lapse = card.lapses > lapses_before;
        scheduled = &schedule;
    }

    if (!kanji_state_append_review(&runtime->state, runtime->ordinal,
                                   next_ordinal, grade, scheduled)) {
        return false;
    }

    kanji_t *previous = runtime->current;
    runtime->current = runtime->pending;
    runtime->pending = previous;
    runtime->ordinal = next_ordinal;

    /* After the append, not before: on a one-card catalog the next ordinal is
     * the one just graded, and its figures are the ones the append wrote. */
    runtime_project(runtime, runtime->current, runtime->ordinal);
    return true;
}
