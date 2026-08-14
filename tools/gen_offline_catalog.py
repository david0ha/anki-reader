#!/usr/bin/env python3
"""Build and independently verify the ESP32 offline study catalog image."""

import argparse
import dataclasses
import hashlib
import json
import os
import re
import sqlite3
import struct
import sys
import tempfile
import zlib

from kanji_server import project_card_content


MAGIC = b"KJCAT01\0"
SCHEMA = 1
HEADER_SIZE = 128
DECK_SIZE = 64
CARD_INDEX_SIZE = 12
BLOCK_INDEX_SIZE = 16
BLOCK_CARDS = 64
MAX_RAW_BLOCK = 96 * 1024
DEFAULT_PARTITION_SIZE = 0x770000

_HEADER = struct.Struct("<8sHHIIHHIIIIIIIIIIQ16s32s")
# One deck record is: level[4], type[8], display_name[44], card_count u32,
# reserved u32. All strings are UTF-8, NUL-terminated, and zero-padded.
_DECK = struct.Struct("<4s8s44sII")
_CARD_INDEX = struct.Struct("<IIB3x")
_BLOCK_INDEX = struct.Struct("<IIII")
_CARD_KEYS = (
    "id", "front", "reading", "on_reading", "kun_reading", "level",
    "gloss", "senses", "description", "hook_title", "hook_body",
    "composition", "parts",
)
_LEVEL = re.compile(r"N[1-5]")


class OrderedCards(list):
    """A normal card list that also carries the seed required by the header."""

    def __init__(self, values=(), *, seed=0):
        super().__init__(values)
        self.seed = seed


@dataclasses.dataclass(frozen=True)
class CatalogManifest:
    deck_count: int
    card_count: int
    block_count: int
    used_size: int
    max_raw_block: int
    max_compressed_block: int
    seed: int
    catalog_id: bytes
    source_sha256: bytes
    decks: tuple
    cards: tuple


def _canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def _u64(seed):
    if isinstance(seed, bool) or not isinstance(seed, int) or not 0 <= seed <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("seed must be an unsigned 64-bit integer")
    return seed


def _level(name, cards=()):
    match = _LEVEL.search(name if isinstance(name, str) else "")
    if match:
        return match.group(0)
    for card in cards:
        value = card.get("level")
        if isinstance(value, str) and _LEVEL.fullmatch(value):
            return value
    return ""


def select_user(conn, explicit_user_id=None):
    """Choose the active user with the greatest card/deck coverage."""
    rows = conn.execute("""
        SELECT sd.user_id,
               COUNT(DISTINCT CASE
                   WHEN sc.deleted_at IS NULL AND sc.suspended = 0
                        AND sc.template_card_id IS NOT NULL THEN sc.id END) AS active_cards,
               COUNT(DISTINCT sd.id) AS active_decks
          FROM study_decks AS sd
          LEFT JOIN study_cards AS sc
            ON sc.study_deck_id = sd.id AND sc.user_id = sd.user_id
         WHERE sd.archived_at IS NULL
         GROUP BY sd.user_id
         ORDER BY active_cards DESC, active_decks DESC, sd.user_id ASC
    """).fetchall()
    if explicit_user_id is not None:
        if not isinstance(explicit_user_id, str) or not explicit_user_id:
            raise ValueError("user id must be a non-empty string")
        if not any(row[0] == explicit_user_id for row in rows):
            raise ValueError(f"unknown user or no active decks: {explicit_user_id}")
        return explicit_user_id
    if not rows:
        raise ValueError("database has no user with an active study deck")
    return rows[0][0]


def load_source(conn, user_id):
    """Load shared templates reachable through one user's active decks."""
    rows = conn.execute("""
        SELECT dt.id, dt.name, dt.deck_type, dt.sort_order,
               ct.id, ct.front, ct.back, ct.hint, ct.tags_json, ct.sort_order
          FROM study_decks AS sd
          JOIN deck_templates AS dt ON dt.id = sd.template_deck_id
          JOIN card_templates AS ct ON ct.template_deck_id = dt.id
         WHERE sd.user_id = ? AND sd.archived_at IS NULL
         ORDER BY dt.sort_order ASC, dt.id ASC, ct.sort_order ASC, ct.id ASC
    """, (user_id,)).fetchall()
    if not rows:
        raise ValueError(f"user has no template cards in active decks: {user_id}")

    decks = []
    by_id = {}
    cards = []
    for deck_id, name, deck_type, _deck_order, card_id, front, back, hint, tags_json, _card_order in rows:
        deck = by_id.get(deck_id)
        if deck is None:
            deck = {"id": deck_id, "name": name, "level": _level(name),
                    "deck_type": deck_type, "cards": []}
            by_id[deck_id] = deck
            decks.append(deck)
        try:
            tags = json.loads(tags_json) if isinstance(tags_json, str) else []
        except ValueError:
            tags = []
        source = {"id": card_id, "front": front, "back": back, "hint": hint,
                  "tags": tags if isinstance(tags, list) else []}
        projected = project_card_content(source)
        deck["cards"].append(projected)
        cards.append(projected)
    for deck in decks:
        if not deck["level"]:
            deck["level"] = _level(deck["name"], deck["cards"])
    return decks, cards


def load_fixture(path):
    """Load a source-shaped JSON fixture and apply the production projection."""
    with open(path, encoding="utf-8") as source:
        document = json.load(source)
    raw_decks = document.get("decks") if isinstance(document, dict) else None
    if not isinstance(raw_decks, list) or not raw_decks:
        raise ValueError("fixture must contain a non-empty decks array")
    decks = []
    cards = []
    for raw_deck in raw_decks:
        if not isinstance(raw_deck, dict) or not isinstance(raw_deck.get("cards"), list):
            raise ValueError("each fixture deck must contain a cards array")
        deck = {
            "id": raw_deck.get("id"),
            "name": raw_deck.get("name"),
            "level": raw_deck.get("level") or _level(raw_deck.get("name")),
            "deck_type": raw_deck.get("deck_type"),
            "cards": [],
        }
        for raw_card in raw_deck["cards"]:
            projected = project_card_content(raw_card)
            deck["cards"].append(projected)
            cards.append(projected)
        decks.append(deck)
    return decks, cards


def balanced_cards(decks, seed):
    """Seed-sort within each deck, then consume non-empty decks round-robin."""
    seed = _u64(seed)
    seed_bytes = seed.to_bytes(8, "little")
    queues = []
    for deck_index, deck in enumerate(decks):
        deck_id = deck.get("id")
        if not isinstance(deck_id, str) or not deck_id:
            raise ValueError(f"deck {deck_index} has no stable id")
        cards = deck.get("cards")
        if not isinstance(cards, list):
            raise ValueError(f"deck {deck_id} has no card list")

        def key(card):
            card_id = card.get("id") if isinstance(card, dict) else None
            if not isinstance(card_id, str) or not card_id:
                raise ValueError(f"deck {deck_id} contains a card without a stable id")
            material = (seed_bytes + b"\0" + deck_id.encode("utf-8") + b"\0" +
                        card_id.encode("utf-8"))
            return hashlib.sha256(material).digest(), card_id.encode("utf-8")

        ordered = []
        for card in sorted(cards, key=key):
            item = dict(card)
            item["deck_index"] = deck_index
            ordered.append(item)
        queues.append(ordered)

    out = OrderedCards(seed=seed)
    position = 0
    while True:
        emitted = False
        for queue in queues:
            if position < len(queue):
                out.append(queue[position])
                emitted = True
        if not emitted:
            return out
        position += 1


def _cstring(value, width, field):
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty string")
    encoded = value.encode("utf-8")
    if len(encoded) >= width:
        raise ValueError(f"{field} exceeds {width - 1} UTF-8 bytes")
    return encoded + bytes(width - len(encoded))


def _deck_table(decks, cards):
    counts = [0] * len(decks)
    for card in cards:
        deck_index = card.get("deck_index")
        if isinstance(deck_index, bool) or not isinstance(deck_index, int) or not 0 <= deck_index < len(decks):
            raise ValueError("card has an invalid deck_index")
        counts[deck_index] += 1
    table = bytearray()
    metadata = []
    for index, deck in enumerate(decks):
        name = deck.get("name")
        level = deck.get("level") or _level(name, deck.get("cards", ()))
        deck_type = deck.get("deck_type")
        if level not in {"N1", "N2", "N3", "N4", "N5"}:
            raise ValueError(f"deck {index} has an invalid JLPT level")
        if deck_type not in {"kanji", "vocab", "kana"}:
            raise ValueError(f"deck {index} has an invalid type")
        if not isinstance(name, str) or len(name.encode("utf-8")) >= 40:
            raise ValueError(f"deck {index} name exceeds 39 UTF-8 bytes")
        table.extend(_DECK.pack(_cstring(level, 4, "deck level"),
                                _cstring(deck_type, 8, "deck type"),
                                _cstring(name, 44, "deck name"), counts[index], 0))
        metadata.append({"level": level, "type": deck_type, "name": name,
                         "card_count": counts[index]})
    return bytes(table), metadata


def _clean_card(card, deck_level):
    clean = {}
    for key in _CARD_KEYS:
        value = card.get(key)
        if key == "level" and not value:
            value = deck_level
        clean[key] = value
    if not isinstance(clean["id"], str) or not clean["id"]:
        raise ValueError("card id must be a non-empty string")
    for key in ("front", "reading", "on_reading", "kun_reading", "level", "gloss",
                "description", "hook_title", "hook_body", "composition"):
        if not isinstance(clean[key], str):
            raise ValueError(f"card {clean['id']} field {key} must be a string")
    if not isinstance(clean["senses"], list) or not all(isinstance(v, str) for v in clean["senses"]):
        raise ValueError(f"card {clean['id']} senses must be strings")
    if not isinstance(clean["parts"], list):
        raise ValueError(f"card {clean['id']} parts must be an array")
    byte_limits = {
        "id": 40, "front": 40, "reading": 144, "on_reading": 144,
        "kun_reading": 144, "level": 24, "gloss": 144,
        "description": 832, "hook_title": 24, "hook_body": 832,
        "composition": 96,
    }
    for key, capacity in byte_limits.items():
        if len(clean[key].encode("utf-8")) >= capacity:
            raise ValueError(
                f"card {clean['id']} field {key} exceeds {capacity - 1} UTF-8 bytes")
    if len(clean["senses"]) > 5:
        raise ValueError(f"card {clean['id']} exceeds five senses")
    for sense in clean["senses"]:
        if len(sense.encode("utf-8")) >= 144:
            raise ValueError(f"card {clean['id']} has a sense above 143 UTF-8 bytes")
    if len(clean["parts"]) > 6:
        raise ValueError(f"card {clean['id']} exceeds six components")
    for part in clean["parts"]:
        if (not isinstance(part, dict) or set(part) != {"glyph", "meaning", "reading"}
                or not all(isinstance(part[name], str) for name in part)):
            raise ValueError(f"card {clean['id']} has an invalid component")
        for name, capacity in (("glyph", 40), ("meaning", 144), ("reading", 144)):
            if len(part[name].encode("utf-8")) >= capacity:
                raise ValueError(
                    f"card {clean['id']} component {name} exceeds "
                    f"{capacity - 1} UTF-8 bytes")
    return clean


def _source_digest(deck_table, envelopes, deck_indexes):
    membership = sorted(zip(deck_indexes, envelopes),
                        key=lambda item: (item[0], json.loads(item[1])["card"]["id"]))
    digest = hashlib.sha256()
    digest.update(deck_table)
    for deck_index, envelope in membership:
        digest.update(bytes((deck_index,)))
        digest.update(struct.pack("<I", len(envelope)))
        digest.update(envelope)
    return digest.digest()


def _catalog_id(source_sha256, seed):
    return hashlib.sha256(MAGIC + struct.pack("<H", SCHEMA) + source_sha256 +
                          struct.pack("<Q", seed)).digest()[:16]


def encode_catalog(decks, cards, partition_size):
    """Encode deck metadata, card indexes, block indexes, and zlib data."""
    if isinstance(partition_size, bool) or not isinstance(partition_size, int) or partition_size < HEADER_SIZE:
        raise ValueError("partition size is too small")
    if len(decks) > 255:
        raise ValueError("deck count exceeds the one-byte card deck index")
    seed = _u64(getattr(cards, "seed", 0))
    deck_table, deck_metadata = _deck_table(decks, cards)
    envelopes = []
    deck_indexes = []
    seen = set()
    for card in cards:
        deck_index = card["deck_index"]
        clean = _clean_card(card, deck_metadata[deck_index]["level"])
        if clean["id"] in seen:
            raise ValueError(f"duplicate card id: {clean['id']}")
        seen.add(clean["id"])
        envelopes.append(_canonical({"v": 1, "card": clean}))
        deck_indexes.append(deck_index)

    card_index = bytearray()
    compressed_blocks = []
    raw_lengths = []
    raw_crcs = []
    for block_start in range(0, len(envelopes), BLOCK_CARDS):
        raw = bytearray()
        for ordinal in range(block_start, min(block_start + BLOCK_CARDS, len(envelopes))):
            record = envelopes[ordinal]
            card_index.extend(_CARD_INDEX.pack(len(raw), len(record), deck_indexes[ordinal]))
            raw.extend(record)
        if len(raw) > MAX_RAW_BLOCK:
            raise ValueError(f"raw block exceeds {MAX_RAW_BLOCK} bytes")
        compressed_blocks.append(zlib.compress(bytes(raw), 9))
        raw_lengths.append(len(raw))
        raw_crcs.append(zlib.crc32(raw) & 0xFFFFFFFF)

    block_count = len(compressed_blocks)
    deck_off = HEADER_SIZE
    card_index_off = deck_off + len(deck_table)
    block_index_off = card_index_off + len(card_index)
    data_off = block_index_off + block_count * BLOCK_INDEX_SIZE
    block_index = bytearray()
    cursor = data_off
    for compressed, raw_len, raw_crc in zip(compressed_blocks, raw_lengths, raw_crcs):
        block_index.extend(_BLOCK_INDEX.pack(cursor, len(compressed), raw_len, raw_crc))
        cursor += len(compressed)
    data = b"".join(compressed_blocks)
    used_size = data_off + len(data)
    if used_size > partition_size:
        raise ValueError(f"catalog image {used_size} exceeds partition {partition_size}")

    source_sha256 = _source_digest(deck_table, envelopes, deck_indexes)
    catalog_id = _catalog_id(source_sha256, seed)
    header_prefix = _HEADER.pack(
        MAGIC, SCHEMA, HEADER_SIZE, 0, used_size, len(decks), BLOCK_CARDS,
        len(envelopes), block_count, deck_off, len(deck_table),
        card_index_off, len(card_index), block_index_off, len(block_index),
        data_off, len(data), seed, catalog_id, source_sha256,
    )
    tables = deck_table + bytes(card_index) + bytes(block_index)
    header = (header_prefix + struct.pack("<I", zlib.crc32(header_prefix) & 0xFFFFFFFF) +
              struct.pack("<I", zlib.crc32(tables) & 0xFFFFFFFF))
    image = header + tables + data
    if len(image) != used_size:
        raise AssertionError("internal image size disagreement")
    return image


def _decode_cstring(raw, field):
    try:
        end = raw.index(0)
    except ValueError:
        raise ValueError(f"{field} is not NUL terminated") from None
    if any(raw[end + 1:]):
        raise ValueError(f"{field} has nonzero padding")
    try:
        value = raw[:end].decode("utf-8")
    except UnicodeDecodeError:
        raise ValueError(f"{field} is not UTF-8") from None
    if not value:
        raise ValueError(f"{field} is empty")
    return value


def _inflate_exact(compressed, expected_length):
    inflater = zlib.decompressobj()
    try:
        raw = inflater.decompress(compressed, expected_length + 1)
    except zlib.error as exc:
        raise ValueError(f"invalid zlib block: {exc}") from None
    if (not inflater.eof or inflater.unused_data or inflater.unconsumed_tail or
            len(raw) != expected_length):
        raise ValueError("zlib block length or framing mismatch")
    return raw


def verify_catalog(image):
    """Parse the image from bytes and independently validate every boundary."""
    if not isinstance(image, (bytes, bytearray, memoryview)):
        raise ValueError("catalog image must be bytes")
    image = bytes(image)
    if len(image) < 128:
        raise ValueError("catalog header is truncated")
    if image[0:8] != b"KJCAT01\0":
        raise ValueError("catalog magic mismatch")
    if struct.unpack_from("<H", image, 8)[0] != 1:
        raise ValueError("unsupported catalog schema")
    if struct.unpack_from("<H", image, 10)[0] != 128:
        raise ValueError("catalog header size mismatch")
    if zlib.crc32(image[0:120]) & 0xFFFFFFFF != struct.unpack_from("<I", image, 120)[0]:
        raise ValueError("catalog header CRC mismatch")

    fields = _HEADER.unpack_from(image, 0)
    (_magic, _schema, _header_size, flags, used_size, deck_count, block_cards,
     card_count, block_count, deck_off, deck_len, card_index_off,
     card_index_len, block_index_off, block_index_len, data_off, data_len,
     seed, catalog_id, source_sha256) = fields
    if flags != 0 or block_cards != 64:
        raise ValueError("unsupported catalog flags or block cardinality")
    if block_count != (card_count + 63) // 64:
        raise ValueError("catalog block count mismatch")
    expected = (
        deck_off == 128 and deck_len == deck_count * 64 and
        card_index_off == deck_off + deck_len and card_index_len == card_count * 12 and
        block_index_off == card_index_off + card_index_len and
        block_index_len == block_count * 16 and
        data_off == block_index_off + block_index_len and
        data_len == used_size - data_off
    )
    if not expected or used_size > len(image) or used_size < data_off:
        raise ValueError("catalog section bounds mismatch")
    tables = image[deck_off:data_off]
    if zlib.crc32(tables) & 0xFFFFFFFF != struct.unpack_from("<I", image, 124)[0]:
        raise ValueError("catalog tables CRC mismatch")

    decks = []
    for index in range(deck_count):
        level_raw, type_raw, name_raw, count, reserved = _DECK.unpack_from(
            image, deck_off + index * 64)
        level = _decode_cstring(level_raw, "deck level")
        deck_type = _decode_cstring(type_raw, "deck type")
        name = _decode_cstring(name_raw, "deck name")
        if level not in {"N1", "N2", "N3", "N4", "N5"}:
            raise ValueError("deck level is invalid")
        if deck_type not in {"kanji", "vocab", "kana"} or reserved != 0:
            raise ValueError("deck type or reserved field is invalid")
        decks.append({"level": level, "type": deck_type, "name": name,
                      "card_count": count})
    if sum(deck["card_count"] for deck in decks) != card_count:
        raise ValueError("deck card counts do not sum to header card count")

    blocks = []
    raw_blocks = []
    cursor = data_off
    max_raw = 0
    max_compressed = 0
    for block_id in range(block_count):
        offset, compressed_len, raw_len, raw_crc = _BLOCK_INDEX.unpack_from(
            image, block_index_off + block_id * 16)
        if (offset != cursor or compressed_len == 0 or raw_len == 0 or
                raw_len > 96 * 1024 or compressed_len > used_size - offset):
            raise ValueError("compressed block bounds are invalid")
        compressed = image[offset:offset + compressed_len]
        raw = _inflate_exact(compressed, raw_len)
        if zlib.crc32(raw) & 0xFFFFFFFF != raw_crc:
            raise ValueError("raw block CRC mismatch")
        blocks.append((offset, compressed_len, raw_len, raw_crc))
        raw_blocks.append(raw)
        cursor += compressed_len
        max_raw = max(max_raw, raw_len)
        max_compressed = max(max_compressed, compressed_len)
    if cursor != used_size:
        raise ValueError("compressed blocks do not fill the data section")

    cards = []
    deck_counts = [0] * deck_count
    block_ends = [0] * block_count
    ids = set()
    envelopes = []
    deck_indexes = []
    for ordinal in range(card_count):
        entry_off = card_index_off + ordinal * 12
        record_off, record_len, deck_index = _CARD_INDEX.unpack_from(image, entry_off)
        if any(image[entry_off + 9:entry_off + 12]):
            raise ValueError("card index padding is nonzero")
        block_id = ordinal // 64
        raw = raw_blocks[block_id]
        if (deck_index >= deck_count or record_len == 0 or
                record_off != block_ends[block_id] or record_len > len(raw) - record_off):
            raise ValueError("card index bounds are invalid")
        record = raw[record_off:record_off + record_len]
        block_ends[block_id] += record_len
        try:
            envelope = json.loads(record.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            raise ValueError("card record is not valid UTF-8 JSON") from None
        if (not isinstance(envelope, dict) or set(envelope) != {"v", "card"}
                or envelope.get("v") != 1 or not isinstance(envelope.get("card"), dict)
                or tuple(sorted(envelope["card"])) != tuple(sorted(_CARD_KEYS))):
            raise ValueError("card envelope schema mismatch")
        clean = _clean_card(envelope["card"], decks[deck_index]["level"])
        if clean != envelope["card"] or clean["level"] != decks[deck_index]["level"]:
            raise ValueError("card content or level mismatch")
        if clean["id"] in ids:
            raise ValueError("duplicate card id")
        ids.add(clean["id"])
        deck_counts[deck_index] += 1
        cards.append(envelope)
        envelopes.append(record)
        deck_indexes.append(deck_index)
    if any(block_ends[index] != len(raw_blocks[index]) for index in range(block_count)):
        raise ValueError("card indexes do not consume their raw block")
    if deck_counts != [deck["card_count"] for deck in decks]:
        raise ValueError("card indexes disagree with deck counts")
    if _source_digest(image[deck_off:card_index_off], envelopes, deck_indexes) != source_sha256:
        raise ValueError("source SHA-256 mismatch")
    if _catalog_id(source_sha256, seed) != catalog_id:
        raise ValueError("catalog id mismatch")

    return CatalogManifest(
        deck_count=deck_count, card_count=card_count, block_count=block_count,
        used_size=used_size, max_raw_block=max_raw,
        max_compressed_block=max_compressed, seed=seed, catalog_id=catalog_id,
        source_sha256=source_sha256, decks=tuple(decks), cards=tuple(cards),
    )


def _write_atomic(path, image):
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=os.path.basename(path) + ".",
                                     suffix=".tmp", dir=directory)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(image)
            output.flush()
            os.fsync(output.fileno())
        try:
            with open(path, "rb") as current:
                unchanged = current.read() == image
        except FileNotFoundError:
            unchanged = False
        if unchanged:
            os.unlink(temporary)
            return False
        os.replace(temporary, path)
        directory_fd = os.open(directory, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
        return True
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def _integer(text):
    try:
        return int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"invalid integer: {text}") from None


def _arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--db", help="read-only backend SQLite database")
    source.add_argument("--fixture-json", help="source-shaped test fixture")
    parser.add_argument("--user-id", help="explicit backend user id")
    parser.add_argument("--seed", type=_integer, default=0)
    parser.add_argument("--partition-size", type=_integer,
                        default=DEFAULT_PARTITION_SIZE)
    parser.add_argument("--output", required=True)
    parser.add_argument("--verify", action="store_true")
    return parser.parse_args(argv)


def main(argv=None):
    args = _arguments(argv)
    try:
        if args.fixture_json:
            decks, _cards = load_fixture(args.fixture_json)
        else:
            path = os.path.abspath(args.db)
            connection = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
            try:
                connection.execute("PRAGMA query_only=ON")
                connection.execute("BEGIN")
                user_id = select_user(connection, args.user_id)
                decks, cards = load_source(connection, user_id)
                if len(decks) != 10 or len({card["id"] for card in cards}) != 9956:
                    raise ValueError(
                        f"production catalog requires 10 decks/9956 unique cards; "
                        f"got {len(decks)} decks/{len({card['id'] for card in cards})} cards")
            finally:
                connection.close()
        ordered = balanced_cards(decks, args.seed)
        image = encode_catalog(decks, ordered, args.partition_size)
        manifest = verify_catalog(image)
        changed = _write_atomic(args.output, image)
        if args.verify:
            print(f"decks={manifest.deck_count} cards={manifest.card_count} "
                  f"blocks={manifest.block_count} bytes={manifest.used_size} "
                  f"max_raw_block={manifest.max_raw_block} "
                  f"output={'updated' if changed else 'unchanged'}")
        return 0
    except (OSError, sqlite3.Error, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
