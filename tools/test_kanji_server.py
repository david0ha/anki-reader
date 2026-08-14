#!/usr/bin/env python3
"""
Tests for tools/kanji_server.py — the pure half, with no network anywhere.

    python3 tools/test_kanji_server.py

No framework and no account. What is testable without api.kanjis.ai is exactly
what is worth testing, because it is where this proxy can be quietly wrong:

  * the Korean span. It is a port of kanjis-front's relativeDue(), and a port
    that disagrees at a boundary means the board and the browser word the same
    schedule differently for the same card. Driven here with the thresholds and
    the values either side of them, including the halves where JavaScript's
    Math.round and Python's round() genuinely differ.
  * the back/hint flattening. Fed REAL rows out of the backend's sqlite —
    9,956 catalog cards, whose content is nine thousand chances to hit a shape
    the code did not expect. The assertion is that it never throws and never
    exceeds a device cap, because both of those failures land on the glass.
  * the grade word. This is the only thing standing between an unauthenticated
    LAN GET and somebody's review history.
  * the glyph gate. A character the shipped faces cannot draw must be
    substituted here, not discovered as a tofu box after a two-second refresh.
  * the byte budgets. Every cap asserted here is read out of kanji_model.h, the
    header that declares the buffers the parser copies into — never imported
    from the module under test. An earlier version of this file imported
    BODY_MAX from kanji_server and checked the proxy against it, which is a
    tautology: the cap was 319 against a 480-byte buffer and every assertion
    agreed, while 6,352 of the catalog's 9,956 cards lost text the panel had
    the room to draw.

The real catalog lives outside this repository, so the sqlite sweep is skipped
rather than failed when it is not there — the other checks still run. Point it
somewhere else with KANJIS_DB.
"""

import json
import os
import re
import shutil
import sqlite3
import sys
import tempfile
import unicodedata
from datetime import datetime, timedelta, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kanji_server  # noqa: E402
from gen_fonts import symbol_set  # noqa: E402
from kanji_server import (  # noqa: E402
    DEFAULT_FIXTURE, GRADE_WORDS, ConfigError, ConflictError, FixtureProxy,
    StudyProxy, card_examples, card_fsrs, card_parts, card_senses,
    check_glyphs, clip, difficulty_pct, flatten_card, flatten_comments,
    flatten_session, jlpt_level, js_round, load_credentials, normalize_text,
    parse_grade, parse_json_column, primary_reading, relative_due,
    project_card_content, raw_card_parts, safe_composition, substitute_missing,
)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL_H = os.path.join(ROOT, "components", "vault_core", "include", "kanji_model.h")

DEFAULT_DB = "/Users/ggrrm/Documents/kanjis-backend/data/kanjis-backend.sqlite3"
SAMPLE_CARDS = int(os.environ.get("KANJIS_SAMPLE", "400"))


def header_caps():
    """The `#define KANJI_* <n>` values, straight out of the header.

    Deliberately a second, independent reader rather than a call into
    kanji_server.device_caps(): the point of every cap assertion below is that
    the proxy's numbers are checked against the struct, and a test that asks the
    code under test what the answer should be cannot fail.
    """
    with open(MODEL_H, encoding="utf-8") as source:
        src = source.read()
    return {name: int(value)
            for name, value in re.findall(r"#define\s+(KANJI_\w+)\s+(\d+)", src)}


CAP = header_caps()

# The header states the buffer; a cap is what fits in it once the NUL has its
# byte. The four row counts are counts, so they are the header's number as is.
FRONT_MAX = CAP["KANJI_FRONT_MAX"] - 1
READING_MAX = CAP["KANJI_READING_MAX"] - 1
SENSE_MAX = CAP["KANJI_SENSE_MAX"] - 1
LABEL_MAX = CAP["KANJI_LABEL_MAX"] - 1
DECK_MAX = CAP["KANJI_DECK_MAX"] - 1
ID_MAX = CAP["KANJI_ID_MAX"] - 1
BODY_MAX = CAP["KANJI_BODY_MAX"] - 1
FORMULA_MAX = CAP["KANJI_FORMULA_MAX"] - 1
AUTHOR_MAX = CAP["KANJI_AUTHOR_MAX"] - 1
COMMENT_MAX = CAP["KANJI_COMMENT_MAX"] - 1

SENSES_MAX = CAP["KANJI_SENSES_MAX"]
EXAMPLES_MAX = CAP["KANJI_EXAMPLES_MAX"]
PARTS_MAX = CAP["KANJI_PARTS_MAX"]
COMMENTS_MAX = CAP["KANJI_COMMENTS_MAX"]

FAILURES = []
CHECKS = [0]

# A fixed instant, so every span in this file is arithmetic rather than a race.
NOW = datetime(2026, 8, 14, 12, 0, 0, tzinfo=timezone.utc)


def check(cond, label, got=None, want=None):
    CHECKS[0] += 1
    if not cond:
        detail = "" if got is None and want is None else f"  (got {got!r}, want {want!r})"
        FAILURES.append(label + detail)
        print(f"  FAIL {label}{detail}")


def eq(got, want, label):
    check(got == want, label, got, want)


def due_in(seconds):
    """An ISO timestamp exactly `seconds` from NOW, as the backend writes it."""
    return (NOW + timedelta(seconds=seconds)).isoformat().replace("+00:00", "Z")


# ---------------------------------------------------------------------------
# One real card, kept here verbatim
# ---------------------------------------------------------------------------
#
# card_templates row 4915fbc8 (会), copied out of the backend's sqlite. A
# literal rather than a query, so the projection has a fixed answer even on a
# machine with no catalog — and so a change in this file's expectations is
# visible in the diff next to the data that produced them.

CARD_BACK = json.dumps({
    "kanji": "会",
    "meaning": {"gloss": "모일 회", "senses": ["모이다", "만나다"]},
    "on_yomi": [
        {"reading": "カイ", "kanji": None, "detail": "주로 사용되는 음독",
         "examples": [
             {"text": "会社", "reading": "かいしゃ", "gloss": "회사"},
             {"text": "大会", "reading": "たいかい", "gloss": "대회"},
         ]},
        {"reading": "エ", "kanji": None, "detail": None,
         "examples": [
             {"text": "会得", "reading": "えとく", "gloss": "터득"},
             {"text": "法会", "reading": "ほうえ", "gloss": "법회"},
         ]},
    ],
    "kun_yomi": [
        {"reading": "あう", "kanji": "会う", "detail": "자동사, '만나다' 의미",
         "examples": [
             {"text": "カフェで友達に会った", "reading": "カフェでともだちにあった",
              "gloss": "카페에서 친구와 만났다"},
         ]},
    ],
    "shape_explanation": "会 = 人 + 云。 会는 하늘의 구름이 만나다 라는 뜻의 글자입니다.",
    "reading_notes": [],
}, ensure_ascii=False)

CARD_HINT = json.dumps({
    "reason": "하늘(人)의 구름(云)이 만나 모이는 모습에서 '모이다·만나다'의 뜻을 이룸",
    "shapes": [
        {"kanji": "人", "meaning": "하늘", "reading": "ジン (on)"},
        {"kanji": "云", "meaning": "구름", "reading": "ウン (on)"},
    ],
    "principle": "회의",
}, ensure_ascii=False)

STUDY_CARD = {
    "id": "f00c539e-23f9-4294-bee1-c642189b105f",
    "study_deck_id": "deck-n5",
    "front": "会",
    "back": CARD_BACK,
    "hint": CARD_HINT,
    "tags": ["N5", "JLPT"],
    "sort_order": 2,
    "state": {
        "state": "review",
        "step": 0,
        "stability": 8.6,
        "difficulty": 5.23,
        "due_at": None,
        "last_review_at": None,
        "total_reviews": 5,
        "lapses": 1,
    },
}


# ---------------------------------------------------------------------------
# The Korean span
# ---------------------------------------------------------------------------

def test_js_round_is_not_pythons_round():
    """The port hinges on this: JavaScript rounds a half up, Python to even."""
    eq(js_round(0.5), 1, "Math.round(0.5) is 1, where Python's round() is 0")
    eq(js_round(2.5), 3, "Math.round(2.5) is 3, where Python's round() is 2")
    eq(js_round(-0.5), 0, "Math.round(-0.5) is 0, toward +infinity")
    eq(js_round(1.4999), 1, "and it is still a round, not a ceiling")


def test_relative_due_thresholds():
    """Every boundary in the contract's table, and the second either side."""
    eq(relative_due(due_in(0), NOW), "곧", "due now reads 곧")
    eq(relative_due(due_in(44), NOW), "곧", "44 seconds is still 곧")
    eq(relative_due(due_in(-99999), NOW), "곧", "an overdue card reads 곧, never negative")
    eq(relative_due(due_in(45), NOW), "1분 뒤", "45 seconds crosses into minutes")

    eq(relative_due(due_in(3599), NOW), "60분 뒤",
       "the last second under an hour still rounds to 60 minutes — the web says so too")
    eq(relative_due(due_in(3600), NOW), "1시간 뒤", "an hour crosses into hours")
    eq(relative_due(due_in(86399), NOW), "24시간 뒤", "the last second under a day")
    eq(relative_due(due_in(86400), NOW), "1일 뒤", "a day crosses into days")
    eq(relative_due(due_in(30 * 86400 - 1), NOW), "30일 뒤", "the last second under a month")
    eq(relative_due(due_in(30 * 86400), NOW), "1개월 뒤", "thirty days crosses into months")
    eq(relative_due(due_in(365 * 86400 - 1), NOW), "12개월 뒤", "the last second under a year")
    eq(relative_due(due_in(365 * 86400), NOW), "1년 뒤", "a year crosses into years")

    # The FSRS learning step the backend actually ships (FSRS_LEARNING_STEPS
    # = "10m"), which is the span the AGAIN button shows on nearly every card.
    eq(relative_due(due_in(600), NOW), "10분 뒤", "the 10m learning step reads 10분 뒤")


def test_relative_due_rounds_the_javascript_way():
    """The halves. These are where a naive port silently disagrees with the web."""
    eq(relative_due(due_in(150), NOW), "3분 뒤",
       "2.5 minutes rounds up, as Math.round does (Python's round() gives 2)")
    eq(relative_due(due_in(9000), NOW), "3시간 뒤",
       "2.5 hours rounds up too")
    eq(relative_due(due_in(216000), NOW), "3일 뒤",
       "2.5 days rounds up as well")
    # The seconds themselves are rounded before the comparison, so a span half a
    # second under the threshold is over it.
    fractional = (NOW + timedelta(seconds=44.5)).isoformat().replace("+00:00", "Z")
    eq(relative_due(fractional, NOW), "1분 뒤",
       "44.5 seconds rounds to 45 and leaves 곧 behind")


def test_relative_due_non_dates():
    eq(relative_due("", NOW), "", "no timestamp is the empty span")
    eq(relative_due(None, NOW), "", "and so is a null one")
    eq(relative_due("  없음  ", NOW), "없음",
       "a value that is not a date comes back trimmed, as relativeDue() returns it")
    eq(relative_due("2026-13-45T00:00:00Z", NOW), "2026-13-45T00:00:00Z",
       "an impossible date is not a date")


def test_relative_due_treats_a_bare_date_as_utc():
    """Date.parse's own convention, kept because this is a port.

    A date-only string is UTC in ECMA-262; a date-time without an offset is
    local. The backend always sends `...Z`, so this exists to make sure a card
    whose timestamp is some other shape lands where the web would put it.
    """
    noon = datetime(2026, 8, 14, 0, 0, 0, tzinfo=timezone.utc)
    eq(relative_due("2026-08-15", noon), "1일 뒤", "a bare date is midnight UTC")


# ---------------------------------------------------------------------------
# Projecting a card
# ---------------------------------------------------------------------------

def test_parse_json_column():
    eq(parse_json_column(CARD_HINT)["principle"], "회의", "the hint column parses")
    eq(parse_json_column("{ not json"), {}, "a malformed column is empty, not fatal")
    eq(parse_json_column(None), {}, "a null column is empty")
    eq(parse_json_column("[1,2]"), {}, "a non-object column is empty")
    eq(parse_json_column({"already": 1}), {"already": 1}, "an already-parsed column passes through")


def test_primary_reading_mirrors_the_web():
    back = parse_json_column(CARD_BACK)
    on = back["on_yomi"]
    kun = back["kun_yomi"]
    eq(primary_reading(on, kun), "カイ・エ",
       "on-yomi leads and the readings are ・-joined, as primaryReading() does")
    eq(primary_reading([], kun), "あう", "a kun-only word falls back to kun-yomi")
    eq(primary_reading([{"reading": "カイ"}, {"reading": "カイ"}], []), "カイ",
       "a repeated reading is one reading")
    eq(primary_reading([{"reading": ""}, {"detail": "x"}], []), "",
       "an entry with no reading contributes nothing")


def test_card_examples_walk_on_then_kun():
    back = parse_json_column(CARD_BACK)
    examples = card_examples(back["on_yomi"], back["kun_yomi"])
    eq(len(examples), EXAMPLES_MAX, "three rows are filled when the card has three")
    eq(examples[0], {"text": "会社", "reading": "かいしゃ", "gloss": "회사"},
       "the first row is the same example firstExample() picks for the web")
    eq([e["text"] for e in examples], ["会社", "大会", "会得"],
       "the walk is on-yomi in order, then kun-yomi")
    eq(card_examples([], []), [], "a card with no examples gets no rows")


def test_card_senses_and_level():
    back = parse_json_column(CARD_BACK)
    eq(card_senses(back), ["모이다", "만나다"], "senses are back.meaning.senses, in order")
    eq(card_senses({"meaning": {"gloss": "모일 회", "senses": []}}), ["모일 회"],
       "a card with no senses falls back to the gloss rather than showing nothing")
    eq(card_senses({}), [], "a card with no meaning at all gets no senses")
    eq(len(card_senses({"meaning": {"senses": ["a", "b", "c", "d", "e"]}})), SENSES_MAX,
       "senses stop at the three rows the panel has")

    eq(jlpt_level(["N5", "JLPT"]), "N5", "the JLPT tag becomes the level chip")
    eq(jlpt_level(["JLPT", "N2"]), "N2", "wherever it sits in the list")
    eq(jlpt_level(["kanji"]), "", "a card with no JLPT tag has no level")
    eq(jlpt_level(None), "", "and neither does one with no tags")


def test_card_parts_tolerate_a_missing_reading():
    parts = card_parts(parse_json_column(CARD_HINT))
    eq(len(parts), 2, "both components become rows")
    eq(parts[0], {"glyph": "人", "meaning": "하늘", "reading": "ジン (on)"},
       "a component is glyph/meaning/reading")
    # ~1.2k catalog rows predate the required `reading` (the backend's own
    # audit calls it legacy_shape_without_reading); the web tolerates it and so
    # does this, because a component with no reading is still worth drawing.
    legacy = card_parts({"shapes": [{"kanji": "水", "meaning": "물"}]})
    eq(legacy, [{"glyph": "水", "meaning": "물", "reading": ""}],
       "a legacy component with no reading is kept, not dropped")
    eq(card_parts({"shapes": [{"meaning": "no glyph"}]}), [],
       "a component with no glyph is not a component")
    eq(len(card_parts({"shapes": [{"kanji": "一", "meaning": "m"}] * 9})), PARTS_MAX,
       "components stop at the three rows the sheet has")


def test_safe_composition_filters_only_structural_self_references():
    raw_parts = [
        {"glyph": "財", "meaning": "wealth", "reading": "ザイ"},
        {"glyph": "貝", "meaning": "money", "reading": "カイ"},
        {"glyph": "才", "meaning": "talent", "reading": "サイ"},
    ]
    compound_parts = [
        {"glyph": "勉", "meaning": "exert", "reading": "ベン"},
        {"glyph": "免", "meaning": "excuse", "reading": "メン"},
        {"glyph": "力", "meaning": "power", "reading": "リョク"},
        {"glyph": "強", "meaning": "strong", "reading": "キョウ"},
        {"glyph": "弓", "meaning": "bow", "reading": "キュウ"},
        {"glyph": "虫", "meaning": "insect", "reading": "チュウ"},
    ]
    okurigana_parts = [
        {"glyph": "懲", "meaning": "punish", "reading": "チョウ"},
        {"glyph": "徴", "meaning": "sign", "reading": "チョウ"},
        {"glyph": "心", "meaning": "heart", "reading": "シン"},
    ]
    eq(safe_composition("財", "財", raw_parts),
       ("貝 + 才 = 財", [raw_parts[1], raw_parts[2]]),
       "single-kanji self reference is removed")
    eq(safe_composition("勉強", "勉強", compound_parts)[0],
       "勉 + 強 = 勉強", "compound sub-radicals are removed")
    eq(safe_composition("懲らしめる", "懲", okurigana_parts)[0],
       "徴 + 心 = 懲", "okurigana result is the constituent kanji")


def test_project_card_content_retains_the_full_catalog_record():
    description = "d" * 819
    mnemonic = "m" * 615
    source = {
        "id": "wealth", "front": "財", "tags": ["N2"],
        "back": json.dumps({
            "kanji": "財", "meaning": {"gloss": "재물 재",
                                       "senses": ["재물", "재산", "부", "자산", "보물"]},
            "on_yomi": [{"reading": "ザイ・サイ"}],
            "kun_yomi": [{"reading": "たから"}],
            "shape_explanation": description,
            "image": "never-project.png",
            "examples": [{"text": "never project"}],
        }, ensure_ascii=False),
        "hint": json.dumps({
            "principle": "형성", "reason": mnemonic,
            "shapes": [
                {"kanji": "財", "meaning": "wealth", "reading": "ザイ"},
                {"kanji": "貝", "meaning": "money", "reading": "カイ"},
                {"kanji": "才", "meaning": "talent", "reading": "サイ"},
                {"kanji": "王", "meaning": "king", "reading": "オウ"},
                {"kanji": "口", "meaning": "mouth", "reading": "コウ"},
                {"kanji": "力", "meaning": "power"},
            ],
            "comment": "never project", "fsrs": {"state": "review"},
        }, ensure_ascii=False),
        "comments": [{"body": "never project"}], "session": {"id": "never"},
        "state": {"state": "review"},
    }
    content = project_card_content(source)
    eq(len(content["description"].encode("utf-8")), 819,
       "full shape explanation is not display-clipped")
    eq(len(content["hook_body"].encode("utf-8")), 615,
       "full mnemonic is not display-clipped")
    eq(content["senses"], ["재물", "재산", "부", "자산", "보물"],
       "all five source senses survive")
    eq(len(content["parts"]), 6, "all six source components survive")
    eq(content["parts"][-1]["reading"], "", "missing component reading stays empty")
    eq(content["gloss"], "재물 재", "the source gloss is separate from senses")
    eq(content["on_reading"], "ザイ・サイ", "structured on reading survives")
    eq(content["kun_reading"], "たから", "structured kun reading survives")
    eq(content["hook_title"], "형성", "source principle is preserved")
    eq(content["composition"], "貝 + 才 + 王 + 口 + 力 = 財",
       "the shared projection supplies the safe equation")
    for key in ("image", "comment", "comments", "examples", "session", "fsrs", "state"):
        check(key not in content, f"projection excludes {key}")
    eq(project_card_content(dict(source, hint=json.dumps({})))["hook_title"], "",
       "a missing principle remains empty")


def test_project_card_content_falls_back_to_the_first_valid_sense():
    source = {
        "id": "missing-gloss", "front": "会", "tags": ["N5"],
        "back": json.dumps({
            "kanji": "会",
            "meaning": {
                "gloss": None,
                "senses": [None, "", "   ", "  만나다  ", "대면하다"],
            },
        }, ensure_ascii=False),
        "hint": json.dumps({}, ensure_ascii=False),
    }

    content = project_card_content(source)

    eq(content["gloss"], "만나다",
       "a missing short gloss uses the first nonempty normalized sense")

    whitespace_gloss = dict(source, back=json.dumps({
        "kanji": "会",
        "meaning": {"gloss": " \t ", "senses": ["  만나다  ", "대면하다"]},
    }, ensure_ascii=False))
    eq(project_card_content(whitespace_gloss)["gloss"], "만나다",
       "a whitespace-only gloss is empty and falls back to a normalized sense")
    eq(project_card_content(whitespace_gloss)["senses"], ["만나다", "대면하다"],
       "the projected sense list drops empty rows and surrounding whitespace")

    explicit_gloss = dict(source, back=json.dumps({
        "kanji": "会",
        "meaning": {"gloss": "  모일 회\n", "senses": ["만나다"]},
    }, ensure_ascii=False))
    eq(project_card_content(explicit_gloss)["gloss"], "모일 회",
       "a nonempty explicit gloss wins after surrounding whitespace is removed")


def test_flatten_card_is_the_whole_projection():
    card = flatten_card(STUDY_CARD, now=NOW)
    eq(card["id"], STUDY_CARD["id"], "the id rides along for routing")
    eq(card["front"], "会", "the headword is the hero")
    eq(card["reading"], "カイ・エ", "the reading line is the web's reading line")
    eq(card["level"], "N5", "the level chip")
    eq(card["senses"], ["모이다", "만나다"], "the senses")
    eq(card["hook_title"], "회의", "hint.principle is the hook's title")
    check(card["hook_body"].startswith("하늘(人)"), "hint.reason is the hook's body")
    check(card["description"].startswith("会 = 人 + 云"),
          "back.shape_explanation is the description")
    eq(len(card["parts"]), 2, "the components came through")
    eq(card["comments"], [], "a card fetched without a thread has no comments")
    eq(card["comment_total"], 0, "and no total")

    # front falls back to back.kanji, exactly as cardContent() does.
    headless = dict(STUDY_CARD, front="")
    eq(flatten_card(headless, now=NOW)["front"], "会",
       "an empty front falls back to back.kanji")

    # A card whose content columns are rubbish must still produce a card.
    broken = dict(STUDY_CARD, back="{ not json", hint=None)
    card = flatten_card(broken, now=NOW)
    eq(card["front"], "会", "a card with unparseable content keeps its headword")
    eq(card["senses"], [], "and simply has no senses")
    eq(card["hook_title"], "",
       "a card with no hint keeps the absent principle empty")
    eq(card["hook_body"], "", "with nothing under it")


def test_fsrs_unscheduled_is_minus_one_not_zero():
    """The distinction the panel actually prints: — against 0일.

    A new card has no stability; a card with a same-day interval has one that
    rounds to zero. Sending 0 for the first makes the board claim to know
    something it does not.
    """
    new = card_fsrs({"state": "new", "stability": None, "difficulty": None,
                     "due_at": None, "total_reviews": 0, "lapses": 0}, NOW)
    eq(new["stability_days"], -1, "an unscheduled card has stability -1")
    eq(new["difficulty_pct"], -1, "and difficulty -1")
    eq(new["state_label"], "새 카드", "and the Korean state label the sheet prints")
    eq(new["due"], "", "and no due span at all")

    same_day = card_fsrs({"state": "learning", "stability": 0.4, "difficulty": 1.0,
                          "due_at": due_in(600), "total_reviews": 2, "lapses": 1}, NOW)
    eq(same_day["stability_days"], 0, "a sub-day stability rounds to 0, which is not -1")
    eq(same_day["difficulty_pct"], 0, "and the floor of the difficulty scale is 0%")
    eq(same_day["state_label"], "학습 중", "learning")
    eq(same_day["due"], "10분 뒤", "the due date is already a Korean span")

    review = card_fsrs(STUDY_CARD["state"], NOW)
    eq(review["stability_days"], 9, "8.6 days of stability rounds to 9")
    eq(review["difficulty_pct"], 47, "5.23 on the 1..10 FSRS scale is 47%")
    eq(review["reps"], 5, "reps")
    eq(review["lapses"], 1, "lapses")

    eq(card_fsrs(None, NOW)["state"], "new", "a card with no state block is a new card")
    eq(card_fsrs({"state": "relearning"}, NOW)["state_label"], "다시 학습", "relearning")
    # An unknown state word must not become a blank label — the sheet has a row
    # for it either way, and the wire word says more than nothing.
    eq(card_fsrs({"state": "hibernating"}, NOW)["state_label"], "hibernating",
       "an unrecognised state prints itself rather than nothing")

    eq(difficulty_pct(10.0), 100, "the top of the scale is 100%")
    eq(difficulty_pct(1.0), 0, "the bottom is 0%")
    eq(difficulty_pct(None), -1, "no difficulty is -1")
    eq(difficulty_pct(True), -1, "and a bool is not a difficulty")


def test_preview_is_four_rendered_spans():
    card = flatten_card(STUDY_CARD, {
        "again": due_in(600),
        "hard": due_in(4 * 86400),
        "good": due_in(9 * 86400),
        "easy": due_in(21 * 86400),
    }, now=NOW)
    eq(card["preview"], {"again": "10분 뒤", "hard": "4일 뒤",
                         "good": "9일 뒤", "easy": "21일 뒤"},
       "each rating's next-due timestamp arrives already worded")
    eq(flatten_card(STUDY_CARD, None, now=NOW)["preview"],
       {word: "" for word in GRADE_WORDS},
       "a card with no preview has four empty spans, not four missing keys")


# ---------------------------------------------------------------------------
# Comments
# ---------------------------------------------------------------------------

def test_flatten_comments():
    thread = [
        {"id": "1", "body": "「会う」는 사람을 만날 때 씁니다.", "like_count": 12,
         "persona": {"id": "p1", "name": "카나 선생", "avatar": ""},
         "replies": [
             {"id": "1a", "body": "그럼 会社는요?", "like_count": 3,
              "persona": None, "replies": []},
         ]},
        {"id": "2", "body": "音読み은 カイ 입니다.", "like_count": 5,
         "persona": {"id": "p2", "name": "유리", "avatar": ""}, "replies": []},
        {"id": "3", "body": "네 번째 줄은 화면에 없습니다.", "like_count": 0,
         "persona": None, "replies": []},
    ]
    comments, total = flatten_comments(thread)
    eq(total, 4, "the total counts every node in the tree, replies included")
    eq(len(comments), COMMENTS_MAX, "but only three rows are served")
    eq([c["author"] for c in comments], ["카나 선생", "나", "유리"],
       "depth-first, so a reply sits next to what it replies to")
    eq(comments[0]["likes"], 12, "the like count rides along")
    eq(comments[1]["author"], "나",
       "a comment with no persona is the learner's own")

    empty, total = flatten_comments([{"id": "x", "body": "   ", "replies": []}])
    eq((empty, total), ([], 0), "a blank body is not a comment")
    eq(flatten_comments(None), ([], 0), "no thread is no comments")
    eq(flatten_comments([{"id": "y", "body": "a\n\nb  c", "replies": []}])[0][0]["body"],
       "a b c", "a comment's whitespace is collapsed — the row is one line")


# ---------------------------------------------------------------------------
# The session block
# ---------------------------------------------------------------------------

SESSION = {
    "id": "sess-1", "study_deck_ids": ["deck-n5"], "mode": "mixed",
    "session_date": "2026-08-14", "active": True,
    "target_new": 20, "target_review": 40,
    "left_new": 7, "left_review": 18, "retry_cards": 2,
    "streak": 12, "started_at": "2026-08-14T00:00:00Z", "time_spent_seconds": 0,
}


def test_flatten_session():
    block = flatten_session(SESSION, deck="JLPT N5 Vocabulary", level="N5")
    eq(block["deck"], "JLPT N5 Vocabulary", "the deck is the caption line")
    eq(block["level"], "N5", "the level filter")
    eq(block["streak"], 12, "연속")
    # 20-7 new solved plus 40-18 review solved.
    eq(block["reviewed_today"], 35, "오늘, derived the way the web derives it")
    eq(block["left_new"], 7, "새로 배울")
    eq(block["left_review"], 18, "복습할")
    eq(block["retry"], 2, "다시 볼")
    eq(block["track_total"], 60, "the queue is the two targets")
    # 60 total, 27 still to serve, so this is the 34th card — 1-based, counting
    # the one being served.
    eq(block["track"], 34, "track is a 1-based position in today's queue")
    eq(block["complete"], False, "and the session is not done")

    eq(flatten_session(SESSION)["level"], "전체",
       "no filter is the whole catalog, said as 전체")
    done = flatten_session(dict(SESSION, left_new=0, left_review=0, retry_cards=0),
                           complete=True)
    eq(done["track"], 60, "a finished session sits on its last card, not past it")
    eq(done["complete"], True, "and says so")
    eq(done["reviewed_today"], 60, "having reviewed the whole queue")

    empty = flatten_session({"target_new": 0, "target_review": 0})
    eq((empty["track"], empty["track_total"]), (0, 0),
       "an empty queue has no position, rather than a position of 1 in 0")

    junk = flatten_session({"streak": "twelve", "left_new": None, "target_new": -4})
    eq((junk["streak"], junk["left_new"], junk["track_total"]), (0, 0, 0),
       "a field of the wrong type takes its floor rather than failing the payload")
    eq(flatten_session(dict(SESSION, streak=10 ** 9))["streak"], 9999,
       "and a runaway counter is clamped to what the chip can print")


# ---------------------------------------------------------------------------
# Grades
# ---------------------------------------------------------------------------

def test_parse_grade():
    for word in GRADE_WORDS:
        eq(parse_grade(word), word, f"'{word}' is a grade")
    eq(parse_grade("  GOOD "), "good", "case and spacing are forgiven")
    # The backend's AnswerRequest normalises 1..4 and other aliases. This does
    # not: the board sends kanji_grade_wire()'s word, and this proxy is the only
    # thing between an unauthenticated LAN GET and somebody's review history.
    eq(parse_grade("3"), None, "a number is not a grade here, whatever the API accepts")
    eq(parse_grade("well"), None, "nor is a near-miss")
    eq(parse_grade(""), None, "nor is nothing")
    eq(parse_grade(None), None, "nor is a missing parameter")
    eq(parse_grade("good; drop table"), None, "nor is anything with a grade inside it")


# ---------------------------------------------------------------------------
# Fitting and the glyph gate
# ---------------------------------------------------------------------------

def test_every_cap_is_the_one_the_header_declares():
    """The proxy's byte budgets against kanji_model.h, name by name.

    The header is the only authority: it declares the buffers kanji_str_copy()
    copies into, and a proxy cap below one of them is text the device had the
    room for and never receives. Checked here rather than trusted because the
    table used to be hand-copied, and a hand-copied table drifts silently — the
    device is happy either way, since a short field is a legal field.
    """
    for name in ("FRONT", "READING", "SENSE", "LABEL", "DECK", "ID", "BODY", "FORMULA",
                 "AUTHOR", "COMMENT"):
        eq(getattr(kanji_server, f"{name}_MAX"), CAP[f"KANJI_{name}_MAX"] - 1,
           f"{name}_MAX is KANJI_{name}_MAX with the NUL taken off")
    # Row counts, not buffers: there is no NUL in a count of senses.
    for name in ("SENSES", "EXAMPLES", "PARTS", "COMMENTS"):
        eq(getattr(kanji_server, f"{name}_MAX"), CAP[f"KANJI_{name}_MAX"],
           f"{name}_MAX is the header's row count as it stands")


def test_a_body_longer_than_the_buffer_keeps_everything_that_fits():
    """The 설명 sheet's two long fields, filled past the edge of the buffer.

    `description` and `hook_body` are the only fields whose real content
    routinely exceeds its cap, and a cap set too low is invisible from the
    outside: the panel draws what it was sent and leaves the rest of the box
    white. This is the regression for the measured case — both were capped at
    319 against a 480-byte buffer, which shortened the shape explanation on
    6,169 catalog rows and the memory hook on 1,772.
    """
    # Hangul is three bytes, so the padding puts the cut exactly on the buffer's
    # last byte and the answer is a round BODY_MAX rather than whatever the last
    # whole character happens to allow.
    edge = "설" * (BODY_MAX // 3) + "a" * (BODY_MAX % 3)
    eq(len(edge.encode("utf-8")), BODY_MAX, "the fixture fills the buffer exactly")

    back = json.loads(CARD_BACK)
    hint = json.loads(CARD_HINT)
    back["shape_explanation"] = hint["reason"] = edge + "여기서부터는 버퍼 밖입니다"
    card = flatten_card(dict(STUDY_CARD, back=json.dumps(back, ensure_ascii=False),
                             hint=json.dumps(hint, ensure_ascii=False)), now=NOW)

    eq(card["description"], edge,
       "an over-long description keeps every byte KANJI_BODY_MAX holds")
    eq(len(card["description"].encode("utf-8")), BODY_MAX, "and stops on the cap")
    eq(card["hook_body"], edge, "and the memory hook is held to the same buffer")
    check(len(card["description"].encode("utf-8")) > 319,
          "neither stops at the stale 319 the header never declared")

    eq(flatten_card(STUDY_CARD, now=NOW)["description"],
       json.loads(CARD_BACK)["shape_explanation"],
       "while a description that fits is carried whole")


def test_clip_cuts_on_a_character_boundary():
    eq(clip("会う", 40), "会う", "a short field is untouched")
    # 회 is three bytes; seven of them do not fit in twenty.
    eq(clip("회" * 7, 20), "회" * 6, "a clip lands on a character boundary")
    eq(len(clip("회" * 7, 20).encode("utf-8")), 18, "and stays under the cap")
    eq(clip(None, 10), "", "a missing field is an empty one")
    eq(clip(12, 10), "12", "and a number is its text")


def test_glyph_gate():
    payload = {"card": {"front": "会", "senses": ["모이다"], "note": "emoji \U0001f600"}}
    tiny = set("abcdefghijklmnopqrstuvwxyz ")
    missing = check_glyphs(payload, tiny, warn=False)
    check(missing, "the glyph check notices characters outside the charset")
    eq(check_glyphs(payload, None, warn=False), set(), "no charset means no complaint")

    # The Obsidian scanner only warned, because a note title is the user's own.
    # This catalog is not: a card citing a component form no face covers would
    # be a tofu box every time it came up, and the learner cannot rename it.
    fitted = substitute_missing(payload, tiny)
    eq(check_glyphs(fitted, tiny, warn=False), set(),
       "after substitution nothing undrawable is left")
    eq(fitted["card"]["front"], "",
       "a face carrying neither the middle dot nor a question mark drops the "
       "character rather than trading one tofu box for another")
    with_ascii = tiny | set("?")
    eq(substitute_missing(payload, with_ascii)["card"]["front"], "?",
       "with ASCII present the mark degrades to a question mark")

    real = symbol_set()
    fitted = substitute_missing(payload, real)
    eq(fitted["card"]["senses"], ["모이다"], "Hangul the faces carry is untouched")
    eq(check_glyphs(fitted, real, warn=False), set(),
       "and the emoji is gone against the real font too")
    eq(substitute_missing(payload, None), payload,
       "with no charset the payload is passed through unchanged")

    control = substitute_missing({"body": "a\x07b\nc"}, real)
    eq(control["body"], "ab\nc",
       "a control character is dropped, not marked — but a newline is a paragraph")

    # U+00A0 and U+2009 are category Zs, so isprintable() is False for both. A
    # gate written on isprintable() dropped them with the controls, and 24
    # catalog cards lost the gap the shape decomposition was written around
    # with nothing in the report to say which cards or why.
    spaced = substitute_missing({"body": "占 =\u00a0卜 +\u2009口"}, real)
    eq(spaced["body"], "占 = 卜 + 口",
       "an undrawable space becomes the space the faces carry, not a deletion")
    check(check_glyphs({"body": "\u00a0"}, real, warn=False),
          "and the report names it, rather than passing over it in silence")
    eq(substitute_missing({"body": "a\u00a0b"}, set("ab·")), {"body": "a·b"},
       "a face carrying no space at all falls back to the mark")

    # ∙ and ․ are their own characters, not compatibility forms, so NFC leaves
    # them alone — but at this size they are the · the faces do carry, and a
    # substituted separator costs a word. U+F90A and U+FA66 are the other half:
    # compatibility ideographs NFC composes back to 金 and 辶.
    twins = "音∙訓․読 金辶"
    eq(substitute_missing({"body": twins}, real)["body"], "音·訓·読 金辶",
       "a twin of a drawable mark is folded onto it and a compatibility "
       "ideograph is composed, rather than either becoming a substitution")
    eq(check_glyphs({"body": twins}, real, warn=False), set(),
       "so none of the four is reported as a hole in the first place")
    eq(substitute_missing({"body": "·∙"}, real | {"∙"})["body"],
       "·∙", "while a twin a face does carry is left as the catalog wrote it")


# ---------------------------------------------------------------------------
# Credentials
# ---------------------------------------------------------------------------

def test_load_credentials():
    env = {
        "KANJIS_SUPABASE_URL": "https://example.supabase.co/",
        "KANJIS_SUPABASE_KEY": "publishable-key",
        "KANJIS_EMAIL": "learner@example.com",
        "KANJIS_PASSWORD": "hunter2",
    }
    values = load_credentials(None, env)
    eq(values["supabase_url"], "https://example.supabase.co",
       "the Supabase URL loses its trailing slash so paths concatenate cleanly")
    eq(values["api_base"], "https://api.kanjis.ai/api/v1",
       "the API base has a default, because only one deployment exists")

    for key in ("KANJIS_SUPABASE_URL", "KANJIS_SUPABASE_KEY", "KANJIS_EMAIL",
                "KANJIS_PASSWORD"):
        partial = {k: v for k, v in env.items() if k != key}
        try:
            load_credentials(None, partial)
            check(False, f"a missing {key} is refused")
        except ConfigError as e:
            check(key in str(e), f"a missing {key} is refused, and named", str(e), key)

    # The file half writes a password to disk, so it is removed on the way out
    # even when an assertion above it raises — a run that leaves one behind in
    # /tmp is the exact thing load_credentials() warns about.
    root = tempfile.mkdtemp(prefix="kanji-config-")
    try:
        path = os.path.join(root, "config.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"supabase_url": "https://from-file.supabase.co",
                       "supabase_key": "file-key", "email": "file@example.com",
                       "password": "file-password",
                       "api_base": "http://localhost:8000/api/v1"}, f)
        os.chmod(path, 0o600)

        values = load_credentials(path, {})
        eq(values["email"], "file@example.com",
           "the file supplies what the environment does not")
        eq(values["api_base"], "http://localhost:8000/api/v1",
           "including a local backend to develop against")
        # The environment is the per-run channel, so it wins: a password exported
        # for one invocation should not be silently ignored because a file exists.
        values = load_credentials(path, {"KANJIS_EMAIL": "env@example.com"})
        eq(values["email"], "env@example.com", "the environment overrides the file")
        eq(values["password"], "file-password", "field by field, not all or nothing")

        with open(path, "w", encoding="utf-8") as f:
            f.write("{ not json")
        try:
            load_credentials(path, {})
            check(False, "a damaged config is refused")
        except ConfigError:
            check(True, "a damaged config is refused with a message, not a traceback")

        try:
            load_credentials(os.path.join(root, "nope.json"), {})
            check(False, "a missing config file is refused")
        except ConfigError:
            check(True, "a config file that is not there is refused")
    finally:
        shutil.rmtree(root, ignore_errors=True)


# ---------------------------------------------------------------------------
# What makes a mutating GET safe
# ---------------------------------------------------------------------------

class RecordingProxy(StudyProxy):
    """A StudyProxy with the network replaced by a script of responses.

    Only `_call` is stubbed, so everything above it — the session state, the
    409 rule, the retry rule, the AnswerResponse's different field names — is
    the real code running against the real shapes the backend returns.
    """

    def __init__(self, responses):
        super().__init__(auth=None, charset=None)
        self.responses = list(responses)
        self.calls = []

    def _call(self, method, path, body=None, retry=True):
        self.calls.append((method, path.split("?")[0], body))
        if path.endswith("/comments"):
            return {"generated": True, "comments": []}
        if path == "/study/decks":
            return [{"id": "deck-n5", "name": "JLPT N5 Vocabulary"}]
        return self.responses.pop(0)


def session_response(card_id, complete=False):
    card = None if card_id is None else dict(STUDY_CARD, id=card_id)
    return {"session": SESSION, "card": card, "rating_preview": None,
            "session_complete": complete}


def answer_response(next_id):
    """The upstream's AnswerResponse, which names the same two things else."""
    return {"session": SESSION, "reviewed_card": STUDY_CARD,
            "next_card": None if next_id is None else dict(STUDY_CARD, id=next_id),
            "next_rating_preview": {"again": due_in(600), "hard": due_in(86400),
                                    "good": due_in(9 * 86400), "easy": due_in(21 * 86400)},
            "session_complete": next_id is None}


def test_grade_is_refused_for_a_card_the_proxy_is_not_serving():
    """The property the whole GET-to-grade decision rests on.

    Grading over GET is only defensible because the proxy knows which card it
    is showing. Take that away and a stray request — a browser prefetch, a
    bookmark, a second board — moves somebody's review history.
    """
    proxy = RecordingProxy([session_response("card-A")])
    try:
        proxy.grade("good")
        check(False, "a grade before any card has been served is refused")
    except ConflictError:
        check(True, "a grade before any card has been served is refused")
    eq(proxy.calls, [], "and it never reached the backend")

    payload = proxy.poll()
    eq(payload["card"]["id"], "card-A", "the poll serves a card")

    try:
        proxy.grade("good", card_id="card-B")
        check(False, "a grade naming another card is refused")
    except ConflictError as e:
        check("card-A" in str(e) and "card-B" in str(e),
              "a grade naming another card is refused, and says which is which", str(e))

    proxy.responses.append(answer_response("card-C"))
    before = len(proxy.calls)
    payload = proxy.grade("good", card_id="card-A")
    posted = [c for c in proxy.calls[before:] if c[0] == "POST"]
    eq(len(posted), 1, "grading the served card posts exactly one answer")
    eq(posted[0][2], {"study_card_id": "card-A", "rating": "good", "duration_seconds": 0},
       "the answer carries the served card's id and the wire rating")
    eq(payload["card"]["id"], "card-C",
       "and the response is the NEXT card, taken from the answer's own fields")
    eq(payload["card"]["preview"]["good"], "9일 뒤",
       "including the next card's rating preview, already worded")

    # The board polls over a flaky LAN; a retried request must not be a second
    # grade. That is what the card id in the URL buys.
    before = len(proxy.calls)
    again = proxy.grade("good", card_id="card-A")
    eq(again, payload, "a repeated grade for the card just graded returns the same payload")
    eq(proxy.calls[before:], [], "without touching the backend a second time")

    # But the retry window closes at the next poll. AGAIN puts a card back in
    # the retry pile, so the same id legitimately comes around again — and if
    # the window stayed open, the second time through would be swallowed as a
    # duplicate and the card would never leave the pile.
    proxy.responses.append(session_response("card-A"))
    proxy.poll()
    proxy.responses.append(answer_response("card-D"))
    before = len(proxy.calls)
    payload = proxy.grade("again", card_id="card-A")
    posted = [c for c in proxy.calls[before:] if c[0] == "POST"]
    eq(len(posted), 1, "a card that comes back around from the retry pile is gradable again")
    eq(posted[0][2]["rating"], "again", "with the rating that put it there")
    eq(payload["card"]["id"], "card-D", "and the session moves on")


def test_a_session_with_no_card_is_completion_not_failure():
    proxy = RecordingProxy([session_response(None, complete=True)])
    payload = proxy.poll()
    check("card" not in payload, "a session with no card omits the card entirely")
    eq(payload["session"]["complete"], True, "and says the session is complete")
    eq(payload["session"]["streak"], 12, "while keeping the counters the board shows")
    eq(payload["session"]["deck"], "JLPT N5 Vocabulary",
       "the deck name still comes from the session's decks")

    # And a grade afterwards has nothing to grade.
    try:
        proxy.grade("good")
        check(False, "a grade after the last card is refused")
    except ConflictError:
        check(True, "a grade after the last card is refused")


def test_deck_name_comes_from_the_card():
    """The session spans decks, so the card's own deck is the honest channel."""
    proxy = RecordingProxy([session_response("card-A")])
    eq(proxy.poll()["session"]["deck"], "JLPT N5 Vocabulary", "resolved from /study/decks")
    proxy.responses.append(session_response("card-A"))
    before = len(proxy.calls)
    proxy.poll()
    eq([c for c in proxy.calls[before:] if c[1] == "/study/decks"], [],
       "and looked up once, not on every poll")

    named = RecordingProxy([session_response("card-A")])
    named.deck_override = "내 덱"
    named.poll()
    eq([c for c in named.calls if c[1] == "/study/decks"], [],
       "--deck skips the lookup altogether")


def test_offline_fixture():
    """--offline must serve the committed fixture with no account at all."""
    if not os.path.exists(DEFAULT_FIXTURE):
        print(f"  (skipped: no fixture at {DEFAULT_FIXTURE})")
        return
    proxy = FixtureProxy(DEFAULT_FIXTURE, charset=symbol_set())
    payload = proxy.poll()
    eq(payload["v"], 1, "the fixture is a v1 payload")
    check("session" in payload and "card" in payload, "with a session and a card")
    eq(check_glyphs(payload, symbol_set(), warn=False), set(),
       "and every character in it has a face")
    # A grade with nothing behind it must not fail the board: pressing a key
    # that works everywhere else should not badge the panel 오래됨.
    eq(proxy.grade("good"), payload, "a grade against a fixture returns the same card")


# ---------------------------------------------------------------------------
# The real catalog
# ---------------------------------------------------------------------------

def glyph_recoveries(payload, charset):
    """(characters normalisation saves, undrawable spaces) in one payload.

    Not an assertion. Both counts are properties of the CATALOG and the shipped
    faces, and gen_fonts.py owns the second of those — a face that grows a glyph
    legitimately drives either to zero. What they are here for is the number in
    the sweep's output, so that a change to normalize_text() is visible as a
    change in what nine thousand real cards keep.
    """
    saved = spaces = 0

    def walk(node):
        nonlocal saved, spaces
        if isinstance(node, str):
            for ch in node:
                if ch in charset:
                    continue
                if unicodedata.category(ch).startswith("Z"):
                    spaces += 1
                elif all(c in charset for c in normalize_text(ch, charset)):
                    saved += 1
        elif isinstance(node, dict):
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(payload)
    return saved, spaces


def field_caps(card):
    """(value, cap, label) for every capped string a flattened card carries."""
    yield card["id"], ID_MAX, "id"
    yield card["front"], FRONT_MAX, "front"
    yield card["reading"], READING_MAX, "reading"
    yield card["on_reading"], READING_MAX, "on_reading"
    yield card["kun_reading"], READING_MAX, "kun_reading"
    yield card["level"], LABEL_MAX, "level"
    yield card["gloss"], SENSE_MAX, "gloss"
    yield card["description"], BODY_MAX, "description"
    yield card["hook_title"], LABEL_MAX, "hook_title"
    yield card["hook_body"], BODY_MAX, "hook_body"
    yield card["composition"], FORMULA_MAX, "composition"
    for sense in card["senses"]:
        yield sense, SENSE_MAX, "sense"
    for example in card["examples"]:
        yield example["text"], FRONT_MAX, "example.text"
        yield example["reading"], READING_MAX, "example.reading"
        yield example["gloss"], SENSE_MAX, "example.gloss"
    for part in card["parts"]:
        yield part["glyph"], FRONT_MAX, "part.glyph"
        yield part["meaning"], SENSE_MAX, "part.meaning"
        yield part["reading"], READING_MAX, "part.reading"
    for comment in card["comments"]:
        yield comment["author"], AUTHOR_MAX, "comment.author"
        yield comment["body"], COMMENT_MAX, "comment.body"
    yield card["fsrs"]["state"], LABEL_MAX, "fsrs.state"
    yield card["fsrs"]["state_label"], LABEL_MAX, "fsrs.state_label"
    yield card["fsrs"]["due"], LABEL_MAX, "fsrs.due"
    for word in GRADE_WORDS:
        yield card["preview"][word], LABEL_MAX, f"preview.{word}"


def test_real_catalog_rows(db_path):
    """Every claim in the contract, against nine thousand real cards.

    A synthetic card proves the code does what it was written to do. The
    catalog proves it survives what is actually in it: cards with no hint,
    cards with an empty senses list, shape explanations three hundred bytes
    long, and the handful of component glyphs no shipped face covers.
    """
    if not os.path.exists(db_path):
        print(f"  (skipped: no catalog at {db_path} — set KANJIS_DB to point at one)")
        return

    conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        total = conn.execute("SELECT count(*) FROM card_templates").fetchone()[0]
        rows = conn.execute(
            "SELECT id, front, back, hint, tags_json, sort_order "
            "FROM card_templates ORDER BY id LIMIT ?", (SAMPLE_CARDS,)
        ).fetchall()
    finally:
        conn.close()

    print(f"  ({len(rows)} of {total} catalog cards)")
    check(rows, "the catalog produced rows to flatten")

    charset = symbol_set()
    over_cap = []
    empty_front = []
    substituted = 0
    recovered_cards = spaced_cards = 0
    seen_senses = seen_parts = seen_examples = 0

    for row in rows:
        card_id, front, back, hint, tags_json, sort_order = row
        try:
            tags = json.loads(tags_json) if tags_json else []
        except ValueError:
            tags = []
        # No FSRS state: a catalog row has never been reviewed by anyone, which
        # is exactly the -1 case, so the sweep exercises it nine thousand times.
        source = {"id": card_id, "front": front, "back": back, "hint": hint,
                  "tags": tags, "sort_order": sort_order, "study_deck_id": "d"}

        card = flatten_card(source, {"again": due_in(600), "hard": due_in(4 * 86400),
                                     "good": due_in(9 * 86400), "easy": due_in(21 * 86400)},
                            now=NOW)
        payload = {"v": 1, "session": flatten_session(SESSION, deck="catalog"),
                   "card": card}
        saved, spaces = glyph_recoveries(payload, charset)
        recovered_cards += 1 if saved else 0
        spaced_cards += 1 if spaces else 0
        fitted = substitute_missing(payload, charset)
        if fitted != payload:
            substituted += 1
        card = fitted["card"]

        if not card["front"]:
            empty_front.append(card_id)
        seen_senses += 1 if card["senses"] else 0
        seen_parts += 1 if card["parts"] else 0
        seen_examples += 1 if card["examples"] else 0

        for value, cap, label in field_caps(card):
            if len(value.encode("utf-8")) > cap:
                over_cap.append((card_id, label, len(value.encode("utf-8")), cap))
        if len(card["senses"]) > SENSES_MAX or len(card["examples"]) > EXAMPLES_MAX \
                or len(card["parts"]) > PARTS_MAX or len(card["comments"]) > COMMENTS_MAX:
            over_cap.append((card_id, "array", 0, 0))
        if card["fsrs"]["stability_days"] != -1 or card["fsrs"]["difficulty_pct"] != -1:
            over_cap.append((card_id, "fsrs-should-be-unscheduled", 0, 0))
        if check_glyphs(fitted, charset, warn=False):
            over_cap.append((card_id, "undrawable-after-substitution", 0, 0))

    eq(over_cap[:3], [], "no real card exceeds a device cap after flattening")
    eq(empty_front[:3], [], "no real card flattens to an empty headword")
    check(seen_senses > len(rows) * 0.9,
          f"nearly every real card has senses ({seen_senses}/{len(rows)})")
    check(seen_parts > len(rows) * 0.5,
          f"most real cards have components ({seen_parts}/{len(rows)})")
    check(seen_examples > len(rows) * 0.5,
          f"most real cards have examples ({seen_examples}/{len(rows)})")
    # Informational, not an assertion: how much of this sample the shipped faces
    # cannot draw is a property of the FONT, which gen_fonts.py owns.
    print(f"  ({substituted} of {len(rows)} cards needed a glyph substitution "
          f"against the current faces)")
    print(f"  ({recovered_cards} of {len(rows)} cards keep a character that NFC "
          f"or a twin recovered, {spaced_cards} keep a word break)")

    # The wire is UTF-8 JSON, which is what actually goes out.
    json.loads(json.dumps(payload, ensure_ascii=False).encode("utf-8").decode("utf-8"))
    check(True, "a flattened real card round-trips as UTF-8 JSON")


def test_payload_matches_the_wire_contract():
    """Every key the device's parser reads, present and the right type."""
    payload = {
        "v": 1,
        "session": flatten_session(SESSION, deck="JLPT N5 Vocabulary", level="N5"),
        "card": flatten_card(STUDY_CARD, {"again": due_in(600), "hard": due_in(4 * 86400),
                                          "good": due_in(9 * 86400), "easy": due_in(21 * 86400)},
                             [{"author": "카나 선생", "body": "본문", "likes": 12}], 12, NOW),
    }
    eq(payload["v"], 1, "the producer emits v1")
    eq(sorted(payload["session"]),
       ["complete", "deck", "left_new", "left_review", "level", "retry",
        "reviewed_today", "streak", "track", "track_total"],
       "the session block has exactly its wire fields")
    eq(sorted(payload["card"]),
       ["comment_total", "comments", "composition", "description", "examples", "front",
        "fsrs", "gloss", "hook_body", "hook_title", "id", "kun_reading", "level",
        "on_reading", "parts", "preview", "reading", "senses"],
       "the card has exactly its wire fields")
    eq(sorted(payload["card"]["fsrs"]),
       ["difficulty_pct", "due", "lapses", "reps", "stability_days", "state",
        "state_label"],
       "the fsrs block has exactly its wire fields")
    eq(sorted(payload["card"]["preview"]), ["again", "easy", "good", "hard"],
       "the preview has exactly the four ratings")
    for example in payload["card"]["examples"]:
        eq(sorted(example), ["gloss", "reading", "text"], "an example has its three fields")
    for part in payload["card"]["parts"]:
        eq(sorted(part), ["glyph", "meaning", "reading"], "a part has its three fields")
    for comment in payload["card"]["comments"]:
        eq(sorted(comment), ["author", "body", "likes"], "a comment has its three fields")
    check(payload["card"]["comment_total"] >= len(payload["card"]["comments"]),
          "comment_total is the server's real count, never smaller than what is shown")
    check(len(payload["session"]["deck"].encode("utf-8")) <= DECK_MAX,
          "the deck name fits the device buffer")


def main():
    test_js_round_is_not_pythons_round()
    test_relative_due_thresholds()
    test_relative_due_rounds_the_javascript_way()
    test_relative_due_non_dates()
    test_relative_due_treats_a_bare_date_as_utc()
    test_parse_json_column()
    test_primary_reading_mirrors_the_web()
    test_card_examples_walk_on_then_kun()
    test_card_senses_and_level()
    test_card_parts_tolerate_a_missing_reading()
    test_safe_composition_filters_only_structural_self_references()
    test_project_card_content_retains_the_full_catalog_record()
    test_project_card_content_falls_back_to_the_first_valid_sense()
    test_flatten_card_is_the_whole_projection()
    test_fsrs_unscheduled_is_minus_one_not_zero()
    test_preview_is_four_rendered_spans()
    test_flatten_comments()
    test_flatten_session()
    test_parse_grade()
    test_every_cap_is_the_one_the_header_declares()
    test_a_body_longer_than_the_buffer_keeps_everything_that_fits()
    test_clip_cuts_on_a_character_boundary()
    test_glyph_gate()
    test_load_credentials()
    test_grade_is_refused_for_a_card_the_proxy_is_not_serving()
    test_a_session_with_no_card_is_completion_not_failure()
    test_deck_name_comes_from_the_card()
    test_offline_fixture()
    test_payload_matches_the_wire_contract()
    test_real_catalog_rows(os.environ.get("KANJIS_DB", DEFAULT_DB))

    print(f"\nkanji_server: {CHECKS[0]} checks, {len(FAILURES)} failures")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
