#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "kanji_catalog.h"

static int failures;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; \
} } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_STREQ(a, b) CHECK(strcmp((a), (b)) == 0)

typedef struct {
    uint8_t *bytes;
    size_t length;
} image_t;

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static image_t load_image(const char *path)
{
    image_t image = {0};
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    if (!file) return image;
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    CHECK(length > 0);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    if (length > 0) {
        image.length = (size_t)length;
        image.bytes = malloc(image.length);
        CHECK(image.bytes != NULL);
        if (image.bytes) CHECK_EQ(fread(image.bytes, 1, image.length, file), image.length);
    }
    CHECK(fclose(file) == 0);
    return image;
}

static image_t clone_image(const image_t *source)
{
    image_t copy = {.length = source->length};
    copy.bytes = malloc(copy.length);
    CHECK(copy.bytes != NULL);
    if (copy.bytes) memcpy(copy.bytes, source->bytes, copy.length);
    return copy;
}

static bool read_at(void *context, uint32_t offset, void *dst, size_t length)
{
    image_t *image = context;
    if ((size_t)offset > image->length || length > image->length - (size_t)offset) {
        return false;
    }
    memcpy(dst, image->bytes + offset, length);
    return true;
}

static bool inflate_exact(void *dst, size_t *dst_len,
                          const void *src, size_t src_len)
{
    z_stream stream = {0};
    stream.next_in = (Bytef *)src;
    stream.avail_in = (uInt)src_len;
    stream.next_out = dst;
    stream.avail_out = (uInt)*dst_len;
    if (inflateInit(&stream) != Z_OK) return false;
    int result = inflate(&stream, Z_FINISH);
    bool ok = result == Z_STREAM_END && stream.avail_in == 0 &&
              stream.total_out == *dst_len;
    *dst_len = (size_t)stream.total_out;
    inflateEnd(&stream);
    return ok;
}

static bool fail_next_inflate_after_write;

static bool inflate_exact_with_recoverable_failure(void *dst, size_t *dst_len,
                                                   const void *src,
                                                   size_t src_len)
{
    bool ok = inflate_exact(dst, dst_len, src, src_len);
    if (ok && fail_next_inflate_after_write) {
        fail_next_inflate_after_write = false;
        return false;
    }
    return ok;
}

static uint32_t catalog_crc32(const void *data, size_t length)
{
    return (uint32_t)crc32(0, data, (uInt)length);
}

/* Arming this makes exactly the next CRC come out wrong, which is how a block
 * whose raw bytes inflated cleanly still gets rejected. Corrupting the stored
 * CRC in the image cannot reach the same state: the reader would then reject
 * the block on its very first load, and the defect under test needs a block
 * that loaded fine once and only fails on the second, different block. */
static bool fail_next_raw_crc;

static uint32_t catalog_crc32_with_recoverable_failure(const void *data,
                                                       size_t length)
{
    uint32_t crc = catalog_crc32(data, length);
    if (fail_next_raw_crc) {
        fail_next_raw_crc = false;
        return ~crc;
    }
    return crc;
}

static kanji_catalog_io_t io_for(image_t *image)
{
    kanji_catalog_io_t io = {
        .context = image,
        .read = read_at,
        .inflate = inflate_exact,
        .crc32 = catalog_crc32,
    };
    return io;
}

static bool open_catalog(image_t *image, uint32_t partition_size,
                         kanji_catalog_t *catalog,
                         uint8_t *compressed, size_t compressed_size,
                         uint8_t *raw, size_t raw_size)
{
    kanji_catalog_io_t io = io_for(image);
    return kanji_catalog_open(catalog, &io, partition_size,
                              compressed, compressed_size, raw, raw_size);
}

static void repair_header_crc(image_t *image)
{
    put_u32(image->bytes + 120, catalog_crc32(image->bytes, 120));
}

static void repair_table_crc(image_t *image)
{
    uint32_t deck_off = get_u32(image->bytes + 32);
    uint32_t data_off = get_u32(image->bytes + 56);
    put_u32(image->bytes + 124,
            catalog_crc32(image->bytes + deck_off, data_off - deck_off));
}

static void expect_open_status(const image_t *base, void (*mutate)(image_t *),
                               uint32_t partition_size,
                               kanji_catalog_status_t expected)
{
    image_t image = clone_image(base);
    uint8_t compressed[65536];
    uint8_t raw[98304];
    kanji_catalog_t catalog;
    mutate(&image);
    CHECK(!open_catalog(&image, partition_size ? partition_size : (uint32_t)image.length,
                        &catalog, compressed, sizeof compressed, raw, sizeof raw));
    CHECK_EQ(kanji_catalog_status(&catalog), expected);
    free(image.bytes);
}

static void expect_image_open_status(image_t *image,
                                     kanji_catalog_status_t expected)
{
    uint8_t compressed[65536];
    uint8_t raw[98304];
    kanji_catalog_t catalog;
    CHECK(!open_catalog(image, (uint32_t)image->length, &catalog,
                        compressed, sizeof compressed, raw, sizeof raw));
    CHECK_EQ(kanji_catalog_status(&catalog), expected);
}

static void corrupt_magic(image_t *image) { image->bytes[0] ^= 1; }
static void corrupt_version(image_t *image) { image->bytes[8] ^= 1; }
static void corrupt_header_crc(image_t *image) { image->bytes[120] ^= 1; }
static void corrupt_table_crc(image_t *image) { image->bytes[128] ^= 1; }
static void no_mutation(image_t *image) { (void)image; }
static void overflow_deck_offset(image_t *image)
{
    put_u32(image->bytes + 32, UINT32_MAX - 31u);
    repair_header_crc(image);
}

static void test_open_and_metadata(const image_t *fixture)
{
    uint8_t compressed[65536];
    uint8_t raw[98304];
    kanji_catalog_t catalog;
    CHECK(open_catalog((image_t *)fixture, (uint32_t)fixture->length, &catalog,
                       compressed, sizeof compressed, raw, sizeof raw));
    CHECK_EQ(kanji_catalog_status(&catalog), KANJI_CATALOG_OK);
    CHECK_EQ(kanji_catalog_deck_count(&catalog), 2u);
    CHECK_EQ(kanji_catalog_card_count(&catalog), 5u);
    CHECK_EQ(kanji_catalog_block_count(&catalog), 1u);
    const uint8_t *catalog_id = kanji_catalog_id(&catalog);
    CHECK(catalog_id != NULL);
    CHECK(memcmp(catalog_id, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) != 0);

    kanji_catalog_deck_info_t deck;
    CHECK(kanji_catalog_deck(&catalog, 0, &deck));
    CHECK_STREQ(deck.level, "N2");
    CHECK_STREQ(deck.type, "kanji");
    CHECK_STREQ(deck.name, "JLPT N2 Kanji");
    CHECK_EQ(deck.card_count, 3u);
    CHECK(kanji_catalog_deck(&catalog, 1, &deck));
    CHECK_STREQ(deck.level, "N5");
    CHECK_STREQ(deck.type, "vocab");
    CHECK_STREQ(deck.name, "JLPT N5 Vocabulary");
    CHECK_EQ(deck.card_count, 2u);
    CHECK(!kanji_catalog_deck(&catalog, 2, &deck));
    CHECK_EQ(kanji_catalog_status(&catalog), KANJI_CATALOG_OUT_OF_RANGE);
}

static void test_cards_and_boundary(const image_t *fixture, const image_t *boundary)
{
    uint8_t compressed[65536];
    uint8_t raw[98304];
    kanji_catalog_t catalog;
    kanji_t card;
    kanji_t workspace;
    CHECK(open_catalog((image_t *)fixture, (uint32_t)fixture->length, &catalog,
                       compressed, sizeof compressed, raw, sizeof raw));
    CHECK(kanji_catalog_read_card(&catalog, 0, &card, &workspace));
    CHECK_STREQ(card.card.id, "punish");
    CHECK_STREQ(card.card.front, "懲らしめる");
    CHECK_STREQ(card.session.deck, "JLPT N2 Kanji");
    CHECK_STREQ(card.session.level, "N2");
    CHECK_EQ(card.source, KANJI_SOURCE_CATALOG);
    CHECK(kanji_catalog_read_card(&catalog, 4, &card, &workspace));
    CHECK_STREQ(card.card.id, "wealth");
    CHECK_STREQ(card.card.front, "財");
    CHECK_STREQ(card.card.gloss, "재물 재");
    CHECK_STREQ(card.card.hook_title, "형성");
    CHECK_STREQ(card.card.composition, "貝 + 才 = 財");
    CHECK_EQ(card.card.part_count, 3);
    CHECK_STREQ(card.card.parts[1].glyph, "貝");
    CHECK_STREQ(card.card.parts[2].glyph, "才");
    CHECK_EQ(card.source, KANJI_SOURCE_CATALOG);

    CHECK(open_catalog((image_t *)boundary, (uint32_t)boundary->length, &catalog,
                       compressed, sizeof compressed, raw, sizeof raw));
    static const struct { uint32_t ordinal; const char *id; } cases[] = {
        {0, "boundary-059"}, {62, "boundary-000"}, {63, "boundary-063"},
        {64, "boundary-012"}, {65, "boundary-065"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        CHECK(kanji_catalog_read_card(&catalog, cases[i].ordinal, &card,
                                      &workspace));
        CHECK_STREQ(card.card.id, cases[i].id);
        CHECK_EQ(card.source, KANJI_SOURCE_CATALOG);
    }
}

static void expect_failed_read(image_t *image, uint32_t ordinal,
                               kanji_catalog_status_t expected)
{
    uint8_t compressed[65536];
    uint8_t raw[98304];
    kanji_catalog_t catalog;
    CHECK(open_catalog(image, (uint32_t)image->length, &catalog,
                       compressed, sizeof compressed, raw, sizeof raw));
    kanji_t out;
    kanji_t workspace;
    kanji_t sentinel;
    memset(&out, 0xA5, sizeof out);
    sentinel = out;
    CHECK(!kanji_catalog_read_card(&catalog, ordinal, &out, &workspace));
    CHECK_EQ(kanji_catalog_status(&catalog), expected);
    CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
}

static void test_read_rejects_missing_or_aliased_workspace(const image_t *fixture)
{
    uint8_t compressed[65536];
    uint8_t raw[98304];
    kanji_catalog_t catalog;
    CHECK(open_catalog((image_t *)fixture, (uint32_t)fixture->length, &catalog,
                       compressed, sizeof compressed, raw, sizeof raw));

    kanji_t out;
    memset(&out, 0xA5, sizeof out);
    kanji_t sentinel = out;
    CHECK(!kanji_catalog_read_card(&catalog, 0, &out, NULL));
    CHECK_EQ(kanji_catalog_status(&catalog), KANJI_CATALOG_BAD_ARGUMENT);
    CHECK(memcmp(&out, &sentinel, sizeof out) == 0);

    CHECK(!kanji_catalog_read_card(&catalog, 0, &out, &out));
    CHECK_EQ(kanji_catalog_status(&catalog), KANJI_CATALOG_BAD_ARGUMENT);
    CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
}

static void test_failed_block_load_invalidates_cached_workspace(
    const image_t *boundary)
{
    uint8_t compressed[65536];
    uint8_t raw[98304];
    kanji_catalog_t catalog;
    kanji_catalog_io_t io = io_for((image_t *)boundary);
    io.inflate = inflate_exact_with_recoverable_failure;
    CHECK(kanji_catalog_open(&catalog, &io, (uint32_t)boundary->length,
                             compressed, sizeof compressed, raw, sizeof raw));

    kanji_t out;
    kanji_t workspace;
    CHECK(kanji_catalog_read_card(&catalog, 0, &out, &workspace));
    CHECK_STREQ(out.card.id, "boundary-059");

    memset(&out, 0xA5, sizeof out);
    kanji_t sentinel = out;
    fail_next_inflate_after_write = true;
    CHECK(!kanji_catalog_read_card(&catalog, 64, &out, &workspace));
    CHECK_EQ(kanji_catalog_status(&catalog), KANJI_CATALOG_INFLATE);
    CHECK(memcmp(&out, &sentinel, sizeof out) == 0);

    CHECK(kanji_catalog_read_card(&catalog, 0, &out, &workspace));
    CHECK_STREQ(out.card.id, "boundary-059");
}

/* The CRC rejection is the same defect wearing a different coat, and it is the
 * nastier of the two: here the inflate succeeded, so the workspace holds a full
 * block's worth of plausible-looking records rather than a short prefix. If the
 * cache still named the previous block afterwards, the next read of that block
 * would parse those records at the old block's offsets and hand back a card
 * that is structurally valid and simply belongs to somebody else. */
static void test_failed_block_crc_invalidates_cached_workspace(
    const image_t *boundary)
{
    uint8_t compressed[65536];
    uint8_t raw[98304];
    kanji_catalog_t catalog;
    kanji_catalog_io_t io = io_for((image_t *)boundary);
    io.crc32 = catalog_crc32_with_recoverable_failure;
    CHECK(kanji_catalog_open(&catalog, &io, (uint32_t)boundary->length,
                             compressed, sizeof compressed, raw, sizeof raw));

    kanji_t out;
    kanji_t workspace;
    CHECK(kanji_catalog_read_card(&catalog, 0, &out, &workspace));
    CHECK_STREQ(out.card.id, "boundary-059");

    memset(&out, 0xA5, sizeof out);
    kanji_t sentinel = out;
    fail_next_raw_crc = true;
    CHECK(!kanji_catalog_read_card(&catalog, 64, &out, &workspace));
    CHECK_EQ(kanji_catalog_status(&catalog), KANJI_CATALOG_RAW_CRC);
    CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
    /* If this is still armed the reader never reached the raw CRC, and the
     * rest of this test would pass for the wrong reason. */
    CHECK(!fail_next_raw_crc);

    CHECK(kanji_catalog_read_card(&catalog, 0, &out, &workspace));
    CHECK_STREQ(out.card.id, "boundary-059");
}

static void test_open_corruptions(const image_t *fixture)
{
    expect_open_status(fixture, corrupt_magic, 0, KANJI_CATALOG_BAD_MAGIC);
    expect_open_status(fixture, corrupt_version, 0, KANJI_CATALOG_UNSUPPORTED_VERSION);
    expect_open_status(fixture, corrupt_header_crc, 0, KANJI_CATALOG_HEADER_CRC);
    expect_open_status(fixture, corrupt_table_crc, 0, KANJI_CATALOG_TABLE_CRC);
    expect_open_status(fixture, no_mutation, (uint32_t)fixture->length - 1,
                       KANJI_CATALOG_BOUNDS);
    expect_open_status(fixture, overflow_deck_offset, 0, KANJI_CATALOG_BOUNDS);
}

static void test_repaired_structural_corruptions(const image_t *fixture,
                                                 const image_t *boundary)
{
    uint32_t card_index_off = get_u32(fixture->bytes + 40);

    /* The new deck index is still in range; only an exact membership tally can
     * detect that deck 0 lost a card and deck 1 gained one. */
    image_t damaged = clone_image(fixture);
    damaged.bytes[card_index_off + 8] = 1;
    repair_table_crc(&damaged);
    expect_image_open_status(&damaged, KANJI_CATALOG_FORMAT);
    free(damaged.bytes);

    /* Both records remain individually in bounds. The second now aliases the
     * beginning of the first instead of continuing at its end. */
    damaged = clone_image(fixture);
    put_u32(damaged.bytes + card_index_off + 12, 0);
    repair_table_crc(&damaged);
    expect_image_open_status(&damaged, KANJI_CATALOG_RECORD_OFFSET);
    free(damaged.bytes);

    /* Move the second record forward by one byte and shorten it by one. Its
     * end remains unchanged and every span stays in bounds, but byte `start`
     * is now unclaimed rather than being the next contiguous record byte. */
    uint32_t second_record = card_index_off + 12;
    uint32_t second_record_start = get_u32(fixture->bytes + second_record);
    uint32_t second_record_length = get_u32(fixture->bytes + second_record + 4);
    CHECK(second_record_length > 1);
    damaged = clone_image(fixture);
    put_u32(damaged.bytes + second_record, second_record_start + 1);
    put_u32(damaged.bytes + second_record + 4, second_record_length - 1);
    repair_table_crc(&damaged);
    expect_image_open_status(&damaged, KANJI_CATALOG_RECORD_OFFSET);
    free(damaged.bytes);

    uint32_t boundary_block_index = get_u32(boundary->bytes + 48);
    uint32_t second_entry = boundary_block_index + 16;
    uint32_t second_offset = get_u32(boundary->bytes + second_entry);
    uint32_t second_length = get_u32(boundary->bytes + second_entry + 4);

    /* Shift the second compressed span left and grow it by one. The span stays
     * in range and still ends at used_size, but overlaps the first block. */
    damaged = clone_image(boundary);
    put_u32(damaged.bytes + second_entry, second_offset - 1);
    put_u32(damaged.bytes + second_entry + 4, second_length + 1);
    repair_table_crc(&damaged);
    expect_image_open_status(&damaged, KANJI_CATALOG_BLOCK_BOUNDS);
    free(damaged.bytes);

    /* Shift the second span right and shrink it by one. Every span is in range
     * and the data section still ends exactly, but one byte is unclaimed. */
    damaged = clone_image(boundary);
    put_u32(damaged.bytes + second_entry, second_offset + 1);
    put_u32(damaged.bytes + second_entry + 4, second_length - 1);
    repair_table_crc(&damaged);
    expect_image_open_status(&damaged, KANJI_CATALOG_BLOCK_BOUNDS);
    free(damaged.bytes);

    /* Header and deck totals still say five cards globally, but the deck table
     * distribution (2 + 3) disagrees with unchanged index membership (3 + 2). */
    damaged = clone_image(fixture);
    put_u32(damaged.bytes + 128 + 56, 2);
    put_u32(damaged.bytes + 128 + 64 + 56, 3);
    repair_table_crc(&damaged);
    expect_image_open_status(&damaged, KANJI_CATALOG_FORMAT);
    free(damaged.bytes);
}

static image_t replace_record_byte(const image_t *base, uint32_t ordinal,
                                   size_t byte_in_record, uint8_t replacement)
{
    image_t image = clone_image(base);
    uint32_t card_index_off = get_u32(image.bytes + 40);
    uint32_t block_index_off = get_u32(image.bytes + 48);
    uint32_t data_off = get_u32(image.bytes + 56);
    uint32_t entry = card_index_off + ordinal * 12u;
    uint32_t record_off = get_u32(image.bytes + entry);
    uint32_t record_len = get_u32(image.bytes + entry + 4);
    uint32_t block_entry = block_index_off + (ordinal / 64u) * 16u;
    uint32_t compressed_off = get_u32(image.bytes + block_entry);
    uint32_t compressed_len = get_u32(image.bytes + block_entry + 4);
    uint32_t raw_len = get_u32(image.bytes + block_entry + 8);
    CHECK(byte_in_record < record_len);
    CHECK_EQ(compressed_off, data_off);
    uint8_t *raw = malloc(raw_len);
    uLongf actual_raw = raw_len;
    CHECK(raw != NULL);
    CHECK_EQ(uncompress(raw, &actual_raw, image.bytes + compressed_off, compressed_len), Z_OK);
    CHECK_EQ(actual_raw, raw_len);
    raw[record_off + byte_in_record] = replacement;

    uLongf capacity = compressBound(raw_len);
    uint8_t *compressed = malloc(capacity);
    CHECK(compressed != NULL);
    CHECK_EQ(compress2(compressed, &capacity, raw, raw_len, 9), Z_OK);
    size_t new_length = data_off + (size_t)capacity;
    uint8_t *resized = realloc(image.bytes, new_length);
    CHECK(resized != NULL);
    if (resized) image.bytes = resized;
    image.length = new_length;
    memcpy(image.bytes + data_off, compressed, capacity);
    put_u32(image.bytes + block_entry + 4, (uint32_t)capacity);
    put_u32(image.bytes + block_entry + 12, catalog_crc32(raw, raw_len));
    put_u32(image.bytes + 16, (uint32_t)new_length);
    put_u32(image.bytes + 60, (uint32_t)(new_length - data_off));
    repair_table_crc(&image);
    repair_header_crc(&image);
    free(compressed);
    free(raw);
    return image;
}

static void test_read_corruptions(const image_t *fixture)
{
    uint32_t card_index_off = get_u32(fixture->bytes + 40);
    uint32_t block_index_off = get_u32(fixture->bytes + 48);
    uint32_t data_off = get_u32(fixture->bytes + 56);
    uint32_t raw_len = get_u32(fixture->bytes + block_index_off + 8);

    image_t damaged = clone_image(fixture);
    damaged.bytes[card_index_off + 8] = 2;
    repair_table_crc(&damaged);
    expect_image_open_status(&damaged, KANJI_CATALOG_DECK_INDEX);
    free(damaged.bytes);

    damaged = clone_image(fixture);
    put_u32(damaged.bytes + block_index_off, UINT32_MAX - 7u);
    repair_table_crc(&damaged);
    expect_image_open_status(&damaged, KANJI_CATALOG_BLOCK_BOUNDS);
    free(damaged.bytes);

    damaged = clone_image(fixture);
    damaged.bytes[data_off] ^= 0x80;
    expect_failed_read(&damaged, 0, KANJI_CATALOG_INFLATE);
    free(damaged.bytes);

    damaged = clone_image(fixture);
    damaged.bytes[block_index_off + 12] ^= 1;
    repair_table_crc(&damaged);
    expect_failed_read(&damaged, 0, KANJI_CATALOG_RAW_CRC);
    free(damaged.bytes);

    damaged = clone_image(fixture);
    put_u32(damaged.bytes + card_index_off, raw_len);
    repair_table_crc(&damaged);
    expect_image_open_status(&damaged, KANJI_CATALOG_RECORD_OFFSET);
    free(damaged.bytes);

    damaged = clone_image(fixture);
    put_u32(damaged.bytes + card_index_off + 4, raw_len + 1u);
    repair_table_crc(&damaged);
    expect_image_open_status(&damaged, KANJI_CATALOG_RECORD_LENGTH);
    free(damaged.bytes);

    damaged = replace_record_byte(fixture, 0, 0, '!');
    expect_failed_read(&damaged, 0, KANJI_CATALOG_PARSE);
    free(damaged.bytes);

    uint32_t record_len = get_u32(fixture->bytes + card_index_off + 4);
    uint32_t compressed_len = get_u32(fixture->bytes + block_index_off + 4);
    uint8_t *raw = malloc(raw_len);
    uLongf actual = raw_len;
    CHECK_EQ(uncompress(raw, &actual, fixture->bytes + data_off, compressed_len), Z_OK);
    size_t utf8_at = 0;
    while (utf8_at < record_len && raw[utf8_at] < 0x80) utf8_at++;
    CHECK(utf8_at < record_len);
    free(raw);
    damaged = replace_record_byte(fixture, 0, utf8_at, 0xff);
    expect_failed_read(&damaged, 0, KANJI_CATALOG_PARSE);
    free(damaged.bytes);

    damaged = clone_image(fixture);
    expect_failed_read(&damaged, 5, KANJI_CATALOG_OUT_OF_RANGE);
    free(damaged.bytes);
}

int main(void)
{
    image_t fixture = load_image(CATALOG_FIXTURE);
    image_t boundary = load_image(CATALOG_BOUNDARY_FIXTURE);
    if (fixture.bytes && boundary.bytes) {
        test_open_and_metadata(&fixture);
        test_cards_and_boundary(&fixture, &boundary);
        test_read_rejects_missing_or_aliased_workspace(&fixture);
        test_failed_block_load_invalidates_cached_workspace(&boundary);
        test_failed_block_crc_invalidates_cached_workspace(&boundary);
        test_open_corruptions(&fixture);
        test_repaired_structural_corruptions(&fixture, &boundary);
        test_read_corruptions(&fixture);
    }
    free(boundary.bytes);
    free(fixture.bytes);
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    puts("ok: catalog metadata, boundaries, corruption, and atomic output");
    return 0;
}
