/*
 * A byte-accurate NOR fake for the portable rating journal.  Tests never use
 * filesystem persistence: a "reboot" is a fresh kanji_state_t replaying the
 * same injected flash bytes.
 */
#include "kanji_state.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARTITION_SIZE (2u * 256u * 1024u)
#define BANK_SIZE      (256u * 1024u)
#define HEADER_SIZE    64u
#define RECORD_SIZE    20u
#define ERASE_SIZE     4096u
#define RECORD_CAPACITY ((BANK_SIZE - HEADER_SIZE) / RECORD_SIZE)

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

typedef struct {
    uint8_t bytes[PARTITION_SIZE];
    bool fail_read;
    bool fail_write;
    bool fail_erase;
    bool drop_write_but_report_success;
    bool cutoff_enabled;
    size_t write_bytes_left;
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
} fixture_record_t;

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
    wr32(out + 16, fixture_crc32(out, 16));
}

static void encode_header(uint8_t out[HEADER_SIZE], uint32_t generation,
                          const uint8_t catalog_id[16], bool committed)
{
    memset(out, 0xff, HEADER_SIZE);
    memcpy(out + 0, "KJSTATE1", 8);
    wr16(out + 8, 1);
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

static bool summary_is_zero(const kanji_rating_summary_t *summary)
{
    const kanji_rating_summary_t zero = {0};
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
    CHECK_U32(rd16(nor->bytes + 8), 1);
    CHECK_U32(rd16(nor->bytes + 10), 64);
    CHECK_U32(rd32(nor->bytes + 12), 0);
    CHECK(memcmp(nor->bytes + 16, id, 16) == 0);
    CHECK_U32(rd32(nor->bytes + 32), BANK_SIZE);
    CHECK_U32(rd16(nor->bytes + 36), RECORD_SIZE);
    CHECK_U32(rd32(nor->bytes + 40), fixture_crc32(nor->bytes, 40));
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
    CHECK_U32(rd32(first + 16), fixture_crc32(first, 16));
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
        const fixture_record_t record = {
            .sequence = i + 1,
            .card = card,
            .next = (uint16_t)((card + 1u) % 3u),
            .repetitions = (uint16_t)(i / 3u + 1u),
            .lapses = card == 0 ? (uint16_t)(i / 3u + 1u) : 0,
            .grade = (uint8_t)(card + 1u),
            .flags = (uint8_t)(0x10u + card),
        };
        uint8_t encoded[RECORD_SIZE];
        encode_record(encoded, &record);
        CHECK(nor_write(nor, HEADER_SIZE + i * RECORD_SIZE,
                        encoded, sizeof encoded));
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

    kanji_state_t rebooted;
    kanji_rating_summary_t replay[3];
    CHECK(kanji_state_open(&rebooted, &io, id, 3, replay));
    CHECK_U32(kanji_state_current_ordinal(&rebooted), 1);
    CHECK(memcmp(replay, summaries, sizeof replay) == 0);
    CHECK_U32(rebooted.record_count, 4);
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

int main(void)
{
    test_erased_first_boot_commits_exact_header_and_zero_state();
    test_all_grades_and_saturation_survive_reboot();
    test_invalid_input_and_immediate_callback_failure_are_atomic();
    test_torn_record_replays_only_the_previous_commit();
    test_generation_selection_is_wrap_safe();
    test_power_loss_before_compaction_commit_keeps_old_bank();
    test_power_loss_after_compaction_commit_selects_new_bank();
    test_successful_compaction_retains_every_latest_summary();
    test_compaction_erase_callback_failure_is_atomic();
    test_catalog_id_mismatch_starts_fresh();

    if (failures != 0) {
        fprintf(stderr, "%d state test failure(s)\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
