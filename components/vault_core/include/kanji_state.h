/*
 * kanji_state.h — portable, power-loss-safe offline rating journal.
 *
 * Storage is a raw 512 KiB NOR partition.  The caller owns both the flash
 * callbacks and one summary per catalog card; this module allocates nothing
 * and has no ESP-IDF or filesystem dependency.
 *
 * Schema 2 carries a real schedule.  Schema 1 stored only what button was
 * pressed and where the session was, so a reboot could resume the *position*
 * but not the *plan*: every card came back in catalog order, which is a
 * round-robin wearing a spaced-repetition badge.  The three numbers added here
 * — stability, difficulty and a due timestamp — are what makes a rebooted
 * board resume the schedule FSRS actually computed.
 *
 * This module still stores numbers and nothing else.  It does not include
 * kanji_fsrs.h, it never reads a clock, and it has no opinion about what a
 * good interval is; kanji_fsrs.c computes the schedule and the layer that owns
 * both hands it here.  That separation is why this file can be unit-tested
 * against a byte array with no time source at all, and why a scheduler bug
 * cannot corrupt the journal that survives it.
 */
#pragma once

#include "kanji_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KANJI_STATE_BANK_SIZE      (256u * 1024u)
#define KANJI_STATE_PARTITION_SIZE (2u * KANJI_STATE_BANK_SIZE)
#define KANJI_STATE_HEADER_SIZE    64u

/* One appended record, fixed forever inside a schema.  Schema 1 was 20 bytes;
 * the schedule costs 12 more and the header's own record-size field plus the
 * schema number both move with it, so an image written by older firmware is
 * rejected at open() and the partition starts fresh rather than being read as
 * if the new fields were there.  Reading a 20-byte image as 32-byte records
 * would not fail a CRC — it would resynchronise onto arbitrary byte offsets
 * and hand the learner somebody else's grades. */
#define KANJI_STATE_RECORD_SIZE    32u

/* How many appends fit in one bank between compactions: (262144-64)/32 = 8190,
 * which divides the bank exactly, so no tail bytes are stranded.
 *
 * This number is also a hard ceiling on the catalog, and it is worth stating
 * plainly because nothing else does: compaction rewrites one record per
 * *reviewed* card, so a catalog whose reviewed cards outnumber this cannot be
 * compacted, and once the bank fills, kanji_state_append_grade() starts
 * returning false permanently.  Schema 1's 13107 sat above the shipped 9956-
 * card catalog; 8190 does not.  See the note in kanji_state.c's compact(). */
#define KANJI_STATE_RECORDS_PER_BANK \
    ((KANJI_STATE_BANK_SIZE - KANJI_STATE_HEADER_SIZE) / KANJI_STATE_RECORD_SIZE)

/*
 * Fixed-point scales for the two real-valued FSRS numbers.
 *
 * FSRS is defined in reals and kanji_fsrs_card_t keeps doubles, but a double
 * must not appear in a CRC'd on-flash struct.  Its byte layout is not fixed by
 * C — sign/exponent order, endianness of the mantissa and the treatment of
 * subnormals are all implementation choices — so a record written by one
 * toolchain and read by the next could pass its CRC and still decode to a
 * different number.  Worse, the arithmetic is not exact: re-encoding a value
 * that was decoded from flash can produce a different bit pattern, which turns
 * compaction (which re-writes every summary) into a slow drift nobody logs.
 * Integers have neither problem.
 *
 * Stability is stored in milli-days.  py-fsrs clamps stability to
 * [0.001, 36500] days, so one milli-day is exactly the library's own floor —
 * the smallest stability the scheduler can produce is 1 LSB here, and the
 * largest is 36 500 000, comfortably inside uint32_t.  Resolution is 0.001 day
 * (86.4 s), which is finer than the shortest interval the board can schedule
 * (a 10-minute learning step) by two orders of magnitude.
 *
 * Difficulty is stored in milli-points over py-fsrs's 1..10 range, so 1000 ..
 * 10000 — the top of the range needs 14 bits and uint16_t has 16.  Resolution
 * is 0.001 difficulty point, i.e. one part in 9000 of the usable range; the
 * panel only ever prints difficulty rounded to a whole percent.
 */
#define KANJI_STATE_STABILITY_SCALE   1000u       /* units per day            */
#define KANJI_STATE_STABILITY_MIN     1u          /* 0.001 day, py-fsrs S_MIN */
#define KANJI_STATE_STABILITY_MAX     36500000u   /* 36500 days               */
#define KANJI_STATE_DIFFICULTY_SCALE  1000u       /* units per point          */
#define KANJI_STATE_DIFFICULTY_MIN    1000u       /* 1.000                    */
#define KANJI_STATE_DIFFICULTY_MAX    10000u      /* 10.000                   */

/* The due timestamp is stored as a count of whole minutes since the Unix
 * epoch, which is the third fixed-point field and the only one whose scale the
 * caller does not see: kanji_rating_summary_t and kanji_state_append_review()
 * both speak int64_t UTC *seconds*, matching kanji_fsrs_card_t.due_epoch,
 * because a due date is the one number that gets compared against a clock and
 * handing it over in an exotic unit is how off-by-sixty bugs are born.
 *
 * A uint32_t of minutes spans 1970 to the year 10139, so unlike a uint32_t of
 * seconds it cannot saturate inside any schedule FSRS can produce (36500 days
 * from now is the year 2126).  The cost is that a stored due time is truncated
 * *down* to the minute — deliberately down, so a card can come due up to 59 s
 * early but never late, and never sits withheld past the moment the scheduler
 * promised.  kanji_state_append_review() writes the truncated value back into
 * the caller's summary, so RAM and flash always agree; a caller that compares
 * the value it passed with the value it can read back must expect this. */
#define KANJI_STATE_DUE_TICK_SECONDS  60

typedef bool (*kanji_state_read_fn)(void *ctx, uint32_t offset,
                                    void *dst, size_t length);
typedef bool (*kanji_state_write_fn)(void *ctx, uint32_t offset,
                                     const void *src, size_t length);
typedef bool (*kanji_state_erase_fn)(void *ctx, uint32_t offset,
                                     size_t length);

typedef struct {
    kanji_state_read_fn read_at;
    kanji_state_write_fn write_at;
    kanji_state_erase_fn erase_range;
    void *ctx;
} kanji_state_io_t;

/* The schedule one rating produced, in the journal's storage units.
 *
 * Stability and difficulty are handed over already scaled because the caller
 * owns the only double arithmetic in the firmware and doing the rounding there
 * keeps this file integer-only; see KANJI_STATE_STABILITY_SCALE above for the
 * conversion and its resolution.  due_epoch is plain UTC seconds. */
typedef struct {
    int64_t due_epoch;          /* UTC seconds, truncated down to the minute */
    uint32_t stability_milli;   /* days x 1000,  KANJI_STATE_STABILITY_*     */
    uint16_t difficulty_milli;  /* points x 1000, KANJI_STATE_DIFFICULTY_*   */
    /* Whether this rating was a LAPSE, which is not the same thing as a 다시.
     *
     * FSRS counts a lapse only for Again *from the Review state*: failing a
     * card you have never graduated is still learning it.  Deciding that needs
     * the state machine, which this file deliberately does not have — so the
     * caller that computed the schedule, which does have it, says so here.  A
     * caller with no scheduler has no way to tell the two apart, and for it
     * kanji_state_append_grade() keeps counting every 다시, which is the most
     * it can honestly claim.
     *
     * The counter this feeds is printed on the card as 실패, and the same
     * ratings are replayed into the server's py-fsrs when the proxy comes
     * back.  A board counting lapses by a different rule from the server's is
     * the silent history drift kanji_fsrs.h exists to prevent. */
    bool lapse;
} kanji_state_schedule_t;

/* Complete absolute state from the newest record for one card.  sequence == 0
 * and grade == 0 describes an unreviewed card after open. Field names mirror
 * the fixed record encoding so the ESP adapter has no semantic translation.
 *
 * A card carries a schedule when stability_milli and difficulty_milli are both
 * non-zero; all three schedule fields are zero together otherwise, which is
 * how a rating appended by a caller with no scheduler is stored.  The zero
 * state is therefore both "never reviewed" and "reviewed without a schedule",
 * exactly as kanji_fsrs_card_t.scheduled == false covers both.
 *
 * The field order is the one that leaves no implementation-defined padding
 * between members: the 8-byte due_epoch leads, and the explicit tail brings
 * sizeof up to its alignment.  Padding would not affect any single field, but
 * the journal's tests compare whole summaries with memcmp — a zeroed summary
 * against a replayed one — and a byte comparison over unspecified padding is a
 * test that passes or fails on the compiler's mood. */
typedef struct {
    int64_t due_epoch;
    uint32_t sequence;
    uint32_t stability_milli;
    uint16_t difficulty_milli;
    uint16_t next_ordinal;
    uint16_t reps;
    uint16_t lapses;
    uint8_t grade;
    uint8_t flags;
    uint8_t reserved[6];
} kanji_rating_summary_t;

/* Public so firmware can allocate it statically.  Fields below summaries are
 * journal bookkeeping, not a scheduling API. */
typedef struct {
    kanji_state_io_t io;
    uint8_t catalog_id[16];
    uint16_t card_count;
    uint16_t current_ordinal;
    kanji_rating_summary_t *summaries;
    uint32_t active_bank;
    uint32_t generation;
    uint32_t record_count;
    uint32_t next_sequence;
    bool ready;
} kanji_state_t;

/* Open and replay the newest committed bank matching catalog_id.  An erased
 * partition, a catalog-id mismatch or an image written under a different
 * schema creates a fresh committed generation with current ordinal zero. */
bool kanji_state_open(kanji_state_t *state, const kanji_state_io_t *io,
                      const uint8_t catalog_id[16], uint16_t card_count,
                      kanji_rating_summary_t summaries[]);

uint16_t kanji_state_current_ordinal(const kanji_state_t *state);

/* Returns NULL for an unopened state or an ordinal outside the catalog. */
const kanji_rating_summary_t *kanji_state_summary(const kanji_state_t *state,
                                                  uint16_t ordinal);

/* Persist one outcome and the schedule it produced, verifying the record on
 * flash before changing RAM.
 *
 * `schedule` may be NULL, which means this rating carries no schedule: the
 * card's stored stability, difficulty and due time are all cleared.  Clearing
 * rather than carrying the previous values forward is deliberate.  A schedule
 * describes the *next* review of a card, so the moment a new rating is
 * recorded the old due date is a statement about a review that has already
 * happened.  Keeping it would leave a card claiming it is due in nine days
 * when the learner just pressed 다시 on it, and nothing downstream could tell
 * that stale schedule from a real one.
 *
 * A non-NULL schedule must be in range — see KANJI_STATE_STABILITY_MIN/MAX and
 * KANJI_STATE_DIFFICULTY_MIN/MAX — and an out-of-range one is rejected with no
 * flash write and no change to the summary, like every other bad argument
 * here.  Pass NULL for "unscheduled"; an all-zero struct is out of range, so a
 * caller cannot half-clear a schedule by mistake.  due_epoch takes any value:
 * negatives and pre-1970 times store as zero, which reads back as "overdue",
 * because a board whose clock never came up must still be able to record the
 * press the learner made. */
bool kanji_state_append_review(kanji_state_t *state, uint16_t ordinal,
                               uint16_t next_ordinal, kanji_grade_t grade,
                               const kanji_state_schedule_t *schedule);

/* kanji_state_append_review() with no schedule.  This is the whole of what
 * schema 1 could record, kept because the offline session may run before the
 * scheduler is wired in, and because a caller that genuinely has no clock has
 * nothing honest to put in a due date. */
bool kanji_state_append_grade(kanji_state_t *state, uint16_t ordinal,
                              uint16_t next_ordinal, kanji_grade_t grade);

#ifdef __cplusplus
}
#endif
