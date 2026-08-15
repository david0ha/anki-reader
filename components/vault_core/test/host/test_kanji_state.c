/*
 * A byte-accurate NOR fake for the portable rating journal.  Tests never use
 * filesystem persistence: a "reboot" is a fresh kanji_state_t replaying the
 * same injected flash bytes.
 */
#include "kanji_state.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARTITION_SIZE (2u * 256u * 1024u)
#define BANK_SIZE      (256u * 1024u)
#define HEADER_SIZE    64u
#define RECORD_SIZE    32u
#define ERASE_SIZE     4096u
#define RECORD_CAPACITY ((BANK_SIZE - HEADER_SIZE) / RECORD_SIZE)

/* Schema 1, for the one test that has to produce an image this firmware must
 * refuse.  Recomputed here rather than imported, because the point of the test
 * is that the *old* bytes are rejected — a constant shared with the production
 * header would move with it and the test would silently stop testing. */
#define LEGACY_SCHEMA        1u
#define LEGACY_RECORD_SIZE   20u

static int failures;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #expr); \
        failures++;                                                             \
    }                                                                           \
} while (0)

#define CHECK_U32(got, want) do {                                               \
    uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want);                       \
    if (g_ != w_) {                                                             \
        fprintf(stderr, "%s:%d: got %lu, want %lu\n", __FILE__, __LINE__,      \
                (unsigned long)g_, (unsigned long)w_);                          \
        failures++;                                                             \
    }                                                                           \
} while (0)

#define CHECK_I64(got, want) do {                                               \
    int64_t g_ = (int64_t)(got), w_ = (int64_t)(want);                          \
    if (g_ != w_) {                                                             \
        fprintf(stderr, "%s:%d: got %lld, want %lld\n", __FILE__, __LINE__,    \
                (long long)g_, (long long)w_);                                  \
        failures++;                                                             \
    }                                                                           \
} while (0)

typedef struct {
    uint8_t bytes[PARTITION_SIZE];
    bool fail_read;
    bool fail_write;
    bool fail_erase;
    bool drop_write_but_report_success;
    bool cutoff_enabled;
    size_t write_bytes_left;
    uint32_t deceptive_write_offset;
    size_t deceptive_write_length;
    enum {
        DECEPTIVE_WRITE_NONE = 0,
        DECEPTIVE_WRITE_DROP,
        DECEPTIVE_WRITE_CORRUPT,
    } deceptive_write;
    unsigned erase_calls;
} nor_fake_t;

static void nor_init(nor_fake_t *nor)
{
    memset(nor, 0, sizeof *nor);
    memset(nor->bytes, 0xff, sizeof nor->bytes);
}

static bool nor_read(void *ctx, uint32_t offset, void *dst, size_t length)
{
    nor_fake_t *nor = ctx;
    if (nor->fail_read || offset > PARTITION_SIZE ||
        length > PARTITION_SIZE - offset) {
        return false;
    }
    memcpy(dst, nor->bytes + offset, length);
    return true;
}

static bool nor_write(void *ctx, uint32_t offset, const void *src, size_t length)
{
    nor_fake_t *nor = ctx;
    if (nor->fail_write || offset > PARTITION_SIZE ||
        length > PARTITION_SIZE - offset) {
        return false;
    }
    if (nor->drop_write_but_report_success) return true;

    const bool deceive = nor->deceptive_write != DECEPTIVE_WRITE_NONE &&
                         offset == nor->deceptive_write_offset &&
                         length == nor->deceptive_write_length;
    if (deceive && nor->deceptive_write == DECEPTIVE_WRITE_DROP) {
        nor->deceptive_write = DECEPTIVE_WRITE_NONE;
        return true;
    }

    size_t written = length;
    if (nor->cutoff_enabled && written > nor->write_bytes_left) {
        written = nor->write_bytes_left;
    }

    const uint8_t *input = src;
    for (size_t i = 0; i < written; i++) {
        if ((nor->bytes[offset + i] & input[i]) != input[i]) {
            return false; /* a real NOR cannot turn a zero back into a one */
        }
    }
    for (size_t i = 0; i < written; i++) {
        nor->bytes[offset + i] &= input[i];
    }
    if (deceive && nor->deceptive_write == DECEPTIVE_WRITE_CORRUPT) {
        /* A plausible NOR fault: one additional programmed zero while the
         * driver incorrectly reports that the requested operation succeeded. */
        for (size_t i = 0; i < written; i++) {
            uint8_t set_bits = nor->bytes[offset + i];
            if (set_bits != 0) {
                nor->bytes[offset + i] &= (uint8_t)(set_bits - 1u);
                break;
            }
        }
        nor->deceptive_write = DECEPTIVE_WRITE_NONE;
    }

    if (nor->cutoff_enabled) {
        nor->write_bytes_left -= written;
    }
    return written == length;
}

static bool nor_erase(void *ctx, uint32_t offset, size_t length)
{
    nor_fake_t *nor = ctx;
    if (nor->fail_erase || (offset % ERASE_SIZE) != 0 ||
        (length % ERASE_SIZE) != 0 || offset > PARTITION_SIZE ||
        length > PARTITION_SIZE - offset) {
        return false;
    }
    memset(nor->bytes + offset, 0xff, length);
    nor->erase_calls++;
    return true;
}

static kanji_state_io_t nor_io(nor_fake_t *nor)
{
    kanji_state_io_t io = {
        .read_at = nor_read,
        .write_at = nor_write,
        .erase_range = nor_erase,
        .ctx = nor,
    };
    return io;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void wr32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* Independent fixture CRC: standard reflected CRC-32/ISO-HDLC. */
static uint32_t fixture_crc32(const void *data, size_t length)
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

typedef struct {
    uint32_t sequence;
    uint16_t card;
    uint16_t next;
    uint16_t repetitions;
    uint16_t lapses;
    uint8_t grade;
    uint8_t flags;
    uint32_t stability_milli;
    uint16_t difficulty_milli;
    uint32_t due_minutes;
} fixture_record_t;

/* The schema-2 record, packed independently of kanji_state.c so a transposed
 * pair of fields in production shows up here as a mismatch rather than as two
 * copies of the same mistake agreeing with each other. */
static void encode_record(uint8_t out[RECORD_SIZE], const fixture_record_t *r)
{
    memset(out, 0, RECORD_SIZE);
    wr32(out + 0, r->sequence);
    wr16(out + 4, r->card);
    wr16(out + 6, r->next);
    wr16(out + 8, r->repetitions);
    wr16(out + 10, r->lapses);
    out[12] = r->grade;
    out[13] = r->flags;
    wr32(out + 16, r->stability_milli);
    wr16(out + 20, r->difficulty_milli);
    wr32(out + 24, r->due_minutes);
    wr32(out + 28, fixture_crc32(out, 28));
}

static void encode_header(uint8_t out[HEADER_SIZE], uint32_t generation,
                          const uint8_t catalog_id[16], bool committed)
{
    memset(out, 0xff, HEADER_SIZE);
    memcpy(out + 0, "KJSTATE1", 8);
    wr16(out + 8, 2);
    wr16(out + 10, HEADER_SIZE);
    wr32(out + 12, generation);
    memcpy(out + 16, catalog_id, 16);
    wr32(out + 32, BANK_SIZE);
    wr16(out + 36, RECORD_SIZE);
    wr16(out + 38, 0);
    wr32(out + 40, fixture_crc32(out, 40));
    if (committed) {
        wr32(out + 60, 0x434f4d4du);
    }
}

/* Byte-for-byte what firmware built before the schedule existed: schema 1 and
 * a 20-byte record, both announced in the header, and a header CRC that is
 * perfectly correct for those bytes.  Nothing here is malformed — that is the
 * whole point.  This is a healthy image of the wrong generation, and the only
 * thing standing between it and being replayed at a 32-byte stride is the
 * schema check. */
static void legacy_bank(nor_fake_t *nor, unsigned bank, uint32_t generation,
                        const uint8_t catalog_id[16], uint16_t card,
                        uint16_t next, uint8_t grade)
{
    const uint32_t base = bank * BANK_SIZE;
    uint8_t header[HEADER_SIZE];
    memset(header, 0xff, sizeof header);
    memcpy(header + 0, "KJSTATE1", 8);
    wr16(header + 8, LEGACY_SCHEMA);
    wr16(header + 10, HEADER_SIZE);
    wr32(header + 12, generation);
    memcpy(header + 16, catalog_id, 16);
    wr32(header + 32, BANK_SIZE);
    wr16(header + 36, LEGACY_RECORD_SIZE);
    wr16(header + 38, 0);
    wr32(header + 40, fixture_crc32(header, 40));
    wr32(header + 60, 0x434f4d4du);

    uint8_t record[LEGACY_RECORD_SIZE];
    memset(record, 0, sizeof record);
    wr32(record + 0, 1);
    wr16(record + 4, card);
    wr16(record + 6, next);
    wr16(record + 8, 1);
    wr16(record + 10, 0);
    record[12] = grade;
    record[13] = 0;
    wr32(record + 16, fixture_crc32(record, 16));

    CHECK(nor_erase(nor, base, BANK_SIZE));
    CHECK(nor_write(nor, base, header, sizeof header));
    CHECK(nor_write(nor, base + HEADER_SIZE, record, sizeof record));
}

static void fixture_bank(nor_fake_t *nor, unsigned bank, uint32_t generation,
                         const uint8_t catalog_id[16],
                         const fixture_record_t *records, size_t count)
{
    const uint32_t base = bank * BANK_SIZE;
    uint8_t header[HEADER_SIZE];
    encode_header(header, generation, catalog_id, false);
    CHECK(nor_erase(nor, base, BANK_SIZE));
    CHECK(nor_write(nor, base, header, 60));
    for (size_t i = 0; i < count; i++) {
        uint8_t encoded[RECORD_SIZE];
        encode_record(encoded, &records[i]);
        CHECK(nor_write(nor, base + HEADER_SIZE + (uint32_t)i * RECORD_SIZE,
                        encoded, sizeof encoded));
    }
    const uint8_t commit[4] = { 'M', 'M', 'O', 'C' };
    CHECK(nor_write(nor, base + 60, commit, sizeof commit));
}

/* memset rather than `= {0}`: the summary now contains an int64_t, so it has
 * an alignment tail, and C leaves the value of padding after an initializer
 * unspecified.  A byte comparison against an aggregate the compiler was free
 * to leave dirty is a test that reports the optimiser's mood. */
static bool summary_is_zero(const kanji_rating_summary_t *summary)
{
    kanji_rating_summary_t zero;
    memset(&zero, 0, sizeof zero);
    return memcmp(summary, &zero, sizeof zero) == 0;
}

static void check_summary(const kanji_rating_summary_t *s, uint32_t sequence,
                          uint16_t next, uint16_t repetitions, uint16_t lapses,
                          uint8_t grade, uint8_t flags)
{
    CHECK_U32(s->sequence, sequence);
    CHECK_U32(s->next_ordinal, next);
    CHECK_U32(s->reps, repetitions);
    CHECK_U32(s->lapses, lapses);
    CHECK_U32(s->grade, grade);
    CHECK_U32(s->flags, flags);
}

static void check_schedule(const kanji_rating_summary_t *s,
                           uint32_t stability_milli, uint16_t difficulty_milli,
                           int64_t due_epoch)
{
    CHECK_U32(s->stability_milli, stability_milli);
    CHECK_U32(s->difficulty_milli, difficulty_milli);
    CHECK_I64(s->due_epoch, due_epoch);
}

/* The two conversions the policy layer above kanji_state.c owns.  They live in
 * the test because they are the only doubles anywhere near this journal:
 * kanji_state.c is integer-only on purpose, so the scale it documents is a
 * promise nothing in production can check.  This is where it gets checked. */
static uint32_t stability_to_fixed(double days)
{
    return (uint32_t)llround(days * (double)KANJI_STATE_STABILITY_SCALE);
}

static double stability_from_fixed(uint32_t fixed)
{
    return (double)fixed / (double)KANJI_STATE_STABILITY_SCALE;
}

static uint16_t difficulty_to_fixed(double points)
{
    return (uint16_t)llround(points * (double)KANJI_STATE_DIFFICULTY_SCALE);
}

static double difficulty_from_fixed(uint16_t fixed)
{
    return (double)fixed / (double)KANJI_STATE_DIFFICULTY_SCALE;
}

static void test_erased_first_boot_commits_exact_header_and_zero_state(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    CHECK(nor != NULL);
    if (nor == NULL) return;
    nor_init(nor);
    const uint8_t id[16] = { 0, 1, 2, 3, 4, 5, 6, 7,
                             8, 9, 10, 11, 12, 13, 14, 15 };
    kanji_rating_summary_t summaries[5];
    memset(summaries, 0xa5, sizeof summaries);
    kanji_state_t state;
    kanji_state_io_t io = nor_io(nor);

    CHECK(kanji_state_open(&state, &io, id, 5, summaries));
    CHECK_U32(kanji_state_current_ordinal(&state), 0);
    for (size_t i = 0; i < 5; i++) {
        CHECK(summary_is_zero(&summaries[i]));
        CHECK(kanji_state_summary(&state, (uint16_t)i) == &summaries[i]);
    }
    CHECK(kanji_state_summary(&state, 5) == NULL);

    CHECK(memcmp(nor->bytes, "KJSTATE1", 8) == 0);
    CHECK_U32(rd16(nor->bytes + 8), 2);
    CHECK_U32(rd16(nor->bytes + 10), 64);
    CHECK_U32(rd32(nor->bytes + 12), 0);
    CHECK(memcmp(nor->bytes + 16, id, 16) == 0);
    CHECK_U32(rd32(nor->bytes + 32), BANK_SIZE);
    CHECK_U32(rd16(nor->bytes + 36), RECORD_SIZE);
    CHECK_U32(rd16(nor->bytes + 38), 0);
    CHECK_U32(rd32(nor->bytes + 40), fixture_crc32(nor->bytes, 40));
    for (size_t i = 44; i < 60; i++) CHECK_U32(nor->bytes[i], 0xff);
    CHECK_U32(rd32(nor->bytes + 60), 0x434f4d4du);
    for (size_t i = BANK_SIZE; i < PARTITION_SIZE; i++) {
        if (nor->bytes[i] != 0xff) {
            CHECK(false);
            break;
        }
    }

    kanji_state_t rebooted;
    kanji_rating_summary_t replay[5];
    memset(replay, 0x5a, sizeof replay);
    CHECK(kanji_state_open(&rebooted, &io, id, 5, replay));
    CHECK_U32(kanji_state_current_ordinal(&rebooted), 0);
    for (size_t i = 0; i < 5; i++) CHECK(summary_is_zero(&replay[i]));
    free(nor);
}

static void test_non_erased_header_reserved_tail_is_rejected(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    CHECK(nor != NULL);
    if (nor == NULL) return;
    nor_init(nor);
    const uint8_t id[16] = { 0x17 };
    const fixture_record_t record = {
        .sequence = 1, .card = 0, .next = 1, .repetitions = 1,
        .grade = KANJI_GRADE_GOOD,
    };
    fixture_bank(nor, 0, 7, id, &record, 1);

    /* Bytes 44..59 are outside the header CRC but are fixed erased/reserved
     * bytes in schema 1.  Accepting a programmed future-format byte would
     * silently reinterpret an unsupported header as the current schema. */
    nor->bytes[44] = 0xfe;
    kanji_state_t state;
    kanji_rating_summary_t summaries[2];
    kanji_state_io_t io = nor_io(nor);
    CHECK(kanji_state_open(&state, &io, id, 2, summaries));
    CHECK_U32(kanji_state_current_ordinal(&state), 0);
    CHECK(summary_is_zero(&summaries[0]));
    CHECK(summary_is_zero(&summaries[1]));
    CHECK_U32(state.generation, 0);
    free(nor);
}

static void test_all_grades_and_saturation_survive_reboot(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    CHECK(nor != NULL);
    if (nor == NULL) return;
    nor_init(nor);
    const uint8_t id[16] = { 0x21 };
    kanji_rating_summary_t summaries[4];
    kanji_state_t state;
    kanji_state_io_t io = nor_io(nor);
    CHECK(kanji_state_open(&state, &io, id, 4, summaries));

    CHECK(kanji_state_append_grade(&state, 0, 1, KANJI_GRADE_AGAIN));
    const uint8_t *first = nor->bytes + HEADER_SIZE;
    CHECK_U32(rd32(first + 0), 1);
    CHECK_U32(rd16(first + 4), 0);
    CHECK_U32(rd16(first + 6), 1);
    CHECK_U32(rd16(first + 8), 1);
    CHECK_U32(rd16(first + 10), 1);
    CHECK_U32(first[12], KANJI_GRADE_AGAIN);
    CHECK_U32(first[13], 0);
    CHECK_U32(rd16(first + 14), 0);
    /* A rating appended without a schedule leaves all three schedule fields
     * and both reserved halfwords zero, which is the encoding replay reads
     * back as "reviewed, unscheduled". */
    CHECK_U32(rd32(first + 16), 0);
    CHECK_U32(rd16(first + 20), 0);
    CHECK_U32(rd16(first + 22), 0);
    CHECK_U32(rd32(first + 24), 0);
    CHECK_U32(rd32(first + 28), fixture_crc32(first, 28));
    CHECK(kanji_state_append_grade(&state, 1, 2, KANJI_GRADE_HARD));
    CHECK(kanji_state_append_grade(&state, 2, 3, KANJI_GRADE_GOOD));
    CHECK(kanji_state_append_grade(&state, 3, 0, KANJI_GRADE_EASY));

    kanji_state_t rebooted;
    kanji_rating_summary_t replay[4];
    CHECK(kanji_state_open(&rebooted, &io, id, 4, replay));
    CHECK_U32(kanji_state_current_ordinal(&rebooted), 0);
    for (uint16_t card = 0; card < 4; card++) {
        check_summary(&replay[card], card + 1, (uint16_t)((card + 1) % 4),
                      1, card == 0 ? 1 : 0, (uint8_t)(card + 1), 0);
    }

    /* A hand-derived valid prior summary makes the boundary test fast: the
     * production change this catches is a wrapping ++ on either counter. */
    nor_init(nor);
    const fixture_record_t maxed = {
        .sequence = 7, .card = 0, .next = 0,
        .repetitions = UINT16_MAX, .lapses = UINT16_MAX,
        .grade = KANJI_GRADE_AGAIN,
    };
    fixture_bank(nor, 0, 3, id, &maxed, 1);
    CHECK(kanji_state_open(&state, &io, id, 1, summaries));
    CHECK(kanji_state_append_grade(&state, 0, 0, KANJI_GRADE_AGAIN));
    check_summary(&summaries[0], 8, 0, UINT16_MAX, UINT16_MAX,
                  KANJI_GRADE_AGAIN, 0);
    CHECK(kanji_state_append_grade(&state, 0, 0, KANJI_GRADE_EASY));
    check_summary(&summaries[0], 9, 0, UINT16_MAX, UINT16_MAX,
                  KANJI_GRADE_EASY, 0);
    free(nor);
}

static void test_invalid_input_and_immediate_callback_failure_are_atomic(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    uint8_t *flash_before = malloc(PARTITION_SIZE);
    CHECK(nor != NULL && flash_before != NULL);
    if (nor == NULL || flash_before == NULL) {
        free(nor);
        free(flash_before);
        return;
    }
    nor_init(nor);
    const uint8_t id[16] = { 0x31 };
    kanji_rating_summary_t summaries[3];
    kanji_state_t state;
    kanji_state_io_t io = nor_io(nor);
    CHECK(kanji_state_open(&state, &io, id, 3, summaries));
    kanji_rating_summary_t ram_before[3];
    memcpy(ram_before, summaries, sizeof ram_before);
    memcpy(flash_before, nor->bytes, PARTITION_SIZE);

    CHECK(!kanji_state_append_grade(&state, 3, 0, KANJI_GRADE_GOOD));
    CHECK(!kanji_state_append_grade(&state, 0, 3, KANJI_GRADE_GOOD));
    CHECK(!kanji_state_append_grade(&state, 0, 1, (kanji_grade_t)0));
    CHECK(!kanji_state_append_grade(&state, 0, 1, (kanji_grade_t)5));
    CHECK_U32(kanji_state_current_ordinal(&state), 0);
    CHECK(memcmp(ram_before, summaries, sizeof ram_before) == 0);
    CHECK(memcmp(flash_before, nor->bytes, PARTITION_SIZE) == 0);

    nor->fail_read = true;
    CHECK(!kanji_state_append_grade(&state, 0, 1, KANJI_GRADE_GOOD));
    nor->fail_read = false;
    nor->fail_write = true;
    CHECK(!kanji_state_append_grade(&state, 0, 1, KANJI_GRADE_GOOD));
    nor->fail_write = false;
    nor->drop_write_but_report_success = true;
    CHECK(!kanji_state_append_grade(&state, 0, 1, KANJI_GRADE_GOOD));
    nor->drop_write_but_report_success = false;
    CHECK_U32(kanji_state_current_ordinal(&state), 0);
    CHECK(memcmp(ram_before, summaries, sizeof ram_before) == 0);
    CHECK(memcmp(flash_before, nor->bytes, PARTITION_SIZE) == 0);
    free(flash_before);
    free(nor);
}

static void test_wide_and_negative_congruent_grades_are_atomic(void)
{
    static const int invalid_grades[] = { 257, 258, -255, -254 };
    nor_fake_t *nor = malloc(sizeof *nor);
    uint8_t *flash_before = malloc(PARTITION_SIZE);
    CHECK(nor != NULL && flash_before != NULL);
    if (nor == NULL || flash_before == NULL) {
        free(nor);
        free(flash_before);
        return;
    }

    for (size_t i = 0; i < sizeof invalid_grades / sizeof invalid_grades[0]; i++) {
        nor_init(nor);
        const uint8_t id[16] = { 0x39, (uint8_t)i };
        kanji_state_io_t io = nor_io(nor);
        kanji_state_t state;
        kanji_rating_summary_t summaries[2];
        CHECK(kanji_state_open(&state, &io, id, 2, summaries));
        kanji_rating_summary_t summaries_before[2];
        memcpy(summaries_before, summaries, sizeof summaries_before);
        memcpy(flash_before, nor->bytes, PARTITION_SIZE);

        CHECK(!kanji_state_append_grade(&state, 0, 1,
                                        (kanji_grade_t)invalid_grades[i]));
        CHECK_U32(kanji_state_current_ordinal(&state), 0);
        CHECK(memcmp(summaries_before, summaries, sizeof summaries_before) == 0);
        CHECK(memcmp(flash_before, nor->bytes, PARTITION_SIZE) == 0);
    }
    free(flash_before);
    free(nor);
}

static void test_torn_record_replays_only_the_previous_commit(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    CHECK(nor != NULL);
    if (nor == NULL) return;
    nor_init(nor);
    const uint8_t id[16] = { 0x41 };
    kanji_rating_summary_t summaries[3];
    kanji_state_t state;
    kanji_state_io_t io = nor_io(nor);
    CHECK(kanji_state_open(&state, &io, id, 3, summaries));
    CHECK(kanji_state_append_grade(&state, 0, 1, KANJI_GRADE_GOOD));

    nor->cutoff_enabled = true;
    nor->write_bytes_left = 7;
    CHECK(!kanji_state_append_grade(&state, 1, 2, KANJI_GRADE_AGAIN));
    CHECK_U32(kanji_state_current_ordinal(&state), 1);
    CHECK(summary_is_zero(&summaries[1]));
    nor->cutoff_enabled = false;

    kanji_state_t rebooted;
    kanji_rating_summary_t replay[3];
    CHECK(kanji_state_open(&rebooted, &io, id, 3, replay));
    CHECK_U32(kanji_state_current_ordinal(&rebooted), 1);
    check_summary(&replay[0], 1, 1, 1, 0, KANJI_GRADE_GOOD, 0);
    CHECK(summary_is_zero(&replay[1]));

    /* Ignoring a torn tail must not make its programmed NOR slot permanent:
     * the next grade recovers through the other bank and remains durable. */
    CHECK(kanji_state_append_grade(&rebooted, 1, 2, KANJI_GRADE_HARD));
    CHECK_U32(kanji_state_current_ordinal(&rebooted), 2);
    kanji_state_t recovered;
    kanji_rating_summary_t recovered_summaries[3];
    CHECK(kanji_state_open(&recovered, &io, id, 3, recovered_summaries));
    CHECK_U32(kanji_state_current_ordinal(&recovered), 2);
    check_summary(&recovered_summaries[0], 1, 1, 1, 0,
                  KANJI_GRADE_GOOD, 0);
    check_summary(&recovered_summaries[1], 2, 2, 1, 0,
                  KANJI_GRADE_HARD, 0);
    free(nor);
}

static void test_generation_selection_is_wrap_safe(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    CHECK(nor != NULL);
    if (nor == NULL) return;
    nor_init(nor);
    const uint8_t id[16] = { 0x51 };
    const fixture_record_t old_record = {
        .sequence = 9, .card = 0, .next = 1, .repetitions = 4,
        .grade = KANJI_GRADE_HARD,
    };
    const fixture_record_t wrapped_new_record = {
        .sequence = 10, .card = 1, .next = 2, .repetitions = 5,
        .grade = KANJI_GRADE_EASY,
    };
    fixture_bank(nor, 0, UINT32_MAX, id, &old_record, 1);
    fixture_bank(nor, 1, 0, id, &wrapped_new_record, 1);

    kanji_state_t state;
    kanji_rating_summary_t summaries[3];
    kanji_state_io_t io = nor_io(nor);
    CHECK(kanji_state_open(&state, &io, id, 3, summaries));
    CHECK_U32(kanji_state_current_ordinal(&state), 2);
    CHECK(summary_is_zero(&summaries[0]));
    check_summary(&summaries[1], 10, 2, 5, 0, KANJI_GRADE_EASY, 0);

    /* The same wrap with the physical bank placements reversed must still
     * choose generation zero, not whichever header happened to be read last. */
    nor_init(nor);
    fixture_bank(nor, 0, 0, id, &wrapped_new_record, 1);
    fixture_bank(nor, 1, UINT32_MAX, id, &old_record, 1);
    CHECK(kanji_state_open(&state, &io, id, 3, summaries));
    CHECK_U32(kanji_state_current_ordinal(&state), 2);
    CHECK(summary_is_zero(&summaries[0]));
    check_summary(&summaries[1], 10, 2, 5, 0, KANJI_GRADE_EASY, 0);

    /* At the exactly ambiguous half range neither serial is "newer".  The
     * documented deterministic tie policy is numeric generation, independent
     * of bank placement, so 0x80000000 wins in either bank. */
    nor_init(nor);
    fixture_bank(nor, 0, 0, id, &old_record, 1);
    fixture_bank(nor, 1, UINT32_C(0x80000000), id, &wrapped_new_record, 1);
    CHECK(kanji_state_open(&state, &io, id, 3, summaries));
    CHECK_U32(kanji_state_current_ordinal(&state), 2);

    nor_init(nor);
    fixture_bank(nor, 0, UINT32_C(0x80000000), id, &wrapped_new_record, 1);
    fixture_bank(nor, 1, 0, id, &old_record, 1);
    CHECK(kanji_state_open(&state, &io, id, 3, summaries));
    CHECK_U32(kanji_state_current_ordinal(&state), 2);
    free(nor);
}

/* A full bank with three reviewed cards. Absolute summaries let compaction
 * discard history without changing the observable state. */
static void fixture_full_bank(nor_fake_t *nor, const uint8_t id[16])
{
    const uint32_t base = 0;
    uint8_t header[HEADER_SIZE];
    encode_header(header, 11, id, true);
    CHECK(nor_erase(nor, base, BANK_SIZE));
    CHECK(nor_write(nor, base, header, sizeof header));
    for (uint32_t i = 0; i < RECORD_CAPACITY; i++) {
        const uint16_t card = (uint16_t)(i % 3u);
        /* The schedule depends only on the card, so whichever record for that
         * card happens to be the newest carries the same numbers and the
         * compaction assertions do not have to model the round-robin. */
        const fixture_record_t record = {
            .sequence = i + 1,
            .card = card,
            .next = (uint16_t)((card + 1u) % 3u),
            .repetitions = (uint16_t)(i / 3u + 1u),
            .lapses = card == 0 ? (uint16_t)(i / 3u + 1u) : 0,
            .grade = (uint8_t)(card + 1u),
            .flags = (uint8_t)(0x10u + card),
            .stability_milli = 1500u + 1000u * card,
            .difficulty_milli = (uint16_t)(4000u + 500u * card),
            .due_minutes = 27000000u + card,
        };
        uint8_t encoded[RECORD_SIZE];
        encode_record(encoded, &record);
        CHECK(nor_write(nor, HEADER_SIZE + i * RECORD_SIZE,
                        encoded, sizeof encoded));
    }
}

static void check_full_bank_schedules(const kanji_rating_summary_t summaries[3])
{
    for (uint16_t card = 0; card < 3; card++) {
        check_schedule(&summaries[card], 1500u + 1000u * card,
                       (uint16_t)(4000u + 500u * card),
                       (int64_t)(27000000u + card) * 60);
    }
}

static void check_full_bank_replay(const kanji_rating_summary_t summaries[3])
{
    check_summary(&summaries[0], RECORD_CAPACITY - 2, 1,
                  (uint16_t)(RECORD_CAPACITY / 3),
                  (uint16_t)(RECORD_CAPACITY / 3), 1, 0x10);
    check_summary(&summaries[1], RECORD_CAPACITY - 1, 2,
                  (uint16_t)(RECORD_CAPACITY / 3), 0, 2, 0x11);
    check_summary(&summaries[2], RECORD_CAPACITY, 0,
                  (uint16_t)(RECORD_CAPACITY / 3), 0, 3, 0x12);
    check_full_bank_schedules(summaries);
}

static void test_power_loss_before_compaction_commit_keeps_old_bank(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    uint8_t *old_bank = malloc(BANK_SIZE);
    CHECK(nor != NULL && old_bank != NULL);
    if (nor == NULL || old_bank == NULL) {
        free(nor);
        free(old_bank);
        return;
    }
    nor_init(nor);
    const uint8_t id[16] = { 0x61 };
    fixture_full_bank(nor, id);
    kanji_state_io_t io = nor_io(nor);
    kanji_state_t state;
    kanji_rating_summary_t summaries[3];
    CHECK(kanji_state_open(&state, &io, id, 3, summaries));
    check_full_bank_replay(summaries);
    CHECK_U32(kanji_state_current_ordinal(&state), 0);
    memcpy(old_bank, nor->bytes, BANK_SIZE);

    /* 60-byte header + three copied summaries + two bytes of the commit. */
    nor->cutoff_enabled = true;
    nor->write_bytes_left = 60 + 3 * RECORD_SIZE + 2;
    CHECK(!kanji_state_append_grade(&state, 0, 1, KANJI_GRADE_EASY));
    CHECK_U32(kanji_state_current_ordinal(&state), 0);
    check_full_bank_replay(summaries);
    CHECK(memcmp(old_bank, nor->bytes, BANK_SIZE) == 0);
    nor->cutoff_enabled = false;

    kanji_state_t rebooted;
    kanji_rating_summary_t replay[3];
    CHECK(kanji_state_open(&rebooted, &io, id, 3, replay));
    CHECK_U32(kanji_state_current_ordinal(&rebooted), 0);
    check_full_bank_replay(replay);
    free(old_bank);
    free(nor);
}

static void test_power_loss_after_compaction_commit_selects_new_bank(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    CHECK(nor != NULL);
    if (nor == NULL) return;
    nor_init(nor);
    const uint8_t id[16] = { 0x71 };
    fixture_full_bank(nor, id);
    kanji_state_io_t io = nor_io(nor);
    kanji_state_t state;
    kanji_rating_summary_t summaries[3];
    CHECK(kanji_state_open(&state, &io, id, 3, summaries));

    /* The new bank is fully committed; the requested append is then cut off. */
    nor->cutoff_enabled = true;
    nor->write_bytes_left = 60 + 3 * RECORD_SIZE + 4;
    CHECK(!kanji_state_append_grade(&state, 0, 1, KANJI_GRADE_EASY));
    nor->cutoff_enabled = false;

    kanji_state_t rebooted;
    kanji_rating_summary_t replay[3];
    CHECK(kanji_state_open(&rebooted, &io, id, 3, replay));
    CHECK_U32(kanji_state_current_ordinal(&rebooted), 0);
    check_full_bank_replay(replay);
    CHECK_U32(rebooted.generation, 12);
    free(nor);
}

static void test_successful_compaction_retains_every_latest_summary(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    CHECK(nor != NULL);
    if (nor == NULL) return;
    nor_init(nor);
    const uint8_t id[16] = { 0x81 };
    fixture_full_bank(nor, id);
    kanji_state_io_t io = nor_io(nor);
    kanji_state_t state;
    kanji_rating_summary_t summaries[3];
    CHECK(kanji_state_open(&state, &io, id, 3, summaries));
    CHECK(kanji_state_append_grade(&state, 0, 1, KANJI_GRADE_EASY));
    CHECK_U32(kanji_state_current_ordinal(&state), 1);
    check_summary(&summaries[0], RECORD_CAPACITY + 1, 1,
                  (uint16_t)(RECORD_CAPACITY / 3 + 1),
                  (uint16_t)(RECORD_CAPACITY / 3), KANJI_GRADE_EASY, 0);
    check_summary(&summaries[1], RECORD_CAPACITY - 1, 2,
                  (uint16_t)(RECORD_CAPACITY / 3), 0, 2, 0x11);
    check_summary(&summaries[2], RECORD_CAPACITY, 0,
                  (uint16_t)(RECORD_CAPACITY / 3), 0, 3, 0x12);
    /* Grading without a schedule clears the one the card had.  Carrying it
     * across would leave card 0 claiming a due date computed for a review the
     * learner has now superseded. */
    check_schedule(&summaries[0], 0, 0, 0);
    check_schedule(&summaries[1], 2500, 4500, (int64_t)27000001 * 60);
    check_schedule(&summaries[2], 3500, 5000, (int64_t)27000002 * 60);

    kanji_state_t rebooted;
    kanji_rating_summary_t replay[3];
    CHECK(kanji_state_open(&rebooted, &io, id, 3, replay));
    CHECK_U32(kanji_state_current_ordinal(&rebooted), 1);
    CHECK(memcmp(replay, summaries, sizeof replay) == 0);
    CHECK_U32(rebooted.record_count, 4);
    free(nor);
}

/* Compaction is the one moment every summary is re-encoded from RAM rather
 * than copied, so it is where a schedule field that make_record() forgot would
 * silently become zero for the whole catalog at once. */
static void test_compaction_carries_schedule_fields(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    CHECK(nor != NULL);
    if (nor == NULL) return;
    nor_init(nor);
    const uint8_t id[16] = { 0x85 };
    fixture_full_bank(nor, id);
    kanji_state_io_t io = nor_io(nor);
    kanji_state_t state;
    kanji_rating_summary_t summaries[3];
    CHECK(kanji_state_open(&state, &io, id, 3, summaries));
    check_full_bank_schedules(summaries);

    const kanji_state_schedule_t graded = {
        .due_epoch = 1771000020,
        .stability_milli = 36500000,
        .difficulty_milli = 1000,
    };
    CHECK(kanji_state_append_review(&state, 0, 1, KANJI_GRADE_EASY, &graded));
    CHECK_U32(state.record_count, 4);
    check_schedule(&summaries[0], 36500000, 1000, 1771000020);
    check_schedule(&summaries[1], 2500, 4500, (int64_t)27000001 * 60);
    check_schedule(&summaries[2], 3500, 5000, (int64_t)27000002 * 60);

    kanji_state_t rebooted;
    kanji_rating_summary_t replay[3];
    CHECK(kanji_state_open(&rebooted, &io, id, 3, replay));
    CHECK_U32(kanji_state_current_ordinal(&rebooted), 1);
    CHECK(memcmp(replay, summaries, sizeof replay) == 0);
    check_schedule(&replay[0], 36500000, 1000, 1771000020);
    check_schedule(&replay[1], 2500, 4500, (int64_t)27000001 * 60);
    check_schedule(&replay[2], 3500, 5000, (int64_t)27000002 * 60);

    /* The three records the compaction wrote carry the schedules verbatim,
     * checked against the bank rather than through the reader that produced
     * them.  Ordinal order is what compact() emits. */
    for (uint16_t card = 0; card < 3; card++) {
        const uint8_t *rec = nor->bytes + BANK_SIZE + HEADER_SIZE +
                             card * RECORD_SIZE;
        CHECK_U32(rd16(rec + 4), card);
        CHECK_U32(rd32(rec + 16), 1500u + 1000u * card);
        CHECK_U32(rd16(rec + 20), 4000u + 500u * card);
        CHECK_U32(rd16(rec + 22), 0);
        CHECK_U32(rd32(rec + 24), 27000000u + card);
        CHECK_U32(rd32(rec + 28), fixture_crc32(rec, 28));
    }
    free(nor);
}

static void test_compaction_erase_callback_failure_is_atomic(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    uint8_t *flash_before = malloc(PARTITION_SIZE);
    CHECK(nor != NULL && flash_before != NULL);
    if (nor == NULL || flash_before == NULL) {
        free(nor);
        free(flash_before);
        return;
    }
    nor_init(nor);
    const uint8_t id[16] = { 0x89 };
    fixture_full_bank(nor, id);
    kanji_state_io_t io = nor_io(nor);
    kanji_state_t state;
    kanji_rating_summary_t summaries[3];
    CHECK(kanji_state_open(&state, &io, id, 3, summaries));
    kanji_rating_summary_t summaries_before[3];
    memcpy(summaries_before, summaries, sizeof summaries_before);
    memcpy(flash_before, nor->bytes, PARTITION_SIZE);

    nor->fail_erase = true;
    CHECK(!kanji_state_append_grade(&state, 0, 1, KANJI_GRADE_EASY));
    nor->fail_erase = false;
    CHECK_U32(kanji_state_current_ordinal(&state), 0);
    CHECK(memcmp(summaries_before, summaries, sizeof summaries_before) == 0);
    CHECK(memcmp(flash_before, nor->bytes, PARTITION_SIZE) == 0);
    free(flash_before);
    free(nor);
}

static void check_deceptive_compaction_write_is_rejected(
    int deceptive_write, bool target_commit)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    uint8_t *old_bank = malloc(BANK_SIZE);
    CHECK(nor != NULL && old_bank != NULL);
    if (nor == NULL || old_bank == NULL) {
        free(nor);
        free(old_bank);
        return;
    }
    nor_init(nor);
    const uint8_t id[16] = { 0x8d, (uint8_t)deceptive_write,
                             target_commit ? 1 : 0 };
    fixture_full_bank(nor, id);
    kanji_state_io_t io = nor_io(nor);
    kanji_state_t state;
    kanji_rating_summary_t summaries[3];
    CHECK(kanji_state_open(&state, &io, id, 3, summaries));
    kanji_rating_summary_t summaries_before[3];
    memcpy(summaries_before, summaries, sizeof summaries_before);
    memcpy(old_bank, nor->bytes, BANK_SIZE);

    nor->deceptive_write_offset = BANK_SIZE + (target_commit ? 60u : 0u);
    nor->deceptive_write_length = target_commit ? 4u : 60u;
    nor->deceptive_write = deceptive_write;
    CHECK(!kanji_state_append_grade(&state, 0, 1, KANJI_GRADE_EASY));
    CHECK_U32(kanji_state_current_ordinal(&state), 0);
    CHECK(memcmp(summaries_before, summaries, sizeof summaries_before) == 0);
    CHECK(memcmp(old_bank, nor->bytes, BANK_SIZE) == 0);

    nor->deceptive_write = DECEPTIVE_WRITE_NONE;
    kanji_state_t rebooted;
    kanji_rating_summary_t replay[3];
    CHECK(kanji_state_open(&rebooted, &io, id, 3, replay));
    CHECK_U32(kanji_state_current_ordinal(&rebooted), 0);
    check_full_bank_replay(replay);
    free(old_bank);
    free(nor);
}

static void test_compaction_validates_header_and_commit_before_switch(void)
{
    check_deceptive_compaction_write_is_rejected(DECEPTIVE_WRITE_DROP, false);
    check_deceptive_compaction_write_is_rejected(DECEPTIVE_WRITE_CORRUPT, false);
    check_deceptive_compaction_write_is_rejected(DECEPTIVE_WRITE_DROP, true);
    check_deceptive_compaction_write_is_rejected(DECEPTIVE_WRITE_CORRUPT, true);
}

static void test_catalog_id_mismatch_starts_fresh(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    CHECK(nor != NULL);
    if (nor == NULL) return;
    nor_init(nor);
    const uint8_t old_id[16] = { 0x91 };
    const uint8_t new_id[16] = { 0x92 };
    kanji_state_io_t io = nor_io(nor);
    kanji_state_t old_state;
    kanji_rating_summary_t old_summaries[2];
    CHECK(kanji_state_open(&old_state, &io, old_id, 2, old_summaries));
    CHECK(kanji_state_append_grade(&old_state, 0, 1, KANJI_GRADE_AGAIN));

    kanji_state_t new_state;
    kanji_rating_summary_t new_summaries[2];
    memset(new_summaries, 0xa5, sizeof new_summaries);
    CHECK(kanji_state_open(&new_state, &io, new_id, 2, new_summaries));
    CHECK_U32(kanji_state_current_ordinal(&new_state), 0);
    CHECK(summary_is_zero(&new_summaries[0]));
    CHECK(summary_is_zero(&new_summaries[1]));
    CHECK(kanji_state_append_grade(&new_state, 0, 1, KANJI_GRADE_EASY));

    kanji_state_t rebooted;
    kanji_rating_summary_t replay[2];
    CHECK(kanji_state_open(&rebooted, &io, new_id, 2, replay));
    CHECK_U32(kanji_state_current_ordinal(&rebooted), 1);
    check_summary(&replay[0], 1, 1, 1, 0, KANJI_GRADE_EASY, 0);
    free(nor);
}

static void test_schedule_survives_reboot_and_clears_without_one(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    CHECK(nor != NULL);
    if (nor == NULL) return;
    nor_init(nor);
    const uint8_t id[16] = { 0x95 };
    kanji_state_io_t io = nor_io(nor);
    kanji_state_t state;
    kanji_rating_summary_t summaries[2];
    CHECK(kanji_state_open(&state, &io, id, 2, summaries));

    const kanji_state_schedule_t schedule = {
        .due_epoch = 1770000000,
        .stability_milli = 12345,
        .difficulty_milli = 5432,
    };
    CHECK(kanji_state_append_review(&state, 0, 1, KANJI_GRADE_GOOD, &schedule));
    check_summary(&summaries[0], 1, 1, 1, 0, KANJI_GRADE_GOOD, 0);
    check_schedule(&summaries[0], 12345, 5432, 1770000000);

    const uint8_t *rec = nor->bytes + HEADER_SIZE;
    CHECK_U32(rd32(rec + 16), 12345);
    CHECK_U32(rd16(rec + 20), 5432);
    CHECK_U32(rd16(rec + 22), 0);
    CHECK_U32(rd32(rec + 24), 1770000000u / 60u);
    CHECK_U32(rd32(rec + 28), fixture_crc32(rec, 28));

    kanji_state_t rebooted;
    kanji_rating_summary_t replay[2];
    CHECK(kanji_state_open(&rebooted, &io, id, 2, replay));
    CHECK_U32(kanji_state_current_ordinal(&rebooted), 1);
    check_summary(&replay[0], 1, 1, 1, 0, KANJI_GRADE_GOOD, 0);
    check_schedule(&replay[0], 12345, 5432, 1770000000);
    CHECK(summary_is_zero(&replay[1]));

    /* Back onto card 0, this time from a caller with no scheduler.  The stale
     * due date must not survive: it describes a review that has just been
     * superseded, and nothing downstream could tell it from a live one. */
    CHECK(kanji_state_append_grade(&rebooted, 1, 0, KANJI_GRADE_HARD));
    CHECK(kanji_state_append_grade(&rebooted, 0, 1, KANJI_GRADE_AGAIN));
    check_summary(&replay[0], 3, 1, 2, 1, KANJI_GRADE_AGAIN, 0);
    check_schedule(&replay[0], 0, 0, 0);

    kanji_state_t recovered;
    kanji_rating_summary_t recovered_summaries[2];
    CHECK(kanji_state_open(&recovered, &io, id, 2, recovered_summaries));
    check_schedule(&recovered_summaries[0], 0, 0, 0);
    check_summary(&recovered_summaries[0], 3, 1, 2, 1, KANJI_GRADE_AGAIN, 0);
    free(nor);
}

/* The scales in kanji_state.h are a claim about precision; this is the test
 * that makes them true.  Every value goes to flash and comes back through a
 * fresh open, so what is being measured is the resolution of the stored
 * format, not of the arithmetic that produced it. */
static void test_fixed_point_round_trip_holds_documented_resolution(void)
{
    static const double stabilities[] = { 0.001, 0.5, 1.0, 2.5, 12.3456,
                                          1234.567, 36499.999, 36500.0 };
    static const double difficulties[] = { 1.0, 1.2345, 5.0, 7.77, 9.9999,
                                           10.0 };
    static const int64_t dues[] = { -86400, -1, 0, 1, 59, 60, 61, 1770000000,
                                    1770000059, 4102444800 };
    const size_t n_stability = sizeof stabilities / sizeof stabilities[0];
    const size_t n_difficulty = sizeof difficulties / sizeof difficulties[0];
    const size_t n_due = sizeof dues / sizeof dues[0];

    /* The documented endpoints have to land exactly on the documented
     * integers, or the range checks in kanji_state.c reject the very values
     * py-fsrs clamps to. */
    CHECK_U32(stability_to_fixed(0.001), KANJI_STATE_STABILITY_MIN);
    CHECK_U32(stability_to_fixed(36500.0), KANJI_STATE_STABILITY_MAX);
    CHECK_U32(difficulty_to_fixed(1.0), KANJI_STATE_DIFFICULTY_MIN);
    CHECK_U32(difficulty_to_fixed(10.0), KANJI_STATE_DIFFICULTY_MAX);

    nor_fake_t *nor = malloc(sizeof *nor);
    CHECK(nor != NULL);
    if (nor == NULL) return;
    nor_init(nor);
    const uint8_t id[16] = { 0x99 };
    kanji_state_io_t io = nor_io(nor);
    kanji_state_t state;
    kanji_rating_summary_t summaries[1];
    CHECK(kanji_state_open(&state, &io, id, 1, summaries));

    const double stability_resolution = 0.5 / (double)KANJI_STATE_STABILITY_SCALE;
    const double difficulty_resolution =
        0.5 / (double)KANJI_STATE_DIFFICULTY_SCALE;

    for (size_t i = 0; i < 60; i++) {
        const double stability = stabilities[i % n_stability];
        const double difficulty = difficulties[i % n_difficulty];
        const int64_t due = dues[i % n_due];
        const kanji_state_schedule_t schedule = {
            .due_epoch = due,
            .stability_milli = stability_to_fixed(stability),
            .difficulty_milli = difficulty_to_fixed(difficulty),
        };
        CHECK(kanji_state_append_review(&state, 0, 0, KANJI_GRADE_GOOD,
                                        &schedule));

        kanji_state_t rebooted;
        kanji_rating_summary_t replay[1];
        CHECK(kanji_state_open(&rebooted, &io, id, 1, replay));

        const double got_stability =
            stability_from_fixed(replay[0].stability_milli);
        const double got_difficulty =
            difficulty_from_fixed(replay[0].difficulty_milli);
        if (fabs(got_stability - stability) > stability_resolution + 1e-9) {
            fprintf(stderr, "%s:%d: stability %.6f -> %.6f\n", __FILE__,
                    __LINE__, stability, got_stability);
            failures++;
        }
        if (fabs(got_difficulty - difficulty) > difficulty_resolution + 1e-9) {
            fprintf(stderr, "%s:%d: difficulty %.6f -> %.6f\n", __FILE__,
                    __LINE__, difficulty, got_difficulty);
            failures++;
        }

        /* A due time is truncated down to the whole minute, never up: a card
         * may come due up to 59 s early, and must never sit withheld past the
         * moment the dock promised the learner. */
        const int64_t floored = due <= 0 ? 0 : (due / 60) * 60;
        CHECK_I64(replay[0].due_epoch, floored);
        CHECK(replay[0].due_epoch <= (due > 0 ? due : 0));
        CHECK((due > 0 ? due : 0) - replay[0].due_epoch <
              KANJI_STATE_DUE_TICK_SECONDS);
        /* RAM must already hold the truncated value, or the panel would show a
         * due time the next boot quietly disagrees with. */
        CHECK_I64(summaries[0].due_epoch, replay[0].due_epoch);
    }
    free(nor);
}

static void test_out_of_range_schedule_is_rejected_atomically(void)
{
    static const kanji_state_schedule_t rejected[] = {
        /* An all-zero struct is not "unscheduled" — NULL is.  Accepting it
         * would let a caller half-clear a schedule by forgetting a field. */
        { .due_epoch = 0, .stability_milli = 0, .difficulty_milli = 0 },
        { .due_epoch = 0, .stability_milli = 0, .difficulty_milli = 5000 },
        { .due_epoch = 0, .stability_milli = 5000, .difficulty_milli = 0 },
        { .due_epoch = 0,
          .stability_milli = KANJI_STATE_STABILITY_MAX + 1,
          .difficulty_milli = 5000 },
        { .due_epoch = 0, .stability_milli = 5000,
          .difficulty_milli = KANJI_STATE_DIFFICULTY_MIN - 1 },
        { .due_epoch = 0, .stability_milli = 5000,
          .difficulty_milli = KANJI_STATE_DIFFICULTY_MAX + 1 },
    };
    nor_fake_t *nor = malloc(sizeof *nor);
    uint8_t *flash_before = malloc(PARTITION_SIZE);
    CHECK(nor != NULL && flash_before != NULL);
    if (nor == NULL || flash_before == NULL) {
        free(nor);
        free(flash_before);
        return;
    }
    nor_init(nor);
    const uint8_t id[16] = { 0x9d };
    kanji_state_io_t io = nor_io(nor);
    kanji_state_t state;
    kanji_rating_summary_t summaries[2];
    CHECK(kanji_state_open(&state, &io, id, 2, summaries));
    kanji_rating_summary_t summaries_before[2];
    memcpy(summaries_before, summaries, sizeof summaries_before);
    memcpy(flash_before, nor->bytes, PARTITION_SIZE);

    for (size_t i = 0; i < sizeof rejected / sizeof rejected[0]; i++) {
        CHECK(!kanji_state_append_review(&state, 0, 1, KANJI_GRADE_GOOD,
                                         &rejected[i]));
        CHECK_U32(kanji_state_current_ordinal(&state), 0);
        CHECK(memcmp(summaries_before, summaries, sizeof summaries_before) == 0);
        CHECK(memcmp(flash_before, nor->bytes, PARTITION_SIZE) == 0);
    }

    /* Both endpoints are inside the range, so the check is a clamp and not an
     * off-by-one that silently loses py-fsrs's own extremes. */
    const kanji_state_schedule_t floor_schedule = {
        .due_epoch = 60,
        .stability_milli = KANJI_STATE_STABILITY_MIN,
        .difficulty_milli = KANJI_STATE_DIFFICULTY_MIN,
    };
    CHECK(kanji_state_append_review(&state, 0, 1, KANJI_GRADE_GOOD,
                                    &floor_schedule));
    const kanji_state_schedule_t ceiling_schedule = {
        .due_epoch = 4102444800,
        .stability_milli = KANJI_STATE_STABILITY_MAX,
        .difficulty_milli = KANJI_STATE_DIFFICULTY_MAX,
    };
    CHECK(kanji_state_append_review(&state, 1, 0, KANJI_GRADE_EASY,
                                    &ceiling_schedule));
    check_schedule(&summaries[0], KANJI_STATE_STABILITY_MIN,
                   KANJI_STATE_DIFFICULTY_MIN, 60);
    check_schedule(&summaries[1], KANJI_STATE_STABILITY_MAX,
                   KANJI_STATE_DIFFICULTY_MAX, 4102444800);
    free(flash_before);
    free(nor);
}

/* A record whose CRC is perfect but whose schedule is impossible was not
 * written by this firmware.  Replay has to stop there rather than hand the
 * scheduler a stability of four hundred years. */
static void test_out_of_range_stored_schedule_ends_replay(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    CHECK(nor != NULL);
    if (nor == NULL) return;
    nor_init(nor);
    const uint8_t id[16] = { 0xa1 };
    const fixture_record_t records[2] = {
        { .sequence = 1, .card = 0, .next = 1, .repetitions = 1,
          .grade = KANJI_GRADE_GOOD, .stability_milli = 4000,
          .difficulty_milli = 6000, .due_minutes = 29500000 },
        { .sequence = 2, .card = 1, .next = 0, .repetitions = 1,
          .grade = KANJI_GRADE_EASY,
          .stability_milli = KANJI_STATE_STABILITY_MAX + 1,
          .difficulty_milli = 6000, .due_minutes = 29500000 },
    };
    fixture_bank(nor, 0, 4, id, records, 2);

    kanji_state_t state;
    kanji_rating_summary_t summaries[2];
    kanji_state_io_t io = nor_io(nor);
    CHECK(kanji_state_open(&state, &io, id, 2, summaries));
    CHECK_U32(state.record_count, 1);
    CHECK_U32(kanji_state_current_ordinal(&state), 1);
    check_summary(&summaries[0], 1, 1, 1, 0, KANJI_GRADE_GOOD, 0);
    check_schedule(&summaries[0], 4000, 6000, (int64_t)29500000 * 60);
    CHECK(summary_is_zero(&summaries[1]));

    /* Same shape, but with a reserved halfword programmed instead: reserved
     * bytes are the only room a later schema has, so a record that uses them
     * is from a format this build cannot read. */
    nor_init(nor);
    uint8_t header[HEADER_SIZE];
    encode_header(header, 4, id, false);
    CHECK(nor_erase(nor, 0, BANK_SIZE));
    CHECK(nor_write(nor, 0, header, 60));
    uint8_t good[RECORD_SIZE];
    encode_record(good, &records[0]);
    CHECK(nor_write(nor, HEADER_SIZE, good, sizeof good));
    uint8_t future[RECORD_SIZE];
    fixture_record_t second = records[1];
    second.stability_milli = 4000;
    encode_record(future, &second);
    wr16(future + 22, 0x0001);
    wr32(future + 28, fixture_crc32(future, 28));
    CHECK(nor_write(nor, HEADER_SIZE + RECORD_SIZE, future, sizeof future));
    const uint8_t commit[4] = { 'M', 'M', 'O', 'C' };
    CHECK(nor_write(nor, 60, commit, sizeof commit));

    CHECK(kanji_state_open(&state, &io, id, 2, summaries));
    CHECK_U32(state.record_count, 1);
    CHECK(summary_is_zero(&summaries[1]));
    free(nor);
}

/* The schema bump's whole job.  A schema-1 image is a perfectly healthy bank
 * of the wrong generation: its header CRC is right, its records' CRCs are
 * right, and read at the new 32-byte stride it would resynchronise onto
 * arbitrary offsets.  It has to be refused at the header. */
static void test_old_schema_image_is_rejected_and_starts_fresh(void)
{
    nor_fake_t *nor = malloc(sizeof *nor);
    uint8_t *legacy_before = malloc(BANK_SIZE);
    CHECK(nor != NULL && legacy_before != NULL);
    if (nor == NULL || legacy_before == NULL) {
        free(nor);
        free(legacy_before);
        return;
    }
    nor_init(nor);
    const uint8_t id[16] = { 0xa9 };
    legacy_bank(nor, 0, 5, id, 0, 1, KANJI_GRADE_GOOD);
    memcpy(legacy_before, nor->bytes, BANK_SIZE);

    kanji_state_io_t io = nor_io(nor);
    kanji_state_t state;
    kanji_rating_summary_t summaries[2];
    memset(summaries, 0xa5, sizeof summaries);
    CHECK(kanji_state_open(&state, &io, id, 2, summaries));
    CHECK_U32(kanji_state_current_ordinal(&state), 0);
    CHECK(summary_is_zero(&summaries[0]));
    CHECK(summary_is_zero(&summaries[1]));
    CHECK_U32(state.generation, 0);
    CHECK_U32(state.record_count, 0);

    /* The fresh generation goes to the other bank and the unreadable one is
     * left exactly as found — the same order a catalog change uses, so the new
     * bank is committed before any committed bytes are destroyed. */
    CHECK_U32(state.active_bank, BANK_SIZE);
    CHECK(memcmp(legacy_before, nor->bytes, BANK_SIZE) == 0);
    CHECK(memcmp(nor->bytes + BANK_SIZE, "KJSTATE1", 8) == 0);
    CHECK_U32(rd16(nor->bytes + BANK_SIZE + 8), 2);
    CHECK_U32(rd16(nor->bytes + BANK_SIZE + 36), RECORD_SIZE);
    CHECK_U32(rd32(nor->bytes + BANK_SIZE + 60), 0x434f4d4du);

    const kanji_state_schedule_t schedule = {
        .due_epoch = 1772000040,
        .stability_milli = 9000,
        .difficulty_milli = 3000,
    };
    CHECK(kanji_state_append_review(&state, 0, 1, KANJI_GRADE_EASY, &schedule));

    kanji_state_t rebooted;
    kanji_rating_summary_t replay[2];
    CHECK(kanji_state_open(&rebooted, &io, id, 2, replay));
    CHECK_U32(kanji_state_current_ordinal(&rebooted), 1);
    CHECK_U32(rebooted.active_bank, BANK_SIZE);
    check_summary(&replay[0], 1, 1, 1, 0, KANJI_GRADE_EASY, 0);
    check_schedule(&replay[0], 9000, 3000, 1772000040);

    /* Both banks holding schema-1 images is the same answer: start fresh. */
    nor_init(nor);
    legacy_bank(nor, 0, 5, id, 0, 1, KANJI_GRADE_GOOD);
    legacy_bank(nor, 1, 6, id, 1, 0, KANJI_GRADE_HARD);
    kanji_state_t upgraded;
    kanji_rating_summary_t upgraded_summaries[2];
    CHECK(kanji_state_open(&upgraded, &io, id, 2, upgraded_summaries));
    CHECK_U32(kanji_state_current_ordinal(&upgraded), 0);
    CHECK(summary_is_zero(&upgraded_summaries[0]));
    CHECK(summary_is_zero(&upgraded_summaries[1]));
    CHECK(kanji_state_append_grade(&upgraded, 0, 1, KANJI_GRADE_GOOD));
    kanji_state_t after;
    kanji_rating_summary_t after_summaries[2];
    CHECK(kanji_state_open(&after, &io, id, 2, after_summaries));
    CHECK_U32(kanji_state_current_ordinal(&after), 1);
    check_summary(&after_summaries[0], 1, 1, 1, 0, KANJI_GRADE_GOOD, 0);
    free(legacy_before);
    free(nor);
}

/* The bank arithmetic, asserted rather than assumed.  A record size that does
 * not divide the bank strands bytes; more importantly, compaction writes one
 * record per reviewed card, so this capacity is also the largest catalog the
 * journal can keep grading forever. */
static void test_record_capacity_is_exact_and_documented(void)
{
    CHECK_U32(KANJI_STATE_RECORD_SIZE, RECORD_SIZE);
    CHECK_U32(KANJI_STATE_RECORDS_PER_BANK, RECORD_CAPACITY);
    CHECK_U32(RECORD_CAPACITY, 8190);
    CHECK_U32((BANK_SIZE - HEADER_SIZE) % RECORD_SIZE, 0);
}

int main(void)
{
    test_erased_first_boot_commits_exact_header_and_zero_state();
    test_non_erased_header_reserved_tail_is_rejected();
    test_all_grades_and_saturation_survive_reboot();
    test_invalid_input_and_immediate_callback_failure_are_atomic();
    test_wide_and_negative_congruent_grades_are_atomic();
    test_torn_record_replays_only_the_previous_commit();
    test_generation_selection_is_wrap_safe();
    test_power_loss_before_compaction_commit_keeps_old_bank();
    test_power_loss_after_compaction_commit_selects_new_bank();
    test_successful_compaction_retains_every_latest_summary();
    test_compaction_erase_callback_failure_is_atomic();
    test_compaction_validates_header_and_commit_before_switch();
    test_compaction_carries_schedule_fields();
    test_catalog_id_mismatch_starts_fresh();
    test_schedule_survives_reboot_and_clears_without_one();
    test_fixed_point_round_trip_holds_documented_resolution();
    test_out_of_range_schedule_is_rejected_atomically();
    test_out_of_range_stored_schedule_ends_replay();
    test_old_schema_image_is_rejected_and_starts_fresh();
    test_record_capacity_is_exact_and_documented();

    if (failures != 0) {
        fprintf(stderr, "%d state test failure(s)\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
