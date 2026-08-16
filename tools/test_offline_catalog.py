#!/usr/bin/env python3
"""Behavior tests for the deterministic offline catalog generator."""

import copy
import csv
import hashlib
import json
import os
import shlex
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
from gen_fonts import symbol_set  # noqa: E402
from kanji_server import check_glyphs  # noqa: E402


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE = os.path.join(ROOT, "tools", "fixtures", "offline_catalog.json")
GENERATOR = os.path.join(ROOT, "tools", "gen_offline_catalog.py")
PARTITIONS = os.path.join(ROOT, "partitions.csv")

# The device model's caps, restated here as literals rather than imported from
# kanji_model.h through kanji_server.  A test that reads its expectation from
# the same header the generator clips against cannot catch the generator
# clipping against the wrong header field, which is precisely the mistake that
# ships a 40-byte example.text into a 40-byte char array.
EXAMPLE_TEXT_CAP = 39
EXAMPLE_READING_CAP = 143
EXAMPLE_GLOSS_CAP = 143
EXAMPLES_PER_CARD = 3


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

    def raises_matching(self, error, text, operation, message):
        self.count += 1
        try:
            operation()
        except error as exc:
            if text in str(exc):
                return
            self.failures += 1
            print(f"FAIL: {message}: raised {exc!r}, expected text {text!r}",
                  file=sys.stderr)
            return
        except Exception as exc:  # pragma: no cover - diagnostic path
            self.failures += 1
            print(f"FAIL: {message}: raised {type(exc).__name__}: {exc}",
                  file=sys.stderr)
            return
        self.failures += 1
        print(f"FAIL: {message}: did not raise {error.__name__}", file=sys.stderr)


T = Checks()


def parse_size(value):
    value = value.strip()
    suffixes = {"K": 1024, "M": 1024 * 1024}
    if value[-1:].upper() in suffixes:
        return int(value[:-1], 0) * suffixes[value[-1:].upper()]
    return int(value, 0)


def load_partitions(path):
    partitions = []
    cursor = 0x9000  # default 0x8000 table offset plus its 4 KiB sector
    with open(path, newline="", encoding="utf-8") as source:
        for row in csv.reader(line for line in source if not line.lstrip().startswith("#")):
            if not row or not any(cell.strip() for cell in row):
                continue
            name, kind, subtype, offset, size, *flags = [cell.strip() for cell in row]
            size_value = parse_size(size)
            if offset:
                offset_value = parse_size(offset)
            else:
                alignment = 0x10000 if kind.lower() in ("app", "0x00") else 0x1000
                offset_value = (cursor + alignment - 1) & ~(alignment - 1)
            partitions.append({
                "name": name,
                "type": kind.lower(),
                "subtype": subtype.lower(),
                "offset": offset_value,
                "size": size_value,
                "flags": {flag.strip().lower() for flag in flags if flag.strip()},
            })
            cursor = offset_value + size_value
    return partitions


def _oracle_json_object(column):
    """Decode one raw DB JSON column without using the production projector."""
    if not isinstance(column, str):
        return {}
    try:
        value = json.loads(column)
    except (TypeError, ValueError):
        return {}
    return value if isinstance(value, dict) else {}


def _independent_formula_from_raw(front, back_column, hint_column):
    """Derive the documented safe equation directly from a raw database row.

    This is deliberately test-local.  It does not call raw_card_parts(),
    safe_composition(), project_card_content(), or consume their projected
    output, so a production formula mutation cannot redefine the expectation.
    """
    back = _oracle_json_object(back_column)
    hint = _oracle_json_object(hint_column)
    target = back.get("kanji") if isinstance(back.get("kanji"), str) else ""
    if not target:
        target = front if isinstance(front, str) else ""
    if not target:
        return ""

    shapes = hint.get("shapes")
    glyphs = []
    if isinstance(shapes, list):
        for shape in shapes:
            if not isinstance(shape, dict):
                continue
            glyph = shape.get("kanji")
            if isinstance(glyph, str) and glyph:
                glyphs.append(glyph)
    if len(target) == 1:
        selected = [glyph for glyph in glyphs if glyph != target]
    else:
        selected = [glyph for glyph in glyphs if glyph in target]
    left = " + ".join(selected)
    return f"{left} = {target}" if left else ""


def _load_live_formula_oracle(connection, user_id):
    """Load raw reachable card rows and derive formula expectations locally."""
    rows = connection.execute("""
        SELECT ct.id, ct.front, ct.back, ct.hint
          FROM study_decks AS sd
          JOIN deck_templates AS dt ON dt.id = sd.template_deck_id
          JOIN card_templates AS ct ON ct.template_deck_id = dt.id
         WHERE sd.user_id = ? AND sd.archived_at IS NULL
         ORDER BY dt.sort_order ASC, dt.id ASC, ct.sort_order ASC, ct.id ASC
    """, (user_id,)).fetchall()
    expected = {}
    for card_id, front, back_column, hint_column in rows:
        if not isinstance(card_id, str) or not card_id:
            raise ValueError("raw formula row has no stable card id")
        if card_id in expected:
            raise ValueError(f"duplicate raw formula row: {card_id}")
        expected[card_id] = _independent_formula_from_raw(
            front, back_column, hint_column)
    return expected


def _independent_gloss_from_raw(back_column):
    """Derive the short gloss from raw JSON without the production projector."""
    back = _oracle_json_object(back_column)
    meaning = back.get("meaning")
    if not isinstance(meaning, dict):
        return ""
    gloss = meaning.get("gloss")
    if isinstance(gloss, str) and gloss.strip():
        return gloss.strip()
    senses = meaning.get("senses")
    if not isinstance(senses, list):
        return ""
    return next((sense.strip() for sense in senses
                 if isinstance(sense, str) and sense.strip()), "")


def _load_live_gloss_oracle(connection, user_id):
    """Load the expected short gloss for every reachable raw card row."""
    rows = connection.execute("""
        SELECT ct.id, ct.back
          FROM study_decks AS sd
          JOIN deck_templates AS dt ON dt.id = sd.template_deck_id
          JOIN card_templates AS ct ON ct.template_deck_id = dt.id
         WHERE sd.user_id = ? AND sd.archived_at IS NULL
         ORDER BY dt.sort_order ASC, dt.id ASC, ct.sort_order ASC, ct.id ASC
    """, (user_id,)).fetchall()
    expected = {}
    for card_id, back_column in rows:
        if not isinstance(card_id, str) or not card_id:
            raise ValueError("raw gloss row has no stable card id")
        if card_id in expected:
            raise ValueError(f"duplicate raw gloss row: {card_id}")
        expected[card_id] = _independent_gloss_from_raw(back_column)
    return expected


def _validate_independent_formulas(decoded_cards, expected_by_id):
    """Reject any decoded formula that differs from the raw-row oracle."""
    if len(decoded_cards) != len(expected_by_id):
        raise ValueError(
            f"formula cardinality mismatch: decoded={len(decoded_cards)} "
            f"raw={len(expected_by_id)}")
    seen = set()
    nonempty = 0
    for ordinal, card in enumerate(decoded_cards):
        card_id = card.get("id") if isinstance(card, dict) else None
        if not isinstance(card_id, str) or card_id not in expected_by_id:
            raise ValueError(f"formula card {ordinal} has no raw-row oracle")
        if card_id in seen:
            raise ValueError(f"duplicate decoded formula card: {card_id}")
        seen.add(card_id)
        expected = expected_by_id[card_id]
        actual = card.get("composition")
        if actual != expected:
            raise ValueError(
                f"formula mismatch at ordinal {ordinal} card {card_id}: "
                f"got {actual!r}, expected {expected!r}")
        nonempty += bool(expected)
    missing = set(expected_by_id).difference(seen)
    if missing:
        raise ValueError(f"formula oracle rows were not decoded: {len(missing)}")
    return len(decoded_cards), nonempty, len(decoded_cards) - nonempty


def _validate_independent_glosses(decoded_cards, expected_by_id):
    """Reject any decoded gloss that differs from raw gloss-or-sense data."""
    if len(decoded_cards) != len(expected_by_id):
        raise ValueError(
            f"gloss cardinality mismatch: decoded={len(decoded_cards)} "
            f"raw={len(expected_by_id)}")
    seen = set()
    nonempty = 0
    for ordinal, card in enumerate(decoded_cards):
        card_id = card.get("id") if isinstance(card, dict) else None
        if not isinstance(card_id, str) or card_id not in expected_by_id:
            raise ValueError(f"gloss card {ordinal} has no raw-row oracle")
        if card_id in seen:
            raise ValueError(f"duplicate decoded gloss card: {card_id}")
        seen.add(card_id)
        expected = expected_by_id[card_id]
        actual = card.get("gloss")
        if actual != expected:
            raise ValueError(
                f"gloss mismatch at ordinal {ordinal} card {card_id}: "
                f"got {actual!r}, expected {expected!r}")
        nonempty += bool(actual)
    missing = set(expected_by_id).difference(seen)
    if missing:
        raise ValueError(f"gloss oracle rows were not decoded: {len(missing)}")
    return len(decoded_cards), nonempty


def _independent_clip(text, cap):
    """Truncate to `cap` bytes on a character boundary. Test-local on purpose.

    Deliberately not kanji_server.clip(): if the production clipper started
    cutting mid-sequence, or started counting characters instead of bytes,
    calling it here would move the expectation with the bug.  Encoding each
    character and stopping before the one that would not fit is the definition
    the C side's kanji_str_copy() implements, written out.
    """
    out = ""
    used = 0
    for character in text:
        width = len(character.encode("utf-8"))
        if used + width > cap:
            break
        out += character
        used += width
    return out


def _independent_examples_from_raw(back_column):
    """Derive the three packed 예문 straight from a raw `back` column.

    Test-local for the same reason the formula and gloss oracles are: it must
    be possible for card_examples() to be rewritten and for this file to say
    so.  The rule being asserted is the documented one — walk on_yomi[] then
    kun_yomi[] in source order, take each entry's examples[] in source order,
    skip a row with no Japanese text, stop at three, clip each field to the
    device model's cap.
    """
    back = _oracle_json_object(back_column)
    out = []
    entries = []
    for key in ("on_yomi", "kun_yomi"):
        value = back.get(key)
        if isinstance(value, list):
            entries.extend(entry for entry in value if isinstance(entry, dict))
    for entry in entries:
        examples = entry.get("examples")
        if not isinstance(examples, list):
            continue
        for example in examples:
            if not isinstance(example, dict):
                continue
            text = example.get("text")
            if not isinstance(text, str) or not text:
                continue
            reading = example.get("reading")
            gloss = example.get("gloss")
            out.append({
                "text": _independent_clip(text, EXAMPLE_TEXT_CAP),
                "reading": _independent_clip(
                    reading if isinstance(reading, str) else "", EXAMPLE_READING_CAP),
                "gloss": _independent_clip(
                    gloss if isinstance(gloss, str) else "", EXAMPLE_GLOSS_CAP),
            })
            if len(out) >= EXAMPLES_PER_CARD:
                return out
    return out


def _load_live_example_oracle(connection, user_id):
    """Load the expected 예문 rows for every reachable raw card row."""
    rows = connection.execute("""
        SELECT ct.id, ct.back
          FROM study_decks AS sd
          JOIN deck_templates AS dt ON dt.id = sd.template_deck_id
          JOIN card_templates AS ct ON ct.template_deck_id = dt.id
         WHERE sd.user_id = ? AND sd.archived_at IS NULL
         ORDER BY dt.sort_order ASC, dt.id ASC, ct.sort_order ASC, ct.id ASC
    """, (user_id,)).fetchall()
    expected = {}
    for card_id, back_column in rows:
        if not isinstance(card_id, str) or not card_id:
            raise ValueError("raw example row has no stable card id")
        if card_id in expected:
            raise ValueError(f"duplicate raw example row: {card_id}")
        expected[card_id] = _independent_examples_from_raw(back_column)
    return expected


def _validate_independent_examples(decoded_cards, expected_by_id):
    """Reject any decoded example list that differs from the raw-row oracle."""
    if len(decoded_cards) != len(expected_by_id):
        raise ValueError(
            f"example cardinality mismatch: decoded={len(decoded_cards)} "
            f"raw={len(expected_by_id)}")
    seen = set()
    nonempty = rows = 0
    for ordinal, card in enumerate(decoded_cards):
        card_id = card.get("id") if isinstance(card, dict) else None
        if not isinstance(card_id, str) or card_id not in expected_by_id:
            raise ValueError(f"example card {ordinal} has no raw-row oracle")
        if card_id in seen:
            raise ValueError(f"duplicate decoded example card: {card_id}")
        seen.add(card_id)
        expected = expected_by_id[card_id]
        actual = card.get("examples")
        if actual != expected:
            raise ValueError(
                f"example mismatch at ordinal {ordinal} card {card_id}: "
                f"got {actual!r}, expected {expected!r}")
        nonempty += bool(expected)
        rows += len(expected)
    missing = set(expected_by_id).difference(seen)
    if missing:
        raise ValueError(f"example oracle rows were not decoded: {len(missing)}")
    return len(decoded_cards), nonempty, rows


def test_independent_example_oracle_rules():
    """The raw-row oracle's own rules, pinned before it judges 9,956 cards."""
    back = json.dumps({
        "on_yomi": [
            {"reading": "カン", "examples": [
                {"text": "", "reading": "skip", "gloss": "skip"},
                {"text": "関連", "reading": "かんれん", "gloss": "관련"},
            ]},
            {"reading": "ケン", "examples": [
                {"text": "関係", "reading": "かんけい"},
            ]},
        ],
        "kun_yomi": [
            {"reading": "せき", "examples": [
                {"text": "関所", "reading": "せきしょ", "gloss": "관문"},
                {"text": "関わる", "reading": "かかわる", "gloss": "관계되다"},
            ]},
        ],
    }, ensure_ascii=False)
    T.eq(_independent_examples_from_raw(back), [
        {"text": "関連", "reading": "かんれん", "gloss": "관련"},
        {"text": "関係", "reading": "かんけい", "gloss": ""},
        {"text": "関所", "reading": "せきしょ", "gloss": "관문"},
    ], "raw-row oracle walks on before kun, skips blanks, and stops at three")
    T.eq(_independent_examples_from_raw("not json"), [],
        "raw-row oracle treats an unusable back column as no examples")
    T.eq(_independent_clip("字" * 20, EXAMPLE_TEXT_CAP), "字" * 13,
         "raw-row oracle clips on a character boundary, never mid-sequence")


def _parse_flash_payloads(arguments_path, build_directory):
    """Parse and stat every payload in an esptool 4.10 write_flash argfile.

    esptool's expand_file_arguments() shlex-splits every line and concatenates
    the result before argparse sees it, so line boundaries have no meaning.
    Keep the accepted option list and arities explicit: guessing an unknown
    option's extent could silently consume a payload pair.  Encrypted groups
    are not generated by the normal targets checked here and are rejected.
    """
    one_value_options = {
        "--flash_freq", "-ff", "--flash_mode", "-fm",
        "--flash_size", "-fs", "--spi-connection", "-sc",
    }
    flag_options = {
        "--no-progress", "-p", "--verify", "--encrypt",
        "--ignore-flash-encryption-efuse-setting", "--force",
        "--compress", "-z", "--no-compress", "-u",
    }
    unsupported_options = {"--encrypt-files", "--erase-all", "-e"}

    tokens = []
    with open(arguments_path, encoding="utf-8") as source:
        for line_number, line in enumerate(source, 1):
            try:
                tokens.extend(shlex.split(line))
            except ValueError as exc:
                raise ValueError(
                    f"{arguments_path}:{line_number}: malformed quoting: {exc}") from None

    positional = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "--":
            positional.extend(tokens[index + 1:])
            break
        if not token.startswith("-"):
            positional.append(token)
            index += 1
            continue

        option, separator, attached_value = token.partition("=")
        if option in unsupported_options:
            raise ValueError(
                f"{arguments_path}: unsupported flash option {option}")
        if option in flag_options:
            if separator:
                raise ValueError(
                    f"{arguments_path}: flash option {option} takes no value")
            index += 1
            continue
        if option in one_value_options:
            if separator:
                if not attached_value:
                    raise ValueError(
                        f"{arguments_path}: flash option {option} requires one value")
                index += 1
                continue
            if index + 1 >= len(tokens) or tokens[index + 1].startswith("-"):
                raise ValueError(
                    f"{arguments_path}: flash option {option} requires one value")
            index += 2
            continue
        raise ValueError(f"{arguments_path}: unknown flash option {option}")

    if len(positional) % 2:
        raise ValueError(
            f"{arguments_path}: payloads must be address/file pairs; "
            f"got {len(positional)} trailing tokens")

    payloads = []
    offsets = set()
    for offset_token, filename_token in zip(positional[::2], positional[1::2]):
        try:
            offset = int(offset_token, 0)
        except ValueError:
            raise ValueError(
                f"{arguments_path}: invalid payload offset {offset_token!r}") from None
        if offset in offsets:
            raise ValueError(
                f"{arguments_path}: duplicate offset 0x{offset:x}")
        offsets.add(offset)
        filename = os.path.normpath(filename_token)
        payload_path = (filename if os.path.isabs(filename)
                        else os.path.join(build_directory, filename))
        size = os.path.getsize(payload_path)
        end = offset + size
        if offset < 0 or size <= 0 or end <= offset or end > 0x1000000:
            raise ValueError(
                f"{filename}: invalid flash interval [0x{offset:x},0x{end:x})")
        payloads.append({
            "offset": offset,
            "end": end,
            "size": size,
            "file": filename,
        })
    if not payloads:
        raise ValueError(f"{arguments_path}: contains no flash payloads")
    return payloads


def _assert_flash_intervals_avoid(payloads, protected_ranges, context):
    for payload in payloads:
        for start, end, name in protected_ranges:
            if payload["offset"] < end and start < payload["end"]:
                raise ValueError(
                    f"{context} payload {payload['file']} "
                    f"[0x{payload['offset']:x},0x{payload['end']:x}) overlaps "
                    f"{name} [0x{start:x},0x{end:x})")


def _assert_flash_payloads_disjoint(payloads, context):
    ordered = sorted(payloads, key=lambda payload: payload["offset"])
    for left, right in zip(ordered, ordered[1:]):
        if left["end"] > right["offset"]:
            raise ValueError(
                f"{context} payload {left['file']} "
                f"[0x{left['offset']:x},0x{left['end']:x}) overlaps "
                f"{right['file']} [0x{right['offset']:x},0x{right['end']:x})")


def _assert_flash_payload_fits(payload, start, end, region, context):
    if payload["offset"] < start or payload["end"] > end:
        raise ValueError(
            f"{context} payload {payload['file']} "
            f"[0x{payload['offset']:x},0x{payload['end']:x}) escapes "
            f"{region} [0x{start:x},0x{end:x})")


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
    # These are literal SHA-256 results for the specified byte material, not
    # values recomputed through the production key builder.  In particular,
    # seed 7 is the eight little-endian bytes 07 00 00 00 00 00 00 00 and the
    # two separators are single NUL bytes:
    #
    #   07000000000000000061006131 -> sha256 0f605ef4... -> a1
    #   07000000000000000061006130 -> sha256 692f210e... -> a0
    #   07000000000000000061006132 -> sha256 71ce94c5... -> a2
    #   07000000000000000062006230 -> sha256 c0ba41a7... -> b0
    #   07000000000000000062006231 -> sha256 edb11a8b... -> b1
    T.eq([c["id"] for c in first], ["a1", "b0", "a0", "b1", "a2"],
         "literal SHA-256 key material fixes per-deck order before interleave")
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
    # `examples` used to sit in this set.  It was moved out deliberately, not
    # relaxed: the redesigned question face quotes a Japanese example sentence
    # and the answer face carries an 예문 block, so an offline card without
    # examples draws two empty rectangles the layout still reserves.  The
    # exclusion that remains is the one that was always the point — nothing
    # user-specific, session-specific, per-review or fetched-at-runtime belongs
    # in a read-only image flashed once.
    forbidden = {"image_url", "audio_url", "comments", "session",
                 "fsrs", "user_id", "study_card_id", "due_at", "tags"}
    T.check(not any(forbidden.intersection(card["card"]) for card in manifest.cards),
            "media, user, session, and FSRS keys are excluded")
    T.check(all("examples" in card["card"] for card in manifest.cards),
            "every packed card carries an examples array")
    # The committed fixture's cards have no examples[] in their back JSON, so
    # this is also the "a card with no examples still packs" case running on
    # every build that touches the generator.
    T.eq([card["card"].get("examples") for card in manifest.cards], [[]] * 5,
         "a fixture card with no source examples packs an empty array")


def test_independent_formula_oracle_rules():
    cases = (
        (
            "財",
            json.dumps({"kanji": "財"}, ensure_ascii=False),
            json.dumps({"shapes": [
                {"kanji": "財"}, {"kanji": "貝"}, {"kanji": "才"},
            ]}, ensure_ascii=False),
            "貝 + 才 = 財",
            "raw-row oracle removes a single-kanji self reference",
        ),
        (
            "勉強",
            json.dumps({"kanji": "勉強"}, ensure_ascii=False),
            json.dumps({"shapes": [
                {"kanji": "勉"}, {"kanji": "強"}, {"kanji": "免"},
                {"kanji": "力"}, {"kanji": "弓"},
            ]}, ensure_ascii=False),
            "勉 + 強 = 勉強",
            "raw-row oracle excludes compound sub-radicals",
        ),
        (
            "懲らしめる",
            json.dumps({"kanji": "懲"}, ensure_ascii=False),
            json.dumps({"shapes": [
                {"kanji": "懲"}, {"kanji": "徴"}, {"kanji": "心"},
            ]}, ensure_ascii=False),
            "徴 + 心 = 懲",
            "raw-row oracle uses the constituent kanji for okurigana",
        ),
    )
    for front, back, hint, expected, message in cases:
        T.eq(_independent_formula_from_raw(front, back, hint), expected, message)


# ---------------------------------------------------------------------------
# 예문
# ---------------------------------------------------------------------------
#
# The offline card is the ONLY card a board with no proxy on the LAN will ever
# show, and the redesigned question face puts a Japanese example sentence on it
# as a pull-quote.  A packed card with no examples renders that pull-quote and
# the answer face's 예문 block as empty rectangles the layout still reserves
# space for, so "offline" reads to the learner as "broken" rather than as
# "unplugged".  Everything below is about the one field that closes that gap.


def _example_deck(cards, deck_id="deck-examples", level="N5", deck_type="kanji"):
    """One source-shaped deck, ready for json.dump() into a fixture file."""
    return {"id": deck_id, "name": f"JLPT {level} Kanji", "level": level,
            "deck_type": deck_type, "cards": cards}


def _example_card(card_id, front, on_entries, kun_entries, level="N5"):
    """One source-shaped card whose readings carry the given example rows.

    Shaped like a real card_templates row: `back` holds on_yomi[]/kun_yomi[],
    each entry carrying its own examples[] of {text, reading, gloss}.  The
    entries are lists of example lists so a test can put examples on the second
    on-yomi reading, which is where the on-before-kun ordering rule stops being
    the same thing as "the first entry wins".
    """
    return {
        "id": card_id,
        "front": front,
        "back": {
            "kanji": front,
            "meaning": {"gloss": f"gloss-{card_id}", "senses": [f"sense-{card_id}"]},
            "on_yomi": [{"reading": f"オン{index}", "examples": examples}
                        for index, examples in enumerate(on_entries)],
            "kun_yomi": [{"reading": f"くん{index}", "examples": examples}
                         for index, examples in enumerate(kun_entries)],
            "shape_explanation": f"설명-{card_id}",
        },
        "hint": {"principle": "형성", "reason": f"이유-{card_id}", "shapes": []},
        "tags": [level],
    }


def _pack_example_cards(cards, seed=0):
    """Take source-shaped cards through the real fixture path to decoded cards.

    Deliberately through load_fixture() and a file on disk rather than through
    a hand-built dict: load_fixture() and load_source() are two call sites of
    the same projection, and a field wired into only one of them is exactly the
    bug that ships a full catalog and an empty simulator (or the reverse).
    """
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "examples.json")
        with open(path, "w", encoding="utf-8") as output:
            json.dump({"decks": [_example_deck(cards)]}, output, ensure_ascii=False)
        decks, _cards = load_fixture(path)
    ordered = balanced_cards(decks, seed)
    manifest = verify_catalog(encode_catalog(decks, ordered, 0x770000))
    by_id = {envelope["card"]["id"]: envelope["card"] for envelope in manifest.cards}
    return decks, ordered, manifest, by_id


def test_examples_are_packed_in_wire_order():
    example = lambda tag: {"text": f"例{tag}", "reading": f"れい{tag}",  # noqa: E731
                           "gloss": f"예{tag}"}
    cards = [
        # Three on-yomi examples spread over two readings, then a kun-yomi one
        # that must lose: on before kun, source order within each.
        _example_card("ordered", "関",
                      [[example("a"), example("b")], [example("c")]],
                      [[example("k")]]),
        # A fourth row is dropped rather than packed and silently truncated on
        # the device: KANJI_EXAMPLES_MAX is 3 and kanji_parse.c stops there.
        _example_card("capped", "係",
                      [[example("1"), example("2"), example("3"), example("4")]],
                      []),
        # kanji_parse.c's parse_examples() drops a row with a blank text ("no
        # text, no row").  Packing one would make the catalog's example count
        # disagree with what the panel draws, which is invisible until somebody
        # counts pull-quotes.
        _example_card("blank", "節",
                      [[{"text": "", "reading": "れい", "gloss": "예"},
                        example("x"), example("y"), example("z")]],
                      []),
        # A card with nothing to quote still has to pack.  1,525 of the 9,956
        # real cards are this card.
        _example_card("silent", "税", [[]], []),
    ]
    _decks, _ordered, _manifest, by_id = _pack_example_cards(cards)

    T.eq([e["text"] for e in by_id["ordered"]["examples"]], ["例a", "例b", "例c"],
         "on-yomi examples fill the three rows before kun-yomi is reached")
    T.eq(by_id["ordered"]["examples"][0],
         {"text": "例a", "reading": "れいa", "gloss": "예a"},
         "an example keeps its text, reading and Korean gloss")
    T.eq([e["text"] for e in by_id["capped"]["examples"]], ["例1", "例2", "例3"],
         "a fourth example is dropped, not packed")
    T.eq([e["text"] for e in by_id["blank"]["examples"]], ["例x", "例y", "例z"],
         "an example the device parser would drop is never packed")
    T.eq(by_id["silent"]["examples"], [],
         "a card with no examples packs an empty array, not a missing key")
    for card_id, card in by_id.items():
        T.check("examples" in card, f"card {card_id} carries the examples key")


def test_example_order_is_deterministic():
    example = lambda tag: {"text": f"例{tag}", "reading": f"れい{tag}",  # noqa: E731
                           "gloss": f"예{tag}"}
    # Two on-yomi readings whose example rows would sort differently under any
    # incidental ordering (a dict, a set, a hash) than they do in source order.
    cards = [_example_card("deterministic", "関",
                           [[example("z"), example("a")], [example("m")]], [])]
    decks, ordered, manifest, by_id = _pack_example_cards(cards)
    T.eq([e["text"] for e in by_id["deterministic"]["examples"]],
         ["例z", "例a", "例m"],
         "source order is preserved, not sorted")
    repeated = encode_catalog(decks, balanced_cards(decks, 0), 0x770000)
    T.eq(encode_catalog(decks, ordered, 0x770000), repeated,
         "a catalog carrying examples is still byte-identical when rebuilt")


def test_example_caps_truncate_on_a_utf8_boundary():
    # 20 kanji is 60 bytes against a 39-byte field; 60 kana and 60 hangul are
    # 180 bytes each against 143-byte fields.  None of the three caps lands on
    # a character boundary, which is the whole point: a byte-count truncation
    # would leave a half-encoded lead byte the C reader turns into a tofu box.
    long_example = {"text": "字" * 20, "reading": "あ" * 60, "gloss": "한" * 60}
    cards = [_example_card("long", "字", [[long_example]], [])]
    _decks, _ordered, _manifest, by_id = _pack_example_cards(cards)
    packed = by_id["long"]["examples"][0]

    T.eq(packed["text"], "字" * 13, "39-byte text cap keeps 13 whole kanji")
    T.eq(packed["reading"], "あ" * 47, "143-byte reading cap keeps 47 whole kana")
    T.eq(packed["gloss"], "한" * 47, "143-byte gloss cap keeps 47 whole hangul")
    for field, cap in (("text", EXAMPLE_TEXT_CAP),
                       ("reading", EXAMPLE_READING_CAP),
                       ("gloss", EXAMPLE_GLOSS_CAP)):
        raw = packed[field].encode("utf-8")
        T.check(len(raw) <= cap,
                f"example {field} is {len(raw)} bytes, at or under {cap}")
        T.check(raw.decode("utf-8", errors="strict") == packed[field],
                f"truncated example {field} is still whole UTF-8")
        T.check(long_example[field].startswith(packed[field]),
                f"example {field} loses only its tail")


def test_oversized_example_is_rejected_not_clipped():
    """The encoder refuses bytes the device would throw away rather than pack.

    The generator clips at the projection, so an over-long example can only
    reach encode_catalog() if a caller hand-built one.  _clean_card() is the
    backstop, and it has to be a rejection: silently shortening here would let
    a future projection bug ship 9,956 quietly-different cards.
    """
    deck = {"id": "over", "name": "Over", "level": "N5", "deck_type": "kanji"}
    base = {"id": "over-1", "front": "字", "reading": "ジ", "on_reading": "ジ",
            "kun_reading": "", "level": "N5", "gloss": "글자", "senses": ["글자"],
            "description": "설명", "hook_title": "", "hook_body": "기억",
            "composition": "", "parts": [], "examples": []}
    for field, size in (("text", EXAMPLE_TEXT_CAP + 1),
                        ("reading", EXAMPLE_READING_CAP + 1),
                        ("gloss", EXAMPLE_GLOSS_CAP + 1)):
        card = copy.deepcopy(base)
        card["examples"] = [{"text": "例", "reading": "れい", "gloss": "예"}]
        card["examples"][0][field] = "x" * size
        T.raises_matching(
            ValueError, field,
            lambda card=card: encode_catalog(
                [deck], balanced_cards([{**deck, "cards": [card]}], 0), 0x770000),
            f"an example {field} one byte over the device cap is rejected")

    too_many = copy.deepcopy(base)
    too_many["examples"] = [{"text": f"例{i}", "reading": "れい", "gloss": "예"}
                            for i in range(EXAMPLES_PER_CARD + 1)]
    T.raises_matching(
        ValueError, "example",
        lambda: encode_catalog(
            [deck], balanced_cards([{**deck, "cards": [too_many]}], 0), 0x770000),
        "a fourth example is rejected by the encoder as well as the projection")

    blank = copy.deepcopy(base)
    blank["examples"] = [{"text": "", "reading": "れい", "gloss": "예"}]
    T.raises_matching(
        ValueError, "example",
        lambda: encode_catalog(
            [deck], balanced_cards([{**deck, "cards": [blank]}], 0), 0x770000),
        "an example the device parser drops is rejected rather than packed")

    malformed = copy.deepcopy(base)
    malformed["examples"] = [{"text": "例", "reading": "れい"}]
    T.raises_matching(
        ValueError, "example",
        lambda: encode_catalog(
            [deck], balanced_cards([{**deck, "cards": [malformed]}], 0), 0x770000),
        "an example missing a field is rejected rather than half-packed")


def _packed_strings(card):
    """Every string a packed card puts on the glass, with a label for each."""
    for key in ("front", "reading", "on_reading", "kun_reading", "level",
                "gloss", "description", "hook_title", "hook_body", "composition"):
        yield card[key], key
    for sense in card["senses"]:
        yield sense, "sense"
    for example in card["examples"]:
        yield example["text"], "example.text"
        yield example["reading"], "example.reading"
        yield example["gloss"], "example.gloss"
    for part in card["parts"]:
        yield part["glyph"], "part.glyph"
        yield part["meaning"], "part.meaning"
        yield part["reading"], "part.reading"


def undrawable_fields(cards, charset):
    """(id, label, character) for every packed string the faces cannot draw.

    The offline catalog never passes through kanji_server's substitute_missing()
    — it is written once, at build time, and the board has no second chance to
    ask anybody for a replacement.  So a character no shipped face covers is a
    tofu box burned into 0x770000 of read-only flash, and the only place to
    catch it is here.  check_glyphs() and symbol_set() are the same two halves
    the wire path's sweep uses, so "drawable" means the same thing on both
    sides rather than two lists to keep in step.
    """
    found = []
    for card in cards:
        for value, label in _packed_strings(card):
            for character in sorted(check_glyphs({"s": value}, charset, warn=False)):
                found.append((card.get("id"), label, character))
    return found


def example_glyph_failures(cards, charset):
    """Only the undrawable characters that came in through an example row.

    Examples get a hard gate and the rest of the card does not, and the reason
    is not that the rest matters less.  It is that the offline catalog ALREADY
    packs undrawable characters into 설명 prose and component meanings — see
    the UNDRAWABLE lines the live sweep prints — because it has no equivalent
    of the wire path's substitute_missing() step.  Pinning that set here would
    write a font bug down as an expectation.  Examples are new, they are clean,
    and a gate that starts at zero is the only one that stays at zero.
    """
    return [entry for entry in undrawable_fields(cards, charset)
            if entry[1].startswith("example.")]


def test_packed_examples_pass_the_glyph_gate():
    charset = symbol_set()
    decks, _cards = load_fixture(FIXTURE)
    manifest = verify_catalog(encode_catalog(decks, balanced_cards(decks, 0), 0x770000))
    fixture_cards = [envelope["card"] for envelope in manifest.cards]
    T.eq(undrawable_fields(fixture_cards, charset)[:3], [],
         "every string the fixture catalog packs is drawable by a shipped face")

    # The gate has to be able to fail, or it is decoration.  U+1F600 is in no
    # face this board ships and never will be.
    _decks, _ordered, _manifest, by_id = _pack_example_cards(
        [_example_card("emoji", "字",
                       [[{"text": "例\U0001F600", "reading": "れい", "gloss": "예"}]], [])])
    T.eq([(label, character)
          for _id, label, character in example_glyph_failures([by_id["emoji"]], charset)],
         [("example.text", "\U0001F600")],
         "an undrawable character inside an example is caught, not ignored")


def rebuilt_noncanonical_image(image):
    """Rebuild every dependent field after adding JSON-leading whitespace."""
    header = bytearray(image[:128])
    deck_off = struct.unpack_from("<I", header, 32)[0]
    deck_len = struct.unpack_from("<I", header, 36)[0]
    card_index_off = struct.unpack_from("<I", header, 40)[0]
    card_index_len = struct.unpack_from("<I", header, 44)[0]
    block_index_off = struct.unpack_from("<I", header, 48)[0]
    data_off = struct.unpack_from("<I", header, 56)[0]
    seed = struct.unpack_from("<Q", header, 64)[0]
    card_count = struct.unpack_from("<I", header, 24)[0]
    block_count = struct.unpack_from("<I", header, 28)[0]
    T.eq(block_count, 1, "noncanonical mutation fixture has exactly one block")

    deck_table = image[deck_off:deck_off + deck_len]
    card_index = bytearray(image[card_index_off:card_index_off + card_index_len])
    compressed_off, compressed_len, raw_len, _raw_crc = struct.unpack_from(
        "<IIII", image, block_index_off)
    raw = zlib.decompress(image[compressed_off:compressed_off + compressed_len])
    T.eq(len(raw), raw_len, "mutation starts from the indexed raw length")

    records = []
    deck_indexes = []
    for ordinal in range(card_count):
        record_off, record_len, deck_index = struct.unpack_from(
            "<IIB3x", card_index, ordinal * 12)
        records.append(raw[record_off:record_off + record_len])
        deck_indexes.append(deck_index)
    records[0] = b" " + records[0]

    rebuilt_raw = bytearray()
    for ordinal, (record, deck_index) in enumerate(zip(records, deck_indexes)):
        struct.pack_into("<IIB3x", card_index, ordinal * 12,
                         len(rebuilt_raw), len(record), deck_index)
        rebuilt_raw.extend(record)
    compressed = zlib.compress(bytes(rebuilt_raw), 9)
    block_index = struct.pack(
        "<IIII", data_off, len(compressed), len(rebuilt_raw),
        zlib.crc32(rebuilt_raw) & 0xFFFFFFFF)
    tables = deck_table + bytes(card_index) + block_index

    source = hashlib.sha256()
    source.update(deck_table)
    membership = sorted(
        zip(deck_indexes, records),
        key=lambda item: (item[0], json.loads(item[1])["card"]["id"]),
    )
    for deck_index, record in membership:
        source.update(bytes((deck_index,)))
        source.update(struct.pack("<I", len(record)))
        source.update(record)
    source_sha256 = source.digest()
    catalog_id = hashlib.sha256(
        b"KJCAT01\0" + struct.pack("<H", 1) + source_sha256 +
        struct.pack("<Q", seed)
    ).digest()[:16]

    used_size = data_off + len(compressed)
    struct.pack_into("<I", header, 16, used_size)
    struct.pack_into("<I", header, 60, len(compressed))
    header[72:88] = catalog_id
    header[88:120] = source_sha256
    struct.pack_into("<I", header, 124, zlib.crc32(tables) & 0xFFFFFFFF)
    struct.pack_into("<I", header, 120, zlib.crc32(header[:120]) & 0xFFFFFFFF)
    return bytes(header) + tables + compressed


def test_noncanonical_json_rejected():
    decks, _cards = load_fixture(FIXTURE)
    canonical = encode_catalog(decks, balanced_cards(decks, 0), 0x770000)
    noncanonical = rebuilt_noncanonical_image(canonical)
    T.raises_matching(
        ValueError, "canonical", lambda: verify_catalog(noncanonical),
        "internally consistent leading-whitespace JSON is rejected")


def test_boundaries_and_rejection():
    deck = {"id": "many", "name": "Many", "level": "N5", "deck_type": "kanji"}
    cards = []
    for ordinal in range(65):
        card = {"id": f"c{ordinal:02}", "front": f"字{ordinal}", "reading": "ジ",
                "on_reading": "ジ", "kun_reading": "", "level": "N5",
                "gloss": "글자", "senses": ["글자"], "description": "설명",
                "hook_title": "", "hook_body": "기억", "composition": "",
                "parts": [], "examples": [], "deck_index": 0}
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
    T.raises_matching(
        ValueError, "0x770000", lambda: encode_catalog([deck], ordered, 0x770001),
        "a caller cannot raise the absolute catalog partition ceiling")
    T.raises_matching(
        ValueError, "length", lambda: verify_catalog(image + b"X"),
        "trailing bytes beyond used_size are rejected")
    oversized = bytearray(image)
    data_off = struct.unpack_from("<I", oversized, 56)[0]
    struct.pack_into("<I", oversized, 16, 0x770001)
    struct.pack_into("<I", oversized, 60, 0x770001 - data_off)
    oversized.extend(bytes(0x770001 - len(oversized)))
    struct.pack_into("<I", oversized, 120,
                     zlib.crc32(oversized[:120]) & 0xFFFFFFFF)
    T.raises_matching(
        ValueError, "0x770000", lambda: verify_catalog(bytes(oversized)),
        "header used_size cannot exceed the absolute partition ceiling")
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
        "examples": [{"text": "字" * 13, "reading": "あ" * 47,
                      "gloss": "" if i == 2 else "한" * 47} for i in range(3)],
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
    T.eq(len(decoded["examples"]), 3, "three examples survive a round trip")
    T.eq([(len(e["text"].encode("utf-8")), len(e["reading"].encode("utf-8")))
          for e in decoded["examples"]], [(39, 141)] * 3,
         "the largest whole-character example text and reading survive intact")
    T.eq(decoded["examples"][2]["gloss"], "",
         "an example with no Korean gloss keeps an empty gloss")
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


def test_partition_layout_and_image_integration():
    partitions = load_partitions(PARTITIONS)
    by_name = {partition["name"]: partition for partition in partitions}
    T.eq(len(by_name), len(partitions), "partition names are unique")
    T.check("catalog" in by_name, "catalog partition is present")
    T.check("study_state" in by_name, "study_state partition is present")
    if "catalog" not in by_name or "study_state" not in by_name:
        return

    catalog = by_name["catalog"]
    state = by_name["study_state"]
    T.eq((catalog["type"], catalog["subtype"]), ("0x40", "0x00"),
         "catalog uses its assigned custom type and subtype")
    T.eq((catalog["offset"], catalog["size"]), (0x810000, 0x770000),
         "catalog occupies the exact read-only flash budget")
    T.eq(catalog["flags"], {"readonly"}, "catalog is read-only")
    T.eq((state["type"], state["subtype"]), ("0x41", "0x00"),
         "study state uses its assigned custom type and subtype")
    T.eq((state["offset"], state["size"]), (0xF80000, 0x080000),
         "study state occupies the final 512 KiB")
    T.eq(state["flags"], set(), "study state remains writable")

    ordered = sorted(partitions, key=lambda partition: partition["offset"])
    T.check(all(partition["offset"] % 0x1000 == 0 and
                partition["size"] % 0x1000 == 0 for partition in ordered),
            "every partition offset and size is 4 KiB aligned")
    T.check(all(left["offset"] + left["size"] <= right["offset"]
                for left, right in zip(ordered, ordered[1:])),
            "partitions do not overlap")
    T.eq(ordered[-1]["offset"] + ordered[-1]["size"], 0x1000000,
         "the partition table ends exactly at 16 MiB")

    with tempfile.TemporaryDirectory() as tmp:
        output = os.path.join(tmp, "catalog.bin")
        result = subprocess.run(
            [sys.executable, GENERATOR, "--fixture-json", FIXTURE,
             "--output", output, "--partition-size", "0x770000",
             "--seed", "0", "--verify"],
            check=False, text=True, capture_output=True)
        T.eq(result.returncode, 0, f"partition-sized fixture generation succeeds: {result.stderr}")
        if result.returncode == 0:
            T.check(os.path.getsize(output) <= catalog["size"],
                    "generated catalog image fits its partition")


def _live_maxima(cards, manifest):
    scalar_fields = (
        "id", "front", "reading", "on_reading", "kun_reading", "level",
        "gloss", "description", "hook_title", "hook_body", "composition",
    )
    maxima = {
        f"{field}_bytes": max(len(card[field].encode("utf-8")) for card in cards)
        for field in scalar_fields
    }
    maxima.update({
        "senses": max(len(card["senses"]) for card in cards),
        "sense_bytes": max(len(sense.encode("utf-8"))
                           for card in cards for sense in card["senses"]),
        "examples": max(len(card["examples"]) for card in cards),
        # default=0 rather than a bare max(): a catalog whose cards all lack
        # examples is a legitimate (if useless) catalog, and it must report
        # zero rather than raise from inside the maxima line of a sweep.
        "example_text_bytes": max((len(example["text"].encode("utf-8"))
                                   for card in cards
                                   for example in card["examples"]), default=0),
        "example_reading_bytes": max((len(example["reading"].encode("utf-8"))
                                      for card in cards
                                      for example in card["examples"]), default=0),
        "example_gloss_bytes": max((len(example["gloss"].encode("utf-8"))
                                    for card in cards
                                    for example in card["examples"]), default=0),
        "parts": max(len(card["parts"]) for card in cards),
        "part_glyph_bytes": max(len(part["glyph"].encode("utf-8"))
                                for card in cards for part in card["parts"]),
        "part_meaning_bytes": max(len(part["meaning"].encode("utf-8"))
                                  for card in cards for part in card["parts"]),
        "part_reading_bytes": max(len(part["reading"].encode("utf-8"))
                                  for card in cards for part in card["parts"]),
        "raw_block_bytes": manifest.max_raw_block,
        "compressed_block_bytes": manifest.max_compressed_block,
    })
    return maxima


def run_live_database_sweep(database):
    path = os.path.abspath(database)
    connection = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        connection.execute("PRAGMA query_only=ON")
        connection.execute("BEGIN")
        user_id = select_user(connection)
        decks, source_cards = load_source(connection, user_id)
        formula_oracle = _load_live_formula_oracle(connection, user_id)
        gloss_oracle = _load_live_gloss_oracle(connection, user_id)
        example_oracle = _load_live_example_oracle(connection, user_id)
    finally:
        connection.close()

    unique_ids = {card["id"] for card in source_cards}
    T.eq(len(decks), 10, "live source has exactly ten decks")
    T.eq(len(source_cards), 9956, "live source has exactly 9,956 cards")
    T.eq(len(unique_ids), 9956, "live source card ids are unique")
    if len(decks) != 10 or len(source_cards) != 9956 or len(unique_ids) != 9956:
        return

    ordered = balanced_cards(decks, 0)
    image = encode_catalog(decks, ordered, 0x770000)
    manifest = verify_catalog(image)
    T.eq((manifest.deck_count, manifest.card_count, manifest.block_count),
         (10, 9956, 156), "live decoded catalog counts are exact")

    decoded_cards = [envelope["card"] for envelope in manifest.cards]
    for ordinal, (source_card, decoded) in enumerate(zip(ordered, decoded_cards)):
        expected = {key: source_card[key] for key in (
            "id", "front", "reading", "on_reading", "kun_reading", "level",
            "gloss", "senses", "description", "hook_title", "hook_body",
            "composition", "parts", "examples",
        )}
        if not expected["level"]:
            expected["level"] = decks[source_card["deck_index"]]["level"]
        T.eq(decoded, expected, f"live JSON envelope {ordinal} round-trips exactly")

    formula_checks, formula_nonempty, formula_empty = (
        _validate_independent_formulas(decoded_cards, formula_oracle))
    T.eq((formula_checks, formula_nonempty, formula_empty), (9956, 9196, 760),
         "raw DB oracle checks every live formula and its exact occupancy")
    gloss_checks, gloss_nonempty = _validate_independent_glosses(
        decoded_cards, gloss_oracle)
    T.eq((gloss_checks, gloss_nonempty), (9956, 9956),
         "every live decoded card has its raw explicit-or-first-sense gloss")
    example_checks, example_nonempty, example_rows = (
        _validate_independent_examples(decoded_cards, example_oracle))
    T.eq((example_checks, example_nonempty, example_rows), (9956, 8431, 24627),
         "raw DB oracle checks every live 예문 list and its exact occupancy")
    # 24,627 example sentences went into the image in this change and not one
    # of them may be a tofu box burned into read-only flash.
    undrawable = undrawable_fields(decoded_cards, symbol_set())
    T.eq(example_glyph_failures(decoded_cards, symbol_set())[:3], [],
         "no live packed example string is undrawable in flash")

    mutated = copy.deepcopy(ordered)
    for card in mutated:
        card["composition"] = "BROKEN"
        card["gloss"] = ""
        card["examples"] = []
    mutated_manifest = verify_catalog(encode_catalog(decks, mutated, 0x770000))
    mutated_cards = [envelope["card"] for envelope in mutated_manifest.cards]
    T.raises_matching(
        ValueError, "formula mismatch",
        lambda: _validate_independent_formulas(mutated_cards, formula_oracle),
        "raw DB oracle rejects a production-output mutation to every formula")
    T.raises_matching(
        ValueError, "gloss mismatch",
        lambda: _validate_independent_glosses(mutated_cards, gloss_oracle),
        "raw DB oracle rejects a production-output mutation to every gloss")
    T.raises_matching(
        ValueError, "example mismatch",
        lambda: _validate_independent_examples(mutated_cards, example_oracle),
        "raw DB oracle rejects a catalog that silently drops every 예문")

    deck_counts = ",".join(
        f"{deck['type']}:{deck['level']}={deck['card_count']}"
        for deck in manifest.decks)
    maxima = json.dumps(_live_maxima(decoded_cards, manifest),
                        ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    print(f"LIVE: decks={manifest.deck_count} cards={manifest.card_count} "
          f"blocks={manifest.block_count} bytes={manifest.used_size} "
          f"formula_checks={formula_checks} formula_nonempty={formula_nonempty} "
          f"formula_empty={formula_empty} gloss_checks={gloss_checks} "
          f"gloss_nonempty={gloss_nonempty} example_cards={example_nonempty} "
          f"example_rows={example_rows} deck_counts={deck_counts} maxima={maxima}")
    # Informational, like the wire sweep's substitution count: which characters
    # the shipped faces cannot draw is a property of the FONT, and gen_fonts.py
    # owns that. It is printed rather than asserted because the count is not
    # zero today — this catalog packs its 설명 prose verbatim, with no
    # equivalent of the proxy's substitute_missing() pass — and every one of
    # these is a tofu box a learner will eventually see.
    distinct = sorted({character for _id, _label, character in undrawable})
    fields = sorted({label for _id, label, _character in undrawable})
    print(f"  ({len(undrawable)} undrawable placements over {len(distinct)} "
          f"distinct characters in fields {','.join(fields)}: "
          f"{''.join(distinct)})")


def test_live_database_sweep():
    database = os.environ.get("KANJIS_DB")
    if not database:
        print("SKIP: live catalog decoder sweep (set KANJIS_DB)")
        return
    run_live_database_sweep(database)


def test_flash_interval_mutations():
    with tempfile.TemporaryDirectory() as tmp:
        app_payload = os.path.join(tmp, "app.bin")
        with open(app_payload, "wb") as output:
            output.write(bytes(0x1000))
        state_payload = os.path.join(tmp, "rogue-state.bin")
        with open(state_payload, "wb") as output:
            output.write(bytes(0x1000))
        state_args = os.path.join(tmp, "state-flash_args")
        with open(state_args, "w", encoding="utf-8") as output:
            output.write("--flash_mode dio\n0xF81000 rogue-state.bin\n")
        state_entries = _parse_flash_payloads(state_args, tmp)
        T.eq([(entry["offset"], entry["end"]) for entry in state_entries],
             [(0xF81000, 0xF82000)],
             "flash parser stats a mid-state payload as a checked interval")
        T.raises_matching(
            ValueError, "study_state",
            lambda: _assert_flash_intervals_avoid(
                state_entries, ((0xF80000, 0x1000000, "study_state"),),
                "normal flash"),
            "mid-partition state payload mutation is rejected")

        catalog_payload = os.path.join(tmp, "rogue-catalog.bin")
        with open(catalog_payload, "wb") as output:
            output.write(bytes(0x2000))
        catalog_args = os.path.join(tmp, "catalog-flash_args")
        with open(catalog_args, "w", encoding="utf-8") as output:
            output.write("--flash_mode dio\n0x820000 rogue-catalog.bin\n")
        catalog_entries = _parse_flash_payloads(catalog_args, tmp)
        T.eq([(entry["offset"], entry["end"]) for entry in catalog_entries],
             [(0x820000, 0x822000)],
             "flash parser stats a mid-catalog payload as a checked interval")
        T.raises_matching(
            ValueError, "catalog",
            lambda: _assert_flash_intervals_avoid(
                catalog_entries, ((0x810000, 0xF80000, "catalog"),),
                "app-flash"),
            "mid-partition catalog payload mutation is rejected for app-flash")

        same_line_state_args = os.path.join(tmp, "same-line-state-flash_args")
        with open(same_line_state_args, "w", encoding="utf-8") as output:
            output.write(
                "--flash_mode dio --flash_freq 80m --flash_size 16MB "
                "0xF81000 rogue-state.bin\n"
                "0x10000 app.bin\n")
        T.raises_matching(
            ValueError, "study_state",
            lambda: _assert_flash_intervals_avoid(
                _parse_flash_payloads(same_line_state_args, tmp),
                ((0xF80000, 0x1000000, "study_state"),), "normal flash"),
            "normal flash cannot hide a state payload after same-line options")

        same_line_catalog_args = os.path.join(tmp, "same-line-catalog-flash_args")
        with open(same_line_catalog_args, "w", encoding="utf-8") as output:
            output.write(
                "--flash_mode dio --flash_freq 80m --flash_size 16MB "
                "0x820000 rogue-catalog.bin\n"
                "0x10000 app.bin\n")
        T.raises_matching(
            ValueError, "catalog",
            lambda: _assert_flash_intervals_avoid(
                _parse_flash_payloads(same_line_catalog_args, tmp),
                ((0x810000, 0xF80000, "catalog"),), "app-flash"),
            "app-flash cannot hide a catalog payload after same-line options")

        encrypted_args = os.path.join(tmp, "encrypted-flash_args")
        with open(encrypted_args, "w", encoding="utf-8") as output:
            output.write(
                "--flash_mode dio\n"
                "0x10000 app.bin\n"
                "--encrypt-files 0xF81000 rogue-state.bin\n")
        T.raises_matching(
            ValueError, "--encrypt-files",
            lambda: _parse_flash_payloads(encrypted_args, tmp),
            "unsupported encrypted payload groups are rejected, never skipped")

        unknown_args = os.path.join(tmp, "unknown-option-flash_args")
        with open(unknown_args, "w", encoding="utf-8") as output:
            output.write(
                "--flash_mode dio\n"
                "0x10000 app.bin\n"
                "--unexpected-option 0xF81000 rogue-state.bin\n")
        T.raises_matching(
            ValueError, "unknown flash option",
            lambda: _parse_flash_payloads(unknown_args, tmp),
            "unknown option arity is rejected instead of guessed or skipped")

        erase_all_args = os.path.join(tmp, "erase-all-flash_args")
        with open(erase_all_args, "w", encoding="utf-8") as output:
            output.write("--flash_mode dio --erase-all\n0x10000 app.bin\n")
        T.raises_matching(
            ValueError, "--erase-all",
            lambda: _parse_flash_payloads(erase_all_args, tmp),
            "whole-flash erase cannot masquerade as state-preserving flash")

        malformed_args = os.path.join(tmp, "malformed-trailing-flash_args")
        with open(malformed_args, "w", encoding="utf-8") as output:
            output.write(
                "--flash_mode dio 0x10000 app.bin unpaired-trailing-token\n")
        T.raises_matching(
            ValueError, "address/file pairs",
            lambda: _parse_flash_payloads(malformed_args, tmp),
            "an unpaired trailing token is rejected instead of discarded")


def assert_idf_flash_metadata(build_directory):
    build_directory = os.path.abspath(build_directory)
    normal_payloads = _parse_flash_payloads(
        os.path.join(build_directory, "flash_args"), build_directory)
    app_payloads = _parse_flash_payloads(
        os.path.join(build_directory, "app-flash_args"), build_directory)
    _assert_flash_payloads_disjoint(normal_payloads, "normal flash")
    _assert_flash_payloads_disjoint(app_payloads, "app-flash")
    _assert_flash_intervals_avoid(
        normal_payloads, ((0xF80000, 0x1000000, "study_state"),),
        "normal flash")
    _assert_flash_intervals_avoid(
        app_payloads,
        ((0x810000, 0xF80000, "catalog"),
         (0xF80000, 0x1000000, "study_state")),
        "app-flash")

    expected_normal = {
        0x0: "bootloader/bootloader.bin",
        0x8000: "partition_table/partition-table.bin",
        0x10000: "ankireader.bin",
        0x810000: "kanji-catalog.bin",
    }
    expected_app = {0x10000: "ankireader.bin"}
    normal_map = {payload["offset"]: payload["file"] for payload in normal_payloads}
    app_map = {payload["offset"]: payload["file"] for payload in app_payloads}
    T.eq(normal_map, expected_normal,
         "normal flash has only bootloader, partition table, app, and catalog")
    T.eq(app_map, expected_app, "app-flash has only the application payload")
    if normal_map != expected_normal or app_map != expected_app:
        return

    normal_by_offset = {payload["offset"]: payload for payload in normal_payloads}
    app_by_offset = {payload["offset"]: payload for payload in app_payloads}
    _assert_flash_payload_fits(
        normal_by_offset[0x0], 0x0, 0x8000, "bootloader region", "normal flash")
    _assert_flash_payload_fits(
        normal_by_offset[0x8000], 0x8000, 0x9000, "partition-table region",
        "normal flash")
    _assert_flash_payload_fits(
        normal_by_offset[0x10000], 0x10000, 0x810000, "factory app partition",
        "normal flash")
    _assert_flash_payload_fits(
        normal_by_offset[0x810000], 0x810000, 0xF80000, "catalog partition",
        "normal flash")
    _assert_flash_payload_fits(
        app_by_offset[0x10000], 0x10000, 0x810000, "factory app partition",
        "app-flash")

    metadata_path = os.path.join(build_directory, "flasher_args.json")
    with open(metadata_path, encoding="utf-8") as source:
        metadata = json.load(source)
    flash_files = metadata.get("flash_files")
    T.check(isinstance(flash_files, dict), "ESP-IDF flash metadata has a file map")
    if not isinstance(flash_files, dict):
        return

    try:
        metadata_map = {
            int(offset, 0): os.path.normpath(filename)
            for offset, filename in flash_files.items()
        }
    except (TypeError, ValueError):
        metadata_map = None
    T.eq(metadata_map, expected_normal,
         "flasher JSON matches every checked normal flash interval")
    T.eq(metadata.get("catalog"), {
        "offset": "0x810000", "file": "kanji-catalog.bin", "encrypted": "false",
    }, "catalog flash metadata is exact")

    app_size = normal_by_offset[0x10000]["size"]
    catalog_size = normal_by_offset[0x810000]["size"]
    T.check(app_size <= 0x780000,
            f"application 0x{app_size:x} leaves at least 512 KiB in its slot")
    T.check(catalog_size <= 0x770000,
            f"catalog 0x{catalog_size:x} fits its exact partition")
    T.eq(app_by_offset[0x10000]["size"], app_size,
         "app-flash and normal flash use the same application image")
    print(
        f"IDF flash metadata: app=[0x10000,0x{0x10000 + app_size:x}) "
        f"catalog=[0x810000,0x{0x810000 + catalog_size:x}) "
        "state=[0xf80000,0x1000000) untouched; "
        "app-flash=[0x10000,app-end) only")


def test_idf_flash_metadata():
    build_directory = os.environ.get("IDF_BUILD_DIR")
    if not build_directory:
        print("SKIP: ESP-IDF flash metadata check (set IDF_BUILD_DIR)")
        return
    assert_idf_flash_metadata(build_directory)


def main():
    test_source_selection()
    test_balanced_order()
    test_fixture_projection_and_format()
    test_independent_formula_oracle_rules()
    test_examples_are_packed_in_wire_order()
    test_example_order_is_deterministic()
    test_example_caps_truncate_on_a_utf8_boundary()
    test_oversized_example_is_rejected_not_clipped()
    test_packed_examples_pass_the_glyph_gate()
    test_independent_example_oracle_rules()
    test_noncanonical_json_rejected()
    test_boundaries_and_rejection()
    test_full_model_maxima_round_trip()
    test_cli_atomic_fixture_write()
    test_partition_layout_and_image_integration()
    test_live_database_sweep()
    test_flash_interval_mutations()
    test_idf_flash_metadata()
    if T.failures:
        print(f"{T.count} checks, {T.failures} failures", file=sys.stderr)
        return 1
    print(f"ok: {T.count} checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
