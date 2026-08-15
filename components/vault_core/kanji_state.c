#include "kanji_state.h"

#include <limits.h>
#include <string.h>

/* Schema 2 added the schedule triple at offsets 16..27 and moved the CRC.  It
 * moves in lockstep with KANJI_STATE_RECORD_SIZE, which the header also
 * carries, so an image from either side of the change is rejected twice over
 * by header_prefix_valid() and the partition restarts empty.  A schema that
 * grew silently would be worse than a corrupt one: 20-byte records read at a
 * 32-byte stride still satisfy their CRC nowhere, so replay would stop at the
 * first record and quietly present a learner's whole history as unreviewed. */
#define STATE_SCHEMA          2u
#define STATE_COMMIT          0x434f4d4du
#define STATE_RECORD_CAPACITY KANJI_STATE_RECORDS_PER_BANK

/*
 * The 32-byte record, little-endian throughout and read byte-wise so no field
 * needs the record to be aligned:
 *
 *    0  sequence      u32     16  stability     u32  (milli-days)
 *    4  card          u16     20  difficulty    u16  (milli-points)
 *    6  next_ordinal  u16     22  reserved      u16  (must be 0)
 *    8  reps          u16     24  due           u32  (minutes since epoch)
 *   10  lapses        u16     28  crc32         u32  (over bytes 0..27)
 *   12  grade         u8
 *   13  flags         u8
 *   14  reserved      u16  (must be 0)
 *
 * Both reserved halfwords are checked for zero on the way in.  They are the
 * only room a future schema has to add a field without moving the CRC again,
 * and a record that has a programmed bit there was written by something this
 * build does not understand.
 */
#define REC_SEQUENCE   0u
#define REC_CARD       4u
#define REC_NEXT       6u
#define REC_REPS       8u
#define REC_LAPSES     10u
#define REC_GRADE      12u
#define REC_FLAGS      13u
#define REC_RESERVED1  14u
#define REC_STABILITY  16u
#define REC_DIFFICULTY 20u
#define REC_RESERVED2  22u
#define REC_DUE        24u
#define REC_CRC        28u

typedef struct {
    bool valid;
    uint32_t generation;
    uint32_t base;
} bank_candidate_t;

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t state_crc32(const void *data, size_t length)
{
    const uint8_t *p = data;
    uint32_t crc = UINT32_MAX;
    while (length-- != 0) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* RFC-1982-style ordering without an implementation-defined unsigned-to-signed
 * conversion. A serial exactly half the uint32_t space away is ambiguous and
 * therefore not newer in either direction. */
static bool serial_newer(uint32_t candidate, uint32_t reference)
{
    const uint32_t delta = candidate - reference;
    return delta != 0 && delta < UINT32_C(0x80000000);
}

static unsigned select_newest_bank(const bank_candidate_t banks[2])
{
    if (banks[0].generation == banks[1].generation) return 0;
    if (serial_newer(banks[1].generation, banks[0].generation)) return 1;
    if (serial_newer(banks[0].generation, banks[1].generation)) return 0;

    /* Exact half-range ambiguity: use the numerically greater generation so
     * the choice is deterministic and independent of physical bank placement. */
    return banks[1].generation > banks[0].generation ? 1u : 0u;
}

static bool encoded_grade_valid(uint8_t grade)
{
    return grade >= (uint8_t)KANJI_GRADE_AGAIN &&
           grade <= (uint8_t)KANJI_GRADE_EASY;
}

static bool requested_grade_valid(kanji_grade_t grade)
{
    return grade == KANJI_GRADE_AGAIN || grade == KANJI_GRADE_HARD ||
           grade == KANJI_GRADE_GOOD || grade == KANJI_GRADE_EASY;
}

static bool summary_reviewed(const kanji_rating_summary_t *summary)
{
    return encoded_grade_valid(summary->grade);
}

/* The layout constants and the record size have to agree or every offset past
 * the first record is wrong, which is precisely the failure a CRC cannot see:
 * a mis-strided read lands on real bytes with a real checksum somewhere else
 * in the bank. */
_Static_assert(REC_CRC + 4u == KANJI_STATE_RECORD_SIZE,
               "record layout does not fill KANJI_STATE_RECORD_SIZE");
_Static_assert(KANJI_STATE_RECORDS_PER_BANK > 0,
               "a bank must hold at least one record");

static bool schedule_in_range(uint32_t stability_milli,
                              uint32_t difficulty_milli)
{
    return stability_milli >= KANJI_STATE_STABILITY_MIN &&
           stability_milli <= KANJI_STATE_STABILITY_MAX &&
           difficulty_milli >= KANJI_STATE_DIFFICULTY_MIN &&
           difficulty_milli <= KANJI_STATE_DIFFICULTY_MAX;
}

/* Whole minutes since the Unix epoch, truncated down.
 *
 * Negative and pre-epoch times collapse to zero rather than being rejected.
 * The board has no RTC, so the offline session this journal exists for is
 * exactly the one most likely to be scheduling against a clock that never
 * synced; refusing the write there would throw away the learner's press, while
 * storing zero reads back as "overdue", which is the truthful answer for a
 * card scheduled by a clock that does not know what year it is. */
static uint32_t due_minutes_from_epoch(int64_t due_epoch)
{
    if (due_epoch <= 0) return 0;
    const int64_t minutes = due_epoch / KANJI_STATE_DUE_TICK_SECONDS;
    return minutes > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)minutes;
}

static int64_t due_epoch_from_minutes(uint32_t minutes)
{
    return (int64_t)minutes * KANJI_STATE_DUE_TICK_SECONDS;
}

static uint32_t next_sequence(uint32_t sequence)
{
    sequence++;
    if (sequence == UINT32_MAX) sequence = 0;
    return sequence;
}

static void make_header(uint8_t header[KANJI_STATE_HEADER_SIZE],
                        uint32_t generation, const uint8_t catalog_id[16])
{
    memset(header, 0xff, KANJI_STATE_HEADER_SIZE);
    memcpy(header + 0, "KJSTATE1", 8);
    put_u16(header + 8, STATE_SCHEMA);
    put_u16(header + 10, KANJI_STATE_HEADER_SIZE);
    put_u32(header + 12, generation);
    memcpy(header + 16, catalog_id, 16);
    put_u32(header + 32, KANJI_STATE_BANK_SIZE);
    put_u16(header + 36, KANJI_STATE_RECORD_SIZE);
    put_u16(header + 38, 0);
    put_u32(header + 40, state_crc32(header, 40));
}

static bool header_prefix_valid(const uint8_t header[KANJI_STATE_HEADER_SIZE],
                                const uint8_t catalog_id[16])
{
    if (memcmp(header + 0, "KJSTATE1", 8) != 0 ||
        get_u16(header + 8) != STATE_SCHEMA ||
        get_u16(header + 10) != KANJI_STATE_HEADER_SIZE ||
        memcmp(header + 16, catalog_id, 16) != 0 ||
        get_u32(header + 32) != KANJI_STATE_BANK_SIZE ||
        get_u16(header + 36) != KANJI_STATE_RECORD_SIZE ||
        get_u16(header + 38) != 0 ||
        get_u32(header + 40) != state_crc32(header, 40)) {
        return false;
    }
    for (size_t i = 44; i < 60; i++) {
        if (header[i] != 0xff) return false;
    }
    return true;
}

static bool header_valid(const uint8_t header[KANJI_STATE_HEADER_SIZE],
                         const uint8_t catalog_id[16])
{
    return header_prefix_valid(header, catalog_id) &&
           get_u32(header + 60) == STATE_COMMIT;
}

static void make_record(uint8_t encoded[KANJI_STATE_RECORD_SIZE],
                        uint16_t card, const kanji_rating_summary_t *summary)
{
    memset(encoded, 0, KANJI_STATE_RECORD_SIZE);
    put_u32(encoded + REC_SEQUENCE, summary->sequence);
    put_u16(encoded + REC_CARD, card);
    put_u16(encoded + REC_NEXT, summary->next_ordinal);
    put_u16(encoded + REC_REPS, summary->reps);
    put_u16(encoded + REC_LAPSES, summary->lapses);
    encoded[REC_GRADE] = summary->grade;
    encoded[REC_FLAGS] = summary->flags;
    put_u32(encoded + REC_STABILITY, summary->stability_milli);
    put_u16(encoded + REC_DIFFICULTY, summary->difficulty_milli);
    /* The summary's due_epoch is already a whole number of minutes — every
     * path that sets it went through due_epoch_from_minutes() — so compaction
     * re-encoding a summary it decoded is exactly idempotent.  Truncating here
     * a second time is what makes that true rather than nearly true. */
    put_u32(encoded + REC_DUE, due_minutes_from_epoch(summary->due_epoch));
    put_u32(encoded + REC_CRC, state_crc32(encoded, REC_CRC));
}

/* The schedule triple is either wholly absent or wholly in range.  Accepting a
 * half-written one — a stability with no due date, say — would let a corrupt
 * record present itself as a schedule and put a card at the wrong end of the
 * queue for as long as it takes the learner to notice, which on a card due in
 * nine days is nine days. */
static bool encoded_schedule_valid(const uint8_t encoded[KANJI_STATE_RECORD_SIZE])
{
    const uint32_t stability = get_u32(encoded + REC_STABILITY);
    const uint32_t difficulty = get_u16(encoded + REC_DIFFICULTY);
    const uint32_t due = get_u32(encoded + REC_DUE);
    if (stability == 0 && difficulty == 0 && due == 0) return true;
    return schedule_in_range(stability, difficulty);
}

static bool record_valid(const uint8_t encoded[KANJI_STATE_RECORD_SIZE],
                         uint16_t card_count)
{
    return get_u32(encoded + REC_SEQUENCE) != UINT32_MAX &&
           get_u16(encoded + REC_CARD) < card_count &&
           get_u16(encoded + REC_NEXT) < card_count &&
           encoded_grade_valid(encoded[REC_GRADE]) &&
           get_u16(encoded + REC_RESERVED1) == 0 &&
           get_u16(encoded + REC_RESERVED2) == 0 &&
           encoded_schedule_valid(encoded) &&
           get_u32(encoded + REC_CRC) == state_crc32(encoded, REC_CRC);
}

static bool bytes_erased(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] != 0xff) return false;
    }
    return true;
}

static void record_to_summary(const uint8_t encoded[KANJI_STATE_RECORD_SIZE],
                              kanji_rating_summary_t *summary)
{
    memset(summary, 0, sizeof *summary);
    summary->sequence = get_u32(encoded + REC_SEQUENCE);
    summary->next_ordinal = get_u16(encoded + REC_NEXT);
    summary->reps = get_u16(encoded + REC_REPS);
    summary->lapses = get_u16(encoded + REC_LAPSES);
    summary->grade = encoded[REC_GRADE];
    summary->flags = encoded[REC_FLAGS];
    summary->stability_milli = get_u32(encoded + REC_STABILITY);
    summary->difficulty_milli = get_u16(encoded + REC_DIFFICULTY);
    summary->due_epoch = due_epoch_from_minutes(get_u32(encoded + REC_DUE));
}

static bool read_bank_candidate(const kanji_state_io_t *io, uint32_t base,
                                const uint8_t catalog_id[16],
                                bank_candidate_t *candidate)
{
    uint8_t header[KANJI_STATE_HEADER_SIZE];
    if (!io->read_at(io->ctx, base, header, sizeof header)) return false;
    candidate->base = base;
    candidate->valid = header_valid(header, catalog_id);
    candidate->generation = candidate->valid ? get_u32(header + 12) : 0;
    return true;
}

static bool write_verified_record(const kanji_state_t *state, uint32_t base,
                                  uint32_t record_index, uint16_t ordinal,
                                  const kanji_rating_summary_t *summary)
{
    uint8_t encoded[KANJI_STATE_RECORD_SIZE];
    uint8_t replay[KANJI_STATE_RECORD_SIZE];
    make_record(encoded, ordinal, summary);
    const uint32_t offset = base + KANJI_STATE_HEADER_SIZE +
                            record_index * KANJI_STATE_RECORD_SIZE;
    if (!state->io.write_at(state->io.ctx, offset, encoded, sizeof encoded) ||
        !state->io.read_at(state->io.ctx, offset, replay, sizeof replay)) {
        return false;
    }
    return memcmp(encoded, replay, sizeof encoded) == 0 &&
           record_valid(replay, state->card_count);
}

static bool commit_fresh_bank(kanji_state_t *state, uint32_t base,
                              uint32_t generation)
{
    uint8_t header[KANJI_STATE_HEADER_SIZE];
    uint8_t verify[KANJI_STATE_HEADER_SIZE];
    make_header(header, generation, state->catalog_id);
    if (!state->io.erase_range(state->io.ctx, base, KANJI_STATE_BANK_SIZE) ||
        !state->io.write_at(state->io.ctx, base, header, 60)) {
        return false;
    }
    uint8_t commit[4];
    put_u32(commit, STATE_COMMIT);
    if (!state->io.write_at(state->io.ctx, base + 60, commit, sizeof commit) ||
        !state->io.read_at(state->io.ctx, base, verify, sizeof verify)) {
        return false;
    }
    return header_valid(verify, state->catalog_id);
}

static bool replay_bank(kanji_state_t *state)
{
    bool have_newest = false;
    uint32_t newest_sequence = 0;
    uint16_t newest_next = 0;
    state->record_count = 0;

    for (uint32_t index = 0; index < STATE_RECORD_CAPACITY; index++) {
        uint8_t encoded[KANJI_STATE_RECORD_SIZE];
        const uint32_t offset = state->active_bank + KANJI_STATE_HEADER_SIZE +
                                index * KANJI_STATE_RECORD_SIZE;
        if (!state->io.read_at(state->io.ctx, offset, encoded, sizeof encoded)) {
            return false;
        }
        if (get_u32(encoded) == UINT32_MAX) break;
        if (!record_valid(encoded, state->card_count)) break;

        const uint32_t sequence = get_u32(encoded + 0);
        const uint16_t card = get_u16(encoded + 4);
        if (!summary_reviewed(&state->summaries[card]) ||
            serial_newer(sequence, state->summaries[card].sequence)) {
            record_to_summary(encoded, &state->summaries[card]);
        }
        if (!have_newest || serial_newer(sequence, newest_sequence)) {
            have_newest = true;
            newest_sequence = sequence;
            newest_next = get_u16(encoded + 6);
        }
        state->record_count++;
    }

    state->current_ordinal = have_newest ? newest_next : 0;
    state->next_sequence = have_newest ? next_sequence(newest_sequence) : 1;
    return true;
}

static bool compact(kanji_state_t *state)
{
    uint32_t reviewed = 0;
    for (uint16_t ordinal = 0; ordinal < state->card_count; ordinal++) {
        if (summary_reviewed(&state->summaries[ordinal])) reviewed++;
    }
    /* Compaction writes one record per reviewed card, so a catalog with more
     * reviewed cards than a bank holds cannot be compacted and grading stops
     * for good.  Schema 1's 20-byte record left room for 13107, which is why
     * nobody had to think about it against a 9956-card catalog; schema 2's
     * schedule brings that down to 8190 and the ceiling is now reachable.  The
     * failure is at least loud in the only way that matters here — the append
     * returns false and no flash is touched, so the previous state stands —
     * but the fix is a bigger study_state partition, not code in this file. */
    if (reviewed >= STATE_RECORD_CAPACITY) return false;

    const uint32_t old_base = state->active_bank;
    const uint32_t new_base = old_base == 0 ? KANJI_STATE_BANK_SIZE : 0;
    const uint32_t new_generation = state->generation + 1;
    uint8_t header[KANJI_STATE_HEADER_SIZE];
    uint8_t verify[KANJI_STATE_HEADER_SIZE];
    make_header(header, new_generation, state->catalog_id);

    if (!state->io.erase_range(state->io.ctx, new_base, KANJI_STATE_BANK_SIZE) ||
        !state->io.write_at(state->io.ctx, new_base, header, 60)) {
        return false;
    }
    if (!state->io.read_at(state->io.ctx, new_base, verify, 60) ||
        memcmp(verify, header, 60) != 0 ||
        !header_prefix_valid(verify, state->catalog_id)) {
        return false;
    }

    uint32_t output_index = 0;
    for (uint16_t ordinal = 0; ordinal < state->card_count; ordinal++) {
        if (!summary_reviewed(&state->summaries[ordinal])) continue;
        if (!write_verified_record(state, new_base, output_index, ordinal,
                                   &state->summaries[ordinal])) {
            return false;
        }
        output_index++;
    }

    uint8_t commit[4];
    put_u32(commit, STATE_COMMIT);
    if (!state->io.write_at(state->io.ctx, new_base + 60, commit,
                            sizeof commit)) {
        return false;
    }
    put_u32(header + 60, STATE_COMMIT);
    if (!state->io.read_at(state->io.ctx, new_base, verify, sizeof verify) ||
        memcmp(verify, header, sizeof header) != 0 ||
        !header_valid(verify, state->catalog_id)) {
        return false;
    }

    /* From this instruction onward the new generation is independently
     * bootable.  The former bank is now only redundant cleanup. */
    state->active_bank = new_base;
    state->generation = new_generation;
    state->record_count = output_index;
    (void)state->io.erase_range(state->io.ctx, old_base, KANJI_STATE_BANK_SIZE);
    return true;
}

bool kanji_state_open(kanji_state_t *state, const kanji_state_io_t *io,
                      const uint8_t catalog_id[16], uint16_t card_count,
                      kanji_rating_summary_t summaries[])
{
    if (state == NULL || io == NULL || catalog_id == NULL || summaries == NULL ||
        io->read_at == NULL || io->write_at == NULL || io->erase_range == NULL ||
        card_count == 0) {
        return false;
    }

    kanji_state_t opened;
    memset(&opened, 0, sizeof opened);
    opened.io = *io;
    memcpy(opened.catalog_id, catalog_id, sizeof opened.catalog_id);
    opened.card_count = card_count;
    opened.summaries = summaries;
    memset(summaries, 0, (size_t)card_count * sizeof *summaries);

    bank_candidate_t banks[2];
    if (!read_bank_candidate(io, 0, catalog_id, &banks[0]) ||
        !read_bank_candidate(io, KANJI_STATE_BANK_SIZE, catalog_id, &banks[1])) {
        return false;
    }

    if (banks[0].valid || banks[1].valid) {
        unsigned selected;
        if (!banks[0].valid) selected = 1;
        else if (!banks[1].valid) selected = 0;
        else selected = select_newest_bank(banks);
        opened.active_bank = banks[selected].base;
        opened.generation = banks[selected].generation;
    } else {
        /* Prefer the second bank when the first contains an old catalog.  The
         * fresh catalog becomes committed before any old committed bytes need
         * to be destroyed.  On a completely erased partition bank zero wins. */
        uint8_t first_word[8];
        if (!io->read_at(io->ctx, 0, first_word, sizeof first_word)) return false;
        opened.active_bank = memcmp(first_word, "\xff\xff\xff\xff\xff\xff\xff\xff", 8) == 0
                                 ? 0 : KANJI_STATE_BANK_SIZE;
        opened.generation = 0;
        if (!commit_fresh_bank(&opened, opened.active_bank, opened.generation)) {
            return false;
        }
    }

    if (!replay_bank(&opened)) {
        memset(summaries, 0, (size_t)card_count * sizeof *summaries);
        return false;
    }
    opened.ready = true;
    *state = opened;
    return true;
}

uint16_t kanji_state_current_ordinal(const kanji_state_t *state)
{
    return state != NULL && state->ready ? state->current_ordinal : 0;
}

const kanji_rating_summary_t *kanji_state_summary(const kanji_state_t *state,
                                                  uint16_t ordinal)
{
    if (state == NULL || !state->ready || ordinal >= state->card_count) return NULL;
    return &state->summaries[ordinal];
}

bool kanji_state_append_review(kanji_state_t *state, uint16_t ordinal,
                               uint16_t next_ordinal, kanji_grade_t grade,
                               const kanji_state_schedule_t *schedule)
{
    if (state == NULL || !state->ready || ordinal >= state->card_count ||
        ordinal != state->current_ordinal || next_ordinal >= state->card_count ||
        !requested_grade_valid(grade)) {
        return false;
    }
    /* Validated before anything is written, alongside the other arguments,
     * because a schedule the journal would refuse on the way back in is one
     * this board must never put on flash: replay stops at the first record it
     * cannot parse, so a single out-of-range stability would truncate every
     * grade appended after it. */
    if (schedule != NULL &&
        !schedule_in_range(schedule->stability_milli,
                           schedule->difficulty_milli)) {
        return false;
    }
    const uint8_t grade_value = (uint8_t)grade;

    kanji_rating_summary_t updated = state->summaries[ordinal];
    updated.sequence = state->next_sequence;
    updated.next_ordinal = next_ordinal;
    if (updated.reps != UINT16_MAX) updated.reps++;
    /* See kanji_state_schedule_t.lapse: a caller holding a schedule holds the
     * state machine too and is the only one that can tell a lapse from a 다시.
     * Without one there is nothing better than counting the presses. */
    const bool lapse = schedule != NULL
                           ? schedule->lapse
                           : grade_value == (uint8_t)KANJI_GRADE_AGAIN;
    if (lapse && updated.lapses != UINT16_MAX) updated.lapses++;
    updated.grade = grade_value;
    updated.flags = 0;
    if (schedule != NULL) {
        updated.stability_milli = schedule->stability_milli;
        updated.difficulty_milli = schedule->difficulty_milli;
        /* Round-trip the due time through the storage unit here rather than
         * storing what the caller passed.  RAM then holds exactly what flash
         * holds, so a reboot cannot change a due date the learner has already
         * been shown, and compaction re-encodes without drifting. */
        updated.due_epoch =
            due_epoch_from_minutes(due_minutes_from_epoch(schedule->due_epoch));
    } else {
        updated.stability_milli = 0;
        updated.difficulty_milli = 0;
        updated.due_epoch = 0;
    }

    uint8_t erased[KANJI_STATE_RECORD_SIZE];
    bool need_compaction = state->record_count == STATE_RECORD_CAPACITY;
    if (!need_compaction) {
        const uint32_t offset = state->active_bank + KANJI_STATE_HEADER_SIZE +
                                state->record_count * KANJI_STATE_RECORD_SIZE;
        if (!state->io.read_at(state->io.ctx, offset, erased, sizeof erased)) {
            return false;
        }
        need_compaction = !bytes_erased(erased, sizeof erased);
    }

    if (need_compaction) {
        if (!compact(state)) return false;
        const uint32_t offset = state->active_bank + KANJI_STATE_HEADER_SIZE +
                                state->record_count * KANJI_STATE_RECORD_SIZE;
        if (!state->io.read_at(state->io.ctx, offset, erased, sizeof erased) ||
            !bytes_erased(erased, sizeof erased)) {
            return false;
        }
    }
    if (!write_verified_record(state, state->active_bank, state->record_count,
                               ordinal, &updated)) {
        return false;
    }

    state->summaries[ordinal] = updated;
    state->current_ordinal = next_ordinal;
    state->record_count++;
    state->next_sequence = next_sequence(state->next_sequence);
    return true;
}

bool kanji_state_append_grade(kanji_state_t *state, uint16_t ordinal,
                              uint16_t next_ordinal, kanji_grade_t grade)
{
    return kanji_state_append_review(state, ordinal, next_ordinal, grade, NULL);
}
