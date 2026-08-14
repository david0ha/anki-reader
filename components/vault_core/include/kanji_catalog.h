/*
 * Bounds-checked reader for the generated offline catalog image.
 *
 * Portable: callers inject storage, zlib, and CRC operations. This header and
 * kanji_catalog.c have no ESP-IDF dependency and are built by the host suite.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kanji_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KANJI_CATALOG_ID_SIZE 16
#define KANJI_CATALOG_MAX_RAW_BLOCK (96u * 1024u)

typedef bool (*kanji_catalog_read_fn)(void *context, uint32_t offset,
                                      void *dst, size_t length);
typedef bool (*kanji_catalog_inflate_fn)(void *dst, size_t *dst_len,
                                         const void *src, size_t src_len);
typedef uint32_t (*kanji_catalog_crc32_fn)(const void *data, size_t length);

typedef struct {
    void *context;
    kanji_catalog_read_fn read;
    kanji_catalog_inflate_fn inflate;
    kanji_catalog_crc32_fn crc32;
} kanji_catalog_io_t;

typedef enum {
    KANJI_CATALOG_OK = 0,
    KANJI_CATALOG_BAD_ARGUMENT,
    KANJI_CATALOG_IO,
    KANJI_CATALOG_BAD_MAGIC,
    KANJI_CATALOG_UNSUPPORTED_VERSION,
    KANJI_CATALOG_HEADER_CRC,
    KANJI_CATALOG_TABLE_CRC,
    KANJI_CATALOG_BOUNDS,
    KANJI_CATALOG_FORMAT,
    KANJI_CATALOG_WORKSPACE,
    KANJI_CATALOG_OUT_OF_RANGE,
    KANJI_CATALOG_DECK_INDEX,
    KANJI_CATALOG_BLOCK_BOUNDS,
    KANJI_CATALOG_INFLATE,
    KANJI_CATALOG_RAW_CRC,
    KANJI_CATALOG_RECORD_OFFSET,
    KANJI_CATALOG_RECORD_LENGTH,
    KANJI_CATALOG_PARSE,
} kanji_catalog_status_t;

typedef struct {
    char level[4];
    char type[8];
    char name[44];
    uint32_t card_count;
} kanji_catalog_deck_info_t;

/* Publicly sized so callers can keep the reader in static storage. Members are
 * implementation state; catalog metadata is exposed by the accessors below. */
typedef struct {
    kanji_catalog_io_t _io;
    uint32_t _partition_size;
    uint32_t _used_size;
    uint32_t _deck_count;
    uint32_t _card_count;
    uint32_t _block_count;
    uint32_t _deck_off;
    uint32_t _card_index_off;
    uint32_t _block_index_off;
    uint32_t _data_off;
    uint8_t _catalog_id[KANJI_CATALOG_ID_SIZE];
    void *_compressed_workspace;
    size_t _compressed_capacity;
    void *_raw_workspace;
    size_t _raw_capacity;
    uint32_t _cached_block;
    size_t _cached_raw_length;
    kanji_catalog_status_t _status;
    bool _open;
} kanji_catalog_t;

bool kanji_catalog_open(kanji_catalog_t *cat,
                        const kanji_catalog_io_t *io,
                        uint32_t partition_size,
                        void *compressed_workspace, size_t compressed_capacity,
                        void *raw_workspace, size_t raw_capacity);

kanji_catalog_status_t kanji_catalog_status(const kanji_catalog_t *cat);
uint32_t kanji_catalog_deck_count(const kanji_catalog_t *cat);
uint32_t kanji_catalog_card_count(const kanji_catalog_t *cat);
uint32_t kanji_catalog_block_count(const kanji_catalog_t *cat);
const uint8_t *kanji_catalog_id(const kanji_catalog_t *cat);

bool kanji_catalog_deck(kanji_catalog_t *cat, uint32_t deck_index,
                        kanji_catalog_deck_info_t *out);
bool kanji_catalog_read_card(kanji_catalog_t *cat, uint32_t ordinal,
                             kanji_t *out);

#ifdef __cplusplus
}
#endif
