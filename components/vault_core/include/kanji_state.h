/*
 * kanji_state.h — portable, power-loss-safe offline rating journal.
 *
 * Storage is a raw 512 KiB NOR partition.  The caller owns both the flash
 * callbacks and one summary per catalog card; this module allocates nothing
 * and has no ESP-IDF or filesystem dependency.
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
#define KANJI_STATE_RECORD_SIZE    20u

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

/* Complete absolute state from the newest record for one card.  sequence == 0
 * and grade == 0 describes an unreviewed card after open. Field names mirror
 * the fixed record encoding so the ESP adapter has no semantic translation. */
typedef struct {
    uint32_t sequence;
    uint16_t next_ordinal;
    uint16_t reps;
    uint16_t lapses;
    uint8_t grade;
    uint8_t flags;
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
 * partition or catalog-id mismatch creates a fresh committed generation with
 * current ordinal zero. */
bool kanji_state_open(kanji_state_t *state, const kanji_state_io_t *io,
                      const uint8_t catalog_id[16], uint16_t card_count,
                      kanji_rating_summary_t summaries[]);

uint16_t kanji_state_current_ordinal(const kanji_state_t *state);

/* Returns NULL for an unopened state or an ordinal outside the catalog. */
const kanji_rating_summary_t *kanji_state_summary(const kanji_state_t *state,
                                                  uint16_t ordinal);

/* Persist one outcome and verify it before changing RAM.  This records only
 * rating/repetition/lapse/current-position state; it deliberately does not
 * calculate FSRS due dates, stability, or wall-clock scheduling. */
bool kanji_state_append_grade(kanji_state_t *state, uint16_t ordinal,
                              uint16_t next_ordinal, kanji_grade_t grade);

#ifdef __cplusplus
}
#endif
