#include "kanji_state.h"

#include <limits.h>
#include <string.h>

#define STATE_SCHEMA          1u
#define STATE_COMMIT          0x434f4d4du
#define STATE_RECORD_CAPACITY \
    ((KANJI_STATE_BANK_SIZE - KANJI_STATE_HEADER_SIZE) / KANJI_STATE_RECORD_SIZE)

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

/* RFC-1982-style ordering.  A generation immediately after UINT32_MAX is 0. */
static bool serial_newer(uint32_t candidate, uint32_t reference)
{
    return (int32_t)(candidate - reference) > 0;
}

static bool grade_valid(uint8_t grade)
{
    return grade >= (uint8_t)KANJI_GRADE_AGAIN &&
           grade <= (uint8_t)KANJI_GRADE_EASY;
}

static bool summary_reviewed(const kanji_rating_summary_t *summary)
{
    return grade_valid(summary->grade);
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

static bool header_valid(const uint8_t header[KANJI_STATE_HEADER_SIZE],
                         const uint8_t catalog_id[16])
{
    return memcmp(header + 0, "KJSTATE1", 8) == 0 &&
           get_u16(header + 8) == STATE_SCHEMA &&
           get_u16(header + 10) == KANJI_STATE_HEADER_SIZE &&
           memcmp(header + 16, catalog_id, 16) == 0 &&
           get_u32(header + 32) == KANJI_STATE_BANK_SIZE &&
           get_u16(header + 36) == KANJI_STATE_RECORD_SIZE &&
           get_u16(header + 38) == 0 &&
           get_u32(header + 40) == state_crc32(header, 40) &&
           get_u32(header + 60) == STATE_COMMIT;
}

static void make_record(uint8_t encoded[KANJI_STATE_RECORD_SIZE],
                        uint16_t card, const kanji_rating_summary_t *summary)
{
    memset(encoded, 0, KANJI_STATE_RECORD_SIZE);
    put_u32(encoded + 0, summary->sequence);
    put_u16(encoded + 4, card);
    put_u16(encoded + 6, summary->next_ordinal);
    put_u16(encoded + 8, summary->reps);
    put_u16(encoded + 10, summary->lapses);
    encoded[12] = summary->grade;
    encoded[13] = summary->flags;
    put_u32(encoded + 16, state_crc32(encoded, 16));
}

static bool record_valid(const uint8_t encoded[KANJI_STATE_RECORD_SIZE],
                         uint16_t card_count)
{
    return get_u32(encoded + 0) != UINT32_MAX &&
           get_u16(encoded + 4) < card_count &&
           get_u16(encoded + 6) < card_count &&
           grade_valid(encoded[12]) && get_u16(encoded + 14) == 0 &&
           get_u32(encoded + 16) == state_crc32(encoded, 16);
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
    summary->sequence = get_u32(encoded + 0);
    summary->next_ordinal = get_u16(encoded + 6);
    summary->reps = get_u16(encoded + 8);
    summary->lapses = get_u16(encoded + 10);
    summary->grade = encoded[12];
    summary->flags = encoded[13];
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
    if (reviewed >= STATE_RECORD_CAPACITY) return false;

    const uint32_t old_base = state->active_bank;
    const uint32_t new_base = old_base == 0 ? KANJI_STATE_BANK_SIZE : 0;
    const uint32_t new_generation = state->generation + 1;
    uint8_t header[KANJI_STATE_HEADER_SIZE];
    make_header(header, new_generation, state->catalog_id);

    if (!state->io.erase_range(state->io.ctx, new_base, KANJI_STATE_BANK_SIZE) ||
        !state->io.write_at(state->io.ctx, new_base, header, 60)) {
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
        else selected = serial_newer(banks[1].generation, banks[0].generation) ? 1 : 0;
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

bool kanji_state_append_grade(kanji_state_t *state, uint16_t ordinal,
                              uint16_t next_ordinal, kanji_grade_t grade)
{
    if (state == NULL || !state->ready || ordinal >= state->card_count ||
        ordinal != state->current_ordinal || next_ordinal >= state->card_count ||
        !grade_valid((uint8_t)grade)) {
        return false;
    }

    kanji_rating_summary_t updated = state->summaries[ordinal];
    updated.sequence = state->next_sequence;
    updated.next_ordinal = next_ordinal;
    if (updated.reps != UINT16_MAX) updated.reps++;
    if (grade == KANJI_GRADE_AGAIN && updated.lapses != UINT16_MAX) {
        updated.lapses++;
    }
    updated.grade = (uint8_t)grade;
    updated.flags = 0;

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
