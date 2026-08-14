#!/usr/bin/env python3
"""Behavior tests for the deterministic offline catalog generator."""

import copy
import json
import os
import sqlite3
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gen_offline_catalog import (  # noqa: E402
    balanced_cards,
    encode_catalog,
    load_fixture,
    load_source,
    select_user,
    verify_catalog,
)


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE = os.path.join(ROOT, "tools", "fixtures", "offline_catalog.json")
GENERATOR = os.path.join(ROOT, "tools", "gen_offline_catalog.py")


class Checks:
    def __init__(self):
        self.count = 0
        self.failures = 0

    def check(self, condition, message):
        self.count += 1
        if not condition:
            self.failures += 1
            print(f"FAIL: {message}", file=sys.stderr)

    def eq(self, actual, expected, message):
        self.check(actual == expected,
                   f"{message}: got {actual!r}, expected {expected!r}")

    def raises(self, error, operation, message):
        self.count += 1
        try:
            operation()
        except error:
            return
        except Exception as exc:  # pragma: no cover - diagnostic path
            self.failures += 1
            print(f"FAIL: {message}: raised {type(exc).__name__}: {exc}",
                  file=sys.stderr)
            return
        self.failures += 1
        print(f"FAIL: {message}: did not raise {error.__name__}", file=sys.stderr)


T = Checks()


def source_card(card_id, front, level="N5"):
    return {
        "id": card_id,
        "front": front,
        "back": json.dumps({
            "kanji": front,
            "meaning": {"gloss": f"gloss-{card_id}", "senses": [f"sense-{card_id}"]},
            "on_yomi": [{"reading": f"read-{card_id}"}],
            "kun_yomi": [],
            "shape_explanation": f"shape-{card_id}",
        }, ensure_ascii=False),
        "hint": json.dumps({
            "principle": "형성",
            "reason": f"reason-{card_id}",
            "shapes": [],
        }, ensure_ascii=False),
        "tags_json": json.dumps([level]),
    }


def create_wal_source(path):
    conn = sqlite3.connect(path)
    conn.executescript("""
        PRAGMA journal_mode=WAL;
        CREATE TABLE deck_templates (
            id TEXT PRIMARY KEY, name TEXT NOT NULL, description TEXT,
            deck_type TEXT NOT NULL, sort_order INTEGER NOT NULL
        );
        CREATE TABLE card_templates (
            id TEXT PRIMARY KEY, template_deck_id TEXT NOT NULL,
            front TEXT NOT NULL, back TEXT NOT NULL, hint TEXT,
            tags_json TEXT NOT NULL, sort_order INTEGER NOT NULL
        );
        CREATE TABLE study_decks (
            id TEXT PRIMARY KEY, user_id TEXT NOT NULL,
            template_deck_id TEXT NOT NULL, name TEXT NOT NULL,
            deck_type TEXT NOT NULL, archived_at TEXT
        );
        CREATE TABLE study_cards (
            id TEXT PRIMARY KEY, user_id TEXT NOT NULL,
            study_deck_id TEXT NOT NULL, template_card_id TEXT,
            front TEXT NOT NULL, back TEXT NOT NULL, hint TEXT,
            suspended INTEGER NOT NULL, deleted_at TEXT
        );
    """)
    conn.executemany("INSERT INTO deck_templates VALUES (?,?,?,?,?)", [
        ("deck-a", "JLPT N5 Kanji", "a", "kanji", 0),
        ("deck-b", "JLPT N4 Vocabulary", "b", "vocab", 1),
    ])
    cards = [source_card("a1", "一"), source_card("a2", "二"),
             source_card("a3", "三"), source_card("b1", "四", "N4")]
    conn.executemany(
        "INSERT INTO card_templates VALUES (?,?,?,?,?,?,?)",
        [(c["id"], "deck-a" if c["id"].startswith("a") else "deck-b",
          c["front"], c["back"], c["hint"], c["tags_json"], i)
         for i, c in enumerate(cards)],
    )
    conn.executemany("INSERT INTO study_decks VALUES (?,?,?,?,?,?)", [
        ("study-a", "alpha", "deck-a", "copied a", "kanji", None),
        ("study-b1", "beta", "deck-a", "copied a", "kanji", None),
        ("study-b2", "beta", "deck-b", "copied b", "vocab", None),
        ("study-old", "alpha", "deck-b", "archived", "vocab", "2026-01-01"),
    ])
    # Alpha wins on three active cards even though beta has more active decks.
    study_cards = [
        ("sa1", "alpha", "study-a", "a1", "CORRUPT", "{}", "{}", 0, None),
        ("sa2", "alpha", "study-a", "a2", "CORRUPT", "{}", "{}", 0, None),
        ("sa3", "alpha", "study-a", "a3", "CORRUPT", "{}", "{}", 0, None),
        ("sb1", "beta", "study-b1", "a1", "CORRUPT", "{}", "{}", 0, None),
        ("sb2", "beta", "study-b2", "b1", "CORRUPT", "{}", "{}", 0, None),
        ("suspended", "beta", "study-b2", "b1", "CORRUPT", "{}", "{}", 1, None),
        ("deleted", "beta", "study-b2", "b1", "CORRUPT", "{}", "{}", 0, "x"),
    ]
    conn.executemany("INSERT INTO study_cards VALUES (?,?,?,?,?,?,?,?,?)", study_cards)
    conn.commit()
    return conn


def test_source_selection():
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "source.sqlite3")
        writer = create_wal_source(path)
        reader = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        reader.execute("PRAGMA query_only=ON")
        reader.execute("BEGIN")
        T.eq(select_user(reader), "alpha", "maximum active card coverage wins")
        T.eq(select_user(reader, "beta"), "beta", "explicit valid user overrides")
        T.raises(ValueError, lambda: select_user(reader, "missing"),
                 "unknown explicit user is rejected")
        decks, cards = load_source(reader, "alpha")
        T.eq([d["id"] for d in decks], ["deck-a"], "archived deck is excluded")
        T.eq(len(cards), 3, "template cards from active deck are loaded")
        T.eq(cards[0]["front"], "一", "template content wins over copied study content")
        T.check(all(card["front"] != "CORRUPT" for card in cards),
                "duplicated study_cards content is never read")
        reader.close()
        writer.close()


def test_balanced_order():
    decks = [
        {"id": "a", "name": "A", "level": "N5", "deck_type": "kanji",
         "cards": [{"id": f"a{i}"} for i in range(3)]},
        {"id": "b", "name": "B", "level": "N4", "deck_type": "vocab",
         "cards": [{"id": f"b{i}"} for i in range(2)]},
    ]
    first = balanced_cards(copy.deepcopy(decks), 7)
    repeated = balanced_cards(copy.deepcopy(decks), 7)
    other = balanced_cards(copy.deepcopy(decks), 8)
    T.eq(first, repeated, "same seed gives the same order")
    T.eq([c["deck_index"] for c in first], [0, 1, 0, 1, 0],
         "non-empty decks are consumed round-robin")
    T.eq({c["id"] for c in first}, {c["id"] for c in other},
         "seed does not change membership")
    T.check([c["id"] for c in first] != [c["id"] for c in other],
            "different seed changes per-deck order")


def test_fixture_projection_and_format():
    decks, cards = load_fixture(FIXTURE)
    ordered = balanced_cards(decks, 0)
    image = encode_catalog(decks, ordered, 0x770000)
    repeated = encode_catalog(decks, balanced_cards(decks, 0), 0x770000)
    manifest = verify_catalog(image)
    T.eq(image, repeated, "repeated fixture output is byte-identical")
    T.eq(image[0:8], b"KJCAT01\0", "header magic is literal")
    T.eq(struct.unpack_from("<H", image, 8)[0], 1, "schema is at byte 8")
    T.eq(struct.unpack_from("<H", image, 10)[0], 128, "header size is at byte 10")
    T.eq(struct.unpack_from("<I", image, 32)[0], 128, "deck table starts at byte 128")
    T.eq(struct.unpack_from("<I", image, 36)[0], 128, "two deck records occupy 128 bytes")
    T.eq(struct.unpack_from("<I", image, 40)[0], 256, "card index follows deck table")
    T.eq(struct.unpack_from("<I", image, 44)[0], 60, "five 12-byte card entries are present")
    T.eq(struct.unpack_from("<I", image, 48)[0], 316, "block index follows card index")
    T.eq(struct.unpack_from("<I", image, 52)[0], 16, "one 16-byte block entry is present")
    T.eq(struct.unpack_from("<I", image, 56)[0], 332, "compressed data follows indexes")
    T.eq(struct.unpack_from("<Q", image, 64)[0], 0, "seed is encoded at byte 64")
    T.eq(zlib.crc32(image[:120]) & 0xFFFFFFFF,
         struct.unpack_from("<I", image, 120)[0], "header CRC covers bytes 0..119")
    T.eq(zlib.crc32(image[128:332]) & 0xFFFFFFFF,
         struct.unpack_from("<I", image, 124)[0], "tables CRC covers contiguous tables")
    T.eq((manifest.deck_count, manifest.card_count, manifest.block_count),
         (2, 5, 1), "fixture manifest counts decode")
    wealth = next(card["card"] for card in manifest.cards if card["card"]["front"] == "財")
    T.eq(wealth["hook_title"], "형성", "source principle survives")
    T.eq(wealth["composition"], "貝 + 才 = 財", "single-kanji equation removes self")
    T.eq([p["glyph"] for p in wealth["parts"]], ["財", "貝", "才"],
         "all source component rows survive")
    study = next(card["card"] for card in manifest.cards if card["card"]["front"] == "勉強")
    punish = next(card["card"] for card in manifest.cards if card["card"]["front"] == "懲らしめる")
    T.eq(study["composition"], "勉 + 強 = 勉強", "compound sub-radicals are excluded")
    T.eq(punish["composition"], "徴 + 心 = 懲", "okurigana result is constituent kanji")
    forbidden = {"image_url", "audio_url", "comments", "examples", "session",
                 "fsrs", "user_id", "study_card_id", "due_at", "tags"}
    T.check(not any(forbidden.intersection(card["card"]) for card in manifest.cards),
            "media, user, session, and FSRS keys are excluded")


def test_boundaries_and_rejection():
    deck = {"id": "many", "name": "Many", "level": "N5", "deck_type": "kanji"}
    cards = []
    for ordinal in range(65):
        card = {"id": f"c{ordinal:02}", "front": f"字{ordinal}", "reading": "ジ",
                "on_reading": "ジ", "kun_reading": "", "level": "N5",
                "gloss": "글자", "senses": ["글자"], "description": "설명",
                "hook_title": "", "hook_body": "기억", "composition": "",
                "parts": [], "deck_index": 0}
        cards.append(card)
    ordered = balanced_cards([{**deck, "cards": cards}], 0)
    image = encode_catalog([deck], ordered, 0x770000)
    manifest = verify_catalog(image)
    T.eq(manifest.block_count, 2, "65 cards cross the 64-card block boundary")
    T.eq([manifest.cards[i]["card"]["id"] for i in (0, 63, 64)],
         [ordered[i]["id"] for i in (0, 63, 64)],
         "first, last-in-block, and first-next-block indexes decode")
    damaged = bytearray(image)
    damaged[-1] ^= 0x01
    T.raises(ValueError, lambda: verify_catalog(bytes(damaged)),
             "compressed data corruption is rejected")
    damaged = bytearray(image)
    damaged[12] ^= 0x01
    T.raises(ValueError, lambda: verify_catalog(bytes(damaged)),
             "header CRC corruption is rejected")
    damaged = bytearray(image)
    damaged[128] ^= 0x01
    T.raises(ValueError, lambda: verify_catalog(bytes(damaged)),
             "table CRC corruption is rejected")
    damaged = bytearray(image)
    block_index_off = struct.unpack_from("<I", damaged, 48)[0]
    damaged[block_index_off + 12] ^= 0x01
    data_off = struct.unpack_from("<I", damaged, 56)[0]
    struct.pack_into("<I", damaged, 124,
                     zlib.crc32(damaged[128:data_off]) & 0xFFFFFFFF)
    T.raises(ValueError, lambda: verify_catalog(bytes(damaged)),
             "raw block CRC corruption is rejected independently")
    T.raises(ValueError, lambda: encode_catalog([deck], ordered, len(image) - 1),
             "partition overflow is rejected")
    huge = []
    for ordinal in range(64):
        item = copy.deepcopy(cards[ordinal % len(cards)])
        item["id"] = f"huge-{ordinal}"
        item["description"] = "d" * 831
        item["hook_body"] = "h" * 831
        huge.append(item)
    T.raises(ValueError,
             lambda: encode_catalog([deck], balanced_cards([{**deck, "cards": huge}], 0),
                                    0x770000),
             "raw block above 96 KiB is rejected")


def test_full_model_maxima_round_trip():
    deck = {"id": "maxima", "name": "Maxima", "level": "N1",
            "deck_type": "kanji"}
    card = {
        "id": "m" * 39, "front": "f" * 39, "reading": "r" * 143,
        "on_reading": "o" * 143, "kun_reading": "k" * 143, "level": "N1",
        "gloss": "g" * 143, "senses": [f"sense-{i}" for i in range(5)],
        "description": "d" * 819, "hook_title": "형성", "hook_body": "h" * 615,
        "composition": "c" * 95,
        "parts": [{"glyph": str(i), "meaning": f"part-{i}",
                   "reading": "" if i == 5 else f"read-{i}"} for i in range(6)],
    }
    ordered = balanced_cards([{**deck, "cards": [card]}], 0)
    manifest = verify_catalog(encode_catalog([deck], ordered, 0x770000))
    decoded = manifest.cards[0]["card"]
    T.eq(len(decoded["description"].encode("utf-8")), 819,
         "819-byte origin prose survives a round trip")
    T.eq(len(decoded["hook_body"].encode("utf-8")), 615,
         "615-byte mnemonic prose survives a round trip")
    T.eq(len(decoded["senses"]), 5, "five senses survive a round trip")
    T.eq(len(decoded["parts"]), 6, "six components survive a round trip")
    T.eq(decoded["parts"][5]["reading"], "",
         "a missing component reading remains empty")
    too_long = copy.deepcopy(card)
    too_long["description"] = "x" * 832
    T.raises(ValueError,
             lambda: encode_catalog(
                 [deck], balanced_cards([{**deck, "cards": [too_long]}], 0),
                 0x770000),
             "a field that cannot fit the device model is rejected")


def test_cli_atomic_fixture_write():
    with tempfile.TemporaryDirectory() as tmp:
        output = os.path.join(tmp, "catalog.bin")
        command = [sys.executable, GENERATOR, "--fixture-json", FIXTURE,
                   "--output", output, "--partition-size", "0x770000",
                   "--seed", "0", "--verify"]
        first = subprocess.run(command, check=False, text=True, capture_output=True)
        T.eq(first.returncode, 0, f"fixture CLI succeeds: {first.stderr}")
        before = os.stat(output)
        second = subprocess.run(command, check=False, text=True, capture_output=True)
        after = os.stat(output)
        T.eq(second.returncode, 0, f"repeated fixture CLI succeeds: {second.stderr}")
        T.eq((before.st_ino, before.st_mtime_ns), (after.st_ino, after.st_mtime_ns),
             "unchanged output is not replaced")
        T.check("decks=2 cards=5 blocks=1" in second.stdout,
                "verify CLI prints a compact manifest")


def main():
    test_source_selection()
    test_balanced_order()
    test_fixture_projection_and_format()
    test_boundaries_and_rejection()
    test_full_model_maxima_round_trip()
    test_cli_atomic_fixture_write()
    if T.failures:
        print(f"{T.count} checks, {T.failures} failures", file=sys.stderr)
        return 1
    print(f"ok: {T.count} checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
