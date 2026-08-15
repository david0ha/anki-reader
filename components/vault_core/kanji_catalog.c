#include "kanji_catalog.h"

#include <limits.h>
#include <string.h>

#include "kanji_parse.h"

#define CATALOG_HEADER_SIZE 128u
#define CATALOG_DECK_SIZE 64u
#define CATALOG_CARD_INDEX_SIZE 12u
#define CATALOG_BLOCK_INDEX_SIZE 16u
#define CATALOG_BLOCK_CARDS 64u
#define CATALOG_PARTITION_MAX 0x770000u
#define NO_CACHED_BLOCK UINT32_MAX

static const uint8_t CATALOG_MAGIC[8] = {'K', 'J', 'C', 'A', 'T', '0', '1', 0};

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool add_u32(uint32_t a, uint32_t b, uint32_t *result)
{
    if (b > UINT32_MAX - a) return false;
    *result = a + b;
    return true;
}

static bool mul_u32(uint32_t a, uint32_t b, uint32_t *result)
{
    if (a != 0 && b > UINT32_MAX / a) return false;
    *result = a * b;
    return true;
}

static bool span_ok(uint32_t offset, size_t length, uint32_t limit)
{
    return offset <= limit && length <= (size_t)(limit - offset);
}

static bool read_bytes(kanji_catalog_t *cat, uint32_t offset,
                       void *dst, size_t length, uint32_t limit,
                       kanji_catalog_status_t status)
{
    if (!span_ok(offset, length, limit)) {
        cat->_status = status;
        return false;
    }
    if (!cat->_io.read(cat->_io.context, offset, dst, length)) {
        cat->_status = KANJI_CATALOG_IO;
        return false;
    }
    return true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    crc = ~crc;
    while (length--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

static bool table_crc_ok(kanji_catalog_t *cat, uint32_t offset,
                         uint32_t length, uint32_t expected)
{
    uint32_t crc = 0;
    uint32_t cursor = offset;
    uint32_t remaining = length;
    while (remaining) {
        size_t chunk = remaining;
        if (chunk > cat->_compressed_capacity) chunk = cat->_compressed_capacity;
        if (chunk == 0 || !read_bytes(cat, cursor, cat->_compressed_workspace,
                                      chunk, cat->_partition_size,
                                      KANJI_CATALOG_BOUNDS)) {
            return false;
        }
        crc = crc32_update(crc, cat->_compressed_workspace, chunk);
        cursor += (uint32_t)chunk;
        remaining -= (uint32_t)chunk;
    }
    if (crc != expected) {
        cat->_status = KANJI_CATALOG_TABLE_CRC;
        return false;
    }
    return true;
}

static bool valid_utf8(const uint8_t *text, size_t length)
{
    for (size_t i = 0; i < length;) {
        uint8_t first = text[i++];
        if (first < 0x80) continue;
        unsigned tails;
        uint32_t codepoint;
        uint32_t minimum;
        if (first >= 0xc2 && first <= 0xdf) {
            tails = 1; codepoint = first & 0x1fu; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            tails = 2; codepoint = first & 0x0fu; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            tails = 3; codepoint = first & 0x07u; minimum = 0x10000;
        } else {
            return false;
        }
        if (tails > length - i) return false;
        for (unsigned tail = 0; tail < tails; tail++) {
            uint8_t next = text[i++];
            if ((next & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6) | (next & 0x3fu);
        }
        if (codepoint < minimum || codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu)) return false;
    }
    return true;
}

static bool decode_string(const uint8_t *field, size_t field_size,
                          char *out, size_t out_size)
{
    size_t length = 0;
    while (length < field_size && field[length]) length++;
    if (length == 0 || length == field_size || length >= out_size) return false;
    for (size_t i = length + 1; i < field_size; i++) {
        if (field[i] != 0) return false;
    }
    if (!valid_utf8(field, length)) return false;
    memcpy(out, field, length);
    out[length] = '\0';
    return true;
}

static bool decode_deck_record(const uint8_t record[CATALOG_DECK_SIZE],
                               kanji_catalog_deck_info_t *deck)
{
    kanji_catalog_deck_info_t temporary = {0};
    if (!decode_string(record, 4, temporary.level, sizeof temporary.level) ||
        !decode_string(record + 4, 8, temporary.type, sizeof temporary.type) ||
        !decode_string(record + 12, 44, temporary.name, sizeof temporary.name)) {
        return false;
    }
    if (!((temporary.level[0] == 'N') && temporary.level[1] >= '1' &&
          temporary.level[1] <= '5' && temporary.level[2] == '\0')) {
        return false;
    }
    if (strcmp(temporary.type, "kanji") != 0 &&
        strcmp(temporary.type, "vocab") != 0 &&
        strcmp(temporary.type, "kana") != 0) {
        return false;
    }
    temporary.card_count = le32(record + 56);
    if (le32(record + 60) != 0) return false;
    *deck = temporary;
    return true;
}

static bool validate_index_structure(kanji_catalog_t *cat,
                                     uint32_t remaining_deck_counts[256])
{
    uint32_t compressed_cursor = cat->_data_off;
    uint32_t ordinal = 0;

    for (uint32_t block_id = 0; block_id < cat->_block_count; block_id++) {
        uint32_t relative;
        uint32_t block_entry_offset;
        uint8_t block_entry[CATALOG_BLOCK_INDEX_SIZE];
        if (!mul_u32(block_id, CATALOG_BLOCK_INDEX_SIZE, &relative) ||
            !add_u32(cat->_block_index_off, relative, &block_entry_offset) ||
            !read_bytes(cat, block_entry_offset, block_entry, sizeof block_entry,
                        cat->_data_off, KANJI_CATALOG_BLOCK_BOUNDS)) {
            return false;
        }
        uint32_t compressed_off = le32(block_entry);
        uint32_t compressed_len = le32(block_entry + 4);
        uint32_t raw_len = le32(block_entry + 8);
        uint32_t compressed_end;
        if (compressed_off != compressed_cursor || compressed_len == 0 ||
            raw_len == 0 || raw_len > KANJI_CATALOG_MAX_RAW_BLOCK ||
            !span_ok(compressed_off, compressed_len, cat->_used_size) ||
            !add_u32(compressed_off, compressed_len, &compressed_end)) {
            cat->_status = KANJI_CATALOG_BLOCK_BOUNDS;
            return false;
        }
        compressed_cursor = compressed_end;

        uint32_t cards_in_block = cat->_card_count - ordinal;
        if (cards_in_block > CATALOG_BLOCK_CARDS) {
            cards_in_block = CATALOG_BLOCK_CARDS;
        }
        uint32_t raw_cursor = 0;
        for (uint32_t slot = 0; slot < cards_in_block; slot++, ordinal++) {
            uint32_t card_relative;
            uint32_t card_entry_offset;
            uint8_t card_entry[CATALOG_CARD_INDEX_SIZE];
            if (!mul_u32(ordinal, CATALOG_CARD_INDEX_SIZE, &card_relative) ||
                !add_u32(cat->_card_index_off, card_relative,
                         &card_entry_offset) ||
                !read_bytes(cat, card_entry_offset, card_entry,
                            sizeof card_entry, cat->_block_index_off,
                            KANJI_CATALOG_BOUNDS)) {
                return false;
            }
            uint32_t record_off = le32(card_entry);
            uint32_t record_len = le32(card_entry + 4);
            uint32_t deck_index = card_entry[8];
            if (card_entry[9] || card_entry[10] || card_entry[11]) {
                cat->_status = KANJI_CATALOG_FORMAT;
                return false;
            }
            if (deck_index >= cat->_deck_count) {
                cat->_status = KANJI_CATALOG_DECK_INDEX;
                return false;
            }
            if (record_off != raw_cursor) {
                cat->_status = KANJI_CATALOG_RECORD_OFFSET;
                return false;
            }
            if (raw_cursor > raw_len || record_len == 0 ||
                record_len > raw_len - raw_cursor ||
                !add_u32(raw_cursor, record_len, &raw_cursor)) {
                cat->_status = KANJI_CATALOG_RECORD_LENGTH;
                return false;
            }
            if (remaining_deck_counts[deck_index] == 0) {
                cat->_status = KANJI_CATALOG_FORMAT;
                return false;
            }
            remaining_deck_counts[deck_index]--;
        }
        if (raw_cursor != raw_len) {
            cat->_status = KANJI_CATALOG_RECORD_LENGTH;
            return false;
        }
    }
    if (ordinal != cat->_card_count || compressed_cursor != cat->_used_size) {
        cat->_status = KANJI_CATALOG_BLOCK_BOUNDS;
        return false;
    }
    for (uint32_t deck = 0; deck < cat->_deck_count; deck++) {
        if (remaining_deck_counts[deck] != 0) {
            cat->_status = KANJI_CATALOG_FORMAT;
            return false;
        }
    }
    return true;
}

bool kanji_catalog_open(kanji_catalog_t *cat,
                        const kanji_catalog_io_t *io,
                        uint32_t partition_size,
                        void *compressed_workspace, size_t compressed_capacity,
                        void *raw_workspace, size_t raw_capacity)
{
    if (!cat) return false;
    memset(cat, 0, sizeof *cat);
    cat->_cached_block = NO_CACHED_BLOCK;
    cat->_status = KANJI_CATALOG_BAD_ARGUMENT;
    if (!io || !io->read || !io->inflate || !io->crc32 ||
        !compressed_workspace || compressed_capacity == 0 ||
        !raw_workspace || raw_capacity == 0) {
        return false;
    }
    cat->_io = *io;
    cat->_partition_size = partition_size;
    cat->_compressed_workspace = compressed_workspace;
    cat->_compressed_capacity = compressed_capacity;
    cat->_raw_workspace = raw_workspace;
    cat->_raw_capacity = raw_capacity;
    if (partition_size < CATALOG_HEADER_SIZE || partition_size > CATALOG_PARTITION_MAX) {
        cat->_status = KANJI_CATALOG_BOUNDS;
        return false;
    }

    uint8_t header[CATALOG_HEADER_SIZE];
    if (!read_bytes(cat, 0, header, sizeof header, partition_size,
                    KANJI_CATALOG_BOUNDS)) return false;
    if (memcmp(header, CATALOG_MAGIC, sizeof CATALOG_MAGIC) != 0) {
        cat->_status = KANJI_CATALOG_BAD_MAGIC;
        return false;
    }
    if (le16(header + 8) != 1 || le16(header + 10) != CATALOG_HEADER_SIZE) {
        cat->_status = KANJI_CATALOG_UNSUPPORTED_VERSION;
        return false;
    }
    if (io->crc32(header, 120) != le32(header + 120)) {
        cat->_status = KANJI_CATALOG_HEADER_CRC;
        return false;
    }

    uint32_t flags = le32(header + 12);
    uint32_t used_size = le32(header + 16);
    uint32_t deck_count = le16(header + 20);
    uint32_t block_cards = le16(header + 22);
    uint32_t card_count = le32(header + 24);
    uint32_t block_count = le32(header + 28);
    uint32_t deck_off = le32(header + 32);
    uint32_t deck_len = le32(header + 36);
    uint32_t card_index_off = le32(header + 40);
    uint32_t card_index_len = le32(header + 44);
    uint32_t block_index_off = le32(header + 48);
    uint32_t block_index_len = le32(header + 52);
    uint32_t data_off = le32(header + 56);
    uint32_t data_len = le32(header + 60);
    uint32_t expected_deck_len;
    uint32_t expected_card_index_len;
    uint32_t expected_block_index_len;
    uint32_t next;

    if (used_size > partition_size || used_size > CATALOG_PARTITION_MAX ||
        used_size < CATALOG_HEADER_SIZE || deck_count == 0 || deck_count > 255 ||
        card_count == 0 || flags != 0 || block_cards != CATALOG_BLOCK_CARDS) {
        cat->_status = KANJI_CATALOG_BOUNDS;
        return false;
    }
    uint32_t expected_blocks = card_count / CATALOG_BLOCK_CARDS +
                               (card_count % CATALOG_BLOCK_CARDS != 0);
    if (block_count != expected_blocks ||
        !mul_u32(deck_count, CATALOG_DECK_SIZE, &expected_deck_len) ||
        !mul_u32(card_count, CATALOG_CARD_INDEX_SIZE, &expected_card_index_len) ||
        !mul_u32(block_count, CATALOG_BLOCK_INDEX_SIZE, &expected_block_index_len) ||
        deck_off != CATALOG_HEADER_SIZE || deck_len != expected_deck_len ||
        !add_u32(deck_off, deck_len, &next) || card_index_off != next ||
        card_index_len != expected_card_index_len ||
        !add_u32(card_index_off, card_index_len, &next) || block_index_off != next ||
        block_index_len != expected_block_index_len ||
        !add_u32(block_index_off, block_index_len, &next) || data_off != next ||
        !add_u32(data_off, data_len, &next) || used_size != next) {
        cat->_status = KANJI_CATALOG_BOUNDS;
        return false;
    }

    cat->_used_size = used_size;
    cat->_deck_count = deck_count;
    cat->_card_count = card_count;
    cat->_block_count = block_count;
    cat->_deck_off = deck_off;
    cat->_card_index_off = card_index_off;
    cat->_block_index_off = block_index_off;
    cat->_data_off = data_off;
    memcpy(cat->_catalog_id, header + 72, sizeof cat->_catalog_id);

    if (!table_crc_ok(cat, deck_off, data_off - deck_off, le32(header + 124))) {
        return false;
    }

    uint64_t deck_card_total = 0;
    uint32_t remaining_deck_counts[256] = {0};
    for (uint32_t index = 0; index < deck_count; index++) {
        uint32_t offset;
        uint8_t record[CATALOG_DECK_SIZE];
        kanji_catalog_deck_info_t deck;
        if (!mul_u32(index, CATALOG_DECK_SIZE, &offset) ||
            !add_u32(deck_off, offset, &offset) ||
            !read_bytes(cat, offset, record, sizeof record, data_off,
                        KANJI_CATALOG_BOUNDS)) return false;
        if (!decode_deck_record(record, &deck)) {
            cat->_status = KANJI_CATALOG_FORMAT;
            return false;
        }
        deck_card_total += deck.card_count;
        remaining_deck_counts[index] = deck.card_count;
    }
    if (deck_card_total != card_count) {
        cat->_status = KANJI_CATALOG_FORMAT;
        return false;
    }
    if (!validate_index_structure(cat, remaining_deck_counts)) return false;

    cat->_open = true;
    cat->_status = KANJI_CATALOG_OK;
    return true;
}

kanji_catalog_status_t kanji_catalog_status(const kanji_catalog_t *cat)
{
    return cat ? cat->_status : KANJI_CATALOG_BAD_ARGUMENT;
}

uint32_t kanji_catalog_deck_count(const kanji_catalog_t *cat)
{
    return cat && cat->_open ? cat->_deck_count : 0;
}

uint32_t kanji_catalog_card_count(const kanji_catalog_t *cat)
{
    return cat && cat->_open ? cat->_card_count : 0;
}

uint32_t kanji_catalog_block_count(const kanji_catalog_t *cat)
{
    return cat && cat->_open ? cat->_block_count : 0;
}

const uint8_t *kanji_catalog_id(const kanji_catalog_t *cat)
{
    return cat && cat->_open ? cat->_catalog_id : NULL;
}

bool kanji_catalog_deck(kanji_catalog_t *cat, uint32_t deck_index,
                        kanji_catalog_deck_info_t *out)
{
    if (!cat || !cat->_open || !out) {
        if (cat) cat->_status = KANJI_CATALOG_BAD_ARGUMENT;
        return false;
    }
    if (deck_index >= cat->_deck_count) {
        cat->_status = KANJI_CATALOG_OUT_OF_RANGE;
        return false;
    }
    uint32_t relative;
    uint32_t offset;
    uint8_t record[CATALOG_DECK_SIZE];
    kanji_catalog_deck_info_t temporary;
    if (!mul_u32(deck_index, CATALOG_DECK_SIZE, &relative) ||
        !add_u32(cat->_deck_off, relative, &offset) ||
        !read_bytes(cat, offset, record, sizeof record, cat->_card_index_off,
                    KANJI_CATALOG_BOUNDS)) return false;
    if (!decode_deck_record(record, &temporary)) {
        cat->_status = KANJI_CATALOG_FORMAT;
        return false;
    }
    *out = temporary;
    cat->_status = KANJI_CATALOG_OK;
    return true;
}

static bool load_block(kanji_catalog_t *cat, uint32_t block_id)
{
    if (cat->_cached_block == block_id) return true;

    /* Drop the cache here, before a single byte of either workspace is written,
     * and do not name a block again until its raw bytes have been both inflated
     * and CRC-checked at the bottom of this function. The state being prevented
     * is "_cached_block says block A while _raw_workspace holds half of block
     * B", and two exits below produce exactly that: inflate writes however many
     * bytes it managed before it gave up, and a CRC mismatch means the bytes now
     * sitting in _raw_workspace are wrong by definition. Both then return false
     * with the workspace already clobbered.
     *
     * If the cache still named block A after one of those, the next read of any
     * card in block A would take the early return above, skip the inflate and
     * the CRC that would have caught the damage, parse whatever of block B is
     * lying in the workspace, and return TRUE with a corrupt card. Nothing logs
     * that, and the stored raw CRC cannot save us, because it is only checked on
     * the load path the early return just skipped.
     *
     * Invalidating up front rather than at those two exits is the point. The
     * earlier failures here — the block-index read, the bounds checks, the
     * workspace-capacity check — leave _raw_workspace untouched and would be
     * safe either way, but one invalidation at the top means no later edit can
     * reopen the window by adding a `return false` after a write. Clearing
     * _cached_raw_length with it keeps the pair honest: a raw length means
     * nothing while no block is named. */
    cat->_cached_block = NO_CACHED_BLOCK;
    cat->_cached_raw_length = 0;

    uint32_t relative;
    uint32_t entry_offset;
    uint8_t entry[CATALOG_BLOCK_INDEX_SIZE];
    if (!mul_u32(block_id, CATALOG_BLOCK_INDEX_SIZE, &relative) ||
        !add_u32(cat->_block_index_off, relative, &entry_offset) ||
        !read_bytes(cat, entry_offset, entry, sizeof entry, cat->_data_off,
                    KANJI_CATALOG_BLOCK_BOUNDS)) return false;
    uint32_t compressed_off = le32(entry);
    uint32_t compressed_len = le32(entry + 4);
    uint32_t raw_len = le32(entry + 8);
    uint32_t raw_crc = le32(entry + 12);
    if (compressed_off < cat->_data_off || compressed_len == 0 ||
        !span_ok(compressed_off, compressed_len, cat->_used_size)) {
        cat->_status = KANJI_CATALOG_BLOCK_BOUNDS;
        return false;
    }
    if (compressed_len > cat->_compressed_capacity || raw_len == 0 ||
        raw_len > KANJI_CATALOG_MAX_RAW_BLOCK || raw_len > cat->_raw_capacity) {
        cat->_status = KANJI_CATALOG_WORKSPACE;
        return false;
    }
    if (!read_bytes(cat, compressed_off, cat->_compressed_workspace,
                    compressed_len, cat->_used_size,
                    KANJI_CATALOG_BLOCK_BOUNDS)) return false;
    size_t actual = raw_len;
    if (!cat->_io.inflate(cat->_raw_workspace, &actual,
                          cat->_compressed_workspace, compressed_len) ||
        actual != raw_len) {
        cat->_status = KANJI_CATALOG_INFLATE;
        return false;
    }
    if (cat->_io.crc32(cat->_raw_workspace, raw_len) != raw_crc) {
        cat->_status = KANJI_CATALOG_RAW_CRC;
        return false;
    }
    cat->_cached_block = block_id;
    cat->_cached_raw_length = raw_len;
    return true;
}

bool kanji_catalog_read_card(kanji_catalog_t *cat, uint32_t ordinal,
                             kanji_t *out, kanji_t *workspace)
{
    if (!cat || !cat->_open || !out || !workspace || workspace == out) {
        if (cat) cat->_status = KANJI_CATALOG_BAD_ARGUMENT;
        return false;
    }
    if (ordinal >= cat->_card_count) {
        cat->_status = KANJI_CATALOG_OUT_OF_RANGE;
        return false;
    }
    uint32_t block_id = ordinal / CATALOG_BLOCK_CARDS;
    if (!load_block(cat, block_id)) return false;

    uint32_t relative;
    uint32_t entry_offset;
    uint8_t entry[CATALOG_CARD_INDEX_SIZE];
    if (!mul_u32(ordinal, CATALOG_CARD_INDEX_SIZE, &relative) ||
        !add_u32(cat->_card_index_off, relative, &entry_offset) ||
        !read_bytes(cat, entry_offset, entry, sizeof entry,
                    cat->_block_index_off, KANJI_CATALOG_BOUNDS)) return false;
    uint32_t record_off = le32(entry);
    uint32_t record_len = le32(entry + 4);
    uint32_t deck_index = entry[8];
    if (entry[9] || entry[10] || entry[11]) {
        cat->_status = KANJI_CATALOG_FORMAT;
        return false;
    }
    if (deck_index >= cat->_deck_count) {
        cat->_status = KANJI_CATALOG_DECK_INDEX;
        return false;
    }
    if (record_off >= cat->_cached_raw_length) {
        cat->_status = KANJI_CATALOG_RECORD_OFFSET;
        return false;
    }
    if (record_len == 0 || record_len > cat->_cached_raw_length - record_off) {
        cat->_status = KANJI_CATALOG_RECORD_LENGTH;
        return false;
    }

    kanji_catalog_deck_info_t deck;
    if (!kanji_catalog_deck(cat, deck_index, &deck)) return false;
    if (!kanji_parse_with_workspace(
            (const char *)cat->_raw_workspace + record_off, record_len,
            out, workspace)) {
        cat->_status = KANJI_CATALOG_PARSE;
        return false;
    }
    kanji_str_copy(out->session.deck, sizeof out->session.deck, deck.name);
    kanji_str_copy(out->session.level, sizeof out->session.level, deck.level);
    out->source = KANJI_SOURCE_CATALOG;
    cat->_status = KANJI_CATALOG_OK;
    return true;
}
