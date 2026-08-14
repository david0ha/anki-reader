#!/usr/bin/env python3
"""
Serve one REAL kanjis.ai study session to the board.

`mock_kanji_server.py` is the contract as a fixed payload — it pins the wire
format and produces the test fixture. This is the other half: it holds a real
account's session against `api.kanjis.ai` and serves the same URL shape from it.
Point the board at this and the panel stops showing the demo card and starts
showing what you actually have due.

    python3 tools/kanji_server.py --check          # verify the setup, no board
    python3 tools/kanji_server.py                  # serve on :8123

Then give the board `http://<this machine>:8123/kanji.json` — that is the URL
the captive portal asks for and stores in NVS.

Everything the board cannot do happens here, and that list is the reason this
program exists at all: TLS to an internet host, a Supabase bearer token, the
second JSON parse of the card's `back`/`hint` columns, and every date-to-Korean
conversion — the board has no RTC, so it cannot compute "9일 뒤" from an ISO
timestamp even in principle. See docs/kanji-contract.md.

The device carries a device identity, never a user credential. Nothing on the
board can leak the account: the token lives in this process and only ever
appears in an outgoing Authorization header.

Credentials
-----------
Never in this repository, never on the command line (a command line is in the
shell history and in `ps`), never in a log. Two channels, and the environment
wins where both supply a value:

    KANJIS_SUPABASE_URL   https://<project-ref>.supabase.co
    KANJIS_SUPABASE_KEY   the publishable ("anon") key — the same one a browser
                          gets; it authorises nothing on its own
    KANJIS_EMAIL          the account's email
    KANJIS_PASSWORD       its password
    KANJIS_API_BASE       optional, default https://api.kanjis.ai/api/v1

    python3 tools/kanji_server.py --config ~/.config/kanjis-board.json

with the same five names as keys (`supabase_url`, `supabase_key`, `email`,
`password`, `api_base`). The file is a password on disk, so this warns if its
mode lets anyone but you read it.

The two requests
----------------
    GET /kanji.json                  the card the session is serving right now
    GET /kanji.json?grade=good       grade that card, then serve the next one

Grading is a GET because `http_port.h` exposes exactly one call and adding a
method, a body and headers to its three implementations to move one enum across
a LAN buys nothing. What makes that safe is the one thing this proxy remembers:
the card it last served. The board names the card it is rating
(`?grade=good&card=<id>`), so a grade for anything else is a 409 and a stray or
late request cannot reach somebody's review history; a repeat of the id just
graded is recognised as a retry instead of being counted twice. An id is
optional — the built-in demo card has none — and without one the proxy grades
whatever it is currently serving.

Modes
-----
    --check              log in, fetch one card, print the payload and the glyph
                         report, exit. The way to find out your credentials work
                         without a board on the desk.
    --offline            serve the committed fixture and never touch the
                         network, so the board can be exercised with no account
                         at all.
    --level N5           study only one JLPT level (default: every level, 전체)

Usage
-----
    python3 tools/kanji_server.py                        # serve on :8123
    python3 tools/kanji_server.py --port 9000
    python3 tools/kanji_server.py --check
    python3 tools/kanji_server.py --offline
    python3 tools/kanji_server.py --fixture some/other.json
"""

import argparse
import json
import math
import os
import re
import stat
import sys
import threading
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_FIXTURE = os.path.join(ROOT, "components", "vault_core", "test", "host",
                               "fixtures", "kanji.json")
MODEL_H = os.path.join(ROOT, "components", "vault_core", "include", "kanji_model.h")

DEFAULT_API_BASE = "https://api.kanjis.ai/api/v1"

# Device-side caps, read out of the header that declares the buffers rather than
# copied into a table here. The parser clamps to them anyway; matching them means
# what is served is what is shown, so a sense that lost its tail on the panel is
# not a mystery. A copy is only right until one of the buffers moves, and this
# one already drifted: BODY_MAX sat at 319 against a 480-byte buffer, which cost
# 6,352 of the catalog's 9,956 cards text the panel had the room to draw
# (6,169 of them a shortened shape explanation, 1,772 a shortened memory hook).
_BUFFERS = ("KANJI_FRONT_MAX", "KANJI_READING_MAX", "KANJI_SENSE_MAX",
            "KANJI_LABEL_MAX", "KANJI_DECK_MAX", "KANJI_ID_MAX",
            "KANJI_BODY_MAX", "KANJI_FORMULA_MAX", "KANJI_AUTHOR_MAX",
            "KANJI_COMMENT_MAX")
_COUNTS = ("KANJI_SENSES_MAX", "KANJI_EXAMPLES_MAX", "KANJI_PARTS_MAX",
           "KANJI_COMMENTS_MAX")


def device_caps(path=MODEL_H):
    """Every `#define KANJI_* <n>` in kanji_model.h, as the header states it.

    Fatal when it cannot be read or a name is absent, rather than falling back
    to a default. A proxy that guesses a cap serves text the device silently
    truncates — which is the failure this function exists to make impossible,
    so inventing a number here would reintroduce it in a quieter form.
    """
    try:
        with open(path, encoding="utf-8") as source:
            src = source.read()
    except OSError as e:
        raise RuntimeError(f"device caps unreadable: {e}") from None
    caps = {name: int(value)
            for name, value in re.findall(r"#define\s+(KANJI_\w+)\s+(\d+)", src)}
    missing = [name for name in _BUFFERS + _COUNTS if name not in caps]
    if missing:
        raise RuntimeError(f"{path} defines no " + ", ".join(missing))
    return caps


CAPS = device_caps()

# A buffer holds one byte fewer of content than it is wide: the last is the NUL
# kanji_str_copy always writes. A row count is a count and has no NUL to lose.
FRONT_MAX = CAPS["KANJI_FRONT_MAX"] - 1
READING_MAX = CAPS["KANJI_READING_MAX"] - 1
SENSE_MAX = CAPS["KANJI_SENSE_MAX"] - 1
LABEL_MAX = CAPS["KANJI_LABEL_MAX"] - 1
DECK_MAX = CAPS["KANJI_DECK_MAX"] - 1
ID_MAX = CAPS["KANJI_ID_MAX"] - 1
BODY_MAX = CAPS["KANJI_BODY_MAX"] - 1
FORMULA_MAX = CAPS["KANJI_FORMULA_MAX"] - 1
AUTHOR_MAX = CAPS["KANJI_AUTHOR_MAX"] - 1
COMMENT_MAX = CAPS["KANJI_COMMENT_MAX"] - 1

SENSES_MAX = CAPS["KANJI_SENSES_MAX"]
EXAMPLES_MAX = CAPS["KANJI_EXAMPLES_MAX"]
PARTS_MAX = CAPS["KANJI_PARTS_MAX"]
COMMENTS_MAX = CAPS["KANJI_COMMENTS_MAX"]

COUNT_MAX = 9999          # the session chips
REPS_MAX = 99999          # reps / lapses

# The four wire words, in kanji_grade_t order. kanji_model.c's
# kanji_grade_wire() emits exactly these.
GRADE_WORDS = ("again", "hard", "good", "easy")

# The two paths this serves, spelled the same as mock_kanji_server.PATHS. `/` is
# here because a URL typed into the captive portal without its file name is the
# most common way to get this wrong.
PATHS = ("/kanji.json", "/")

# The wire word -> the Korean the FSRS sheet prints (S_STATE_* in ui_strings.h).
STATE_LABELS = {
    "new": "새 카드",
    "learning": "학습 중",
    "review": "복습",
    "relearning": "다시 학습",
}

# The level chip when no JLPT filter is active.
LEVEL_ALL = "전체"

JLPT_TAG = re.compile(r"^N[1-5]$")

# py-fsrs difficulty is 1..10; the panel prints a percentage. (D-1)/9 is the
# same normalisation Anki's FSRS display uses, so a card that reads 47% here
# reads 47% there.
FSRS_DIFFICULTY_MIN = 1.0
FSRS_DIFFICULTY_MAX = 10.0

# What a character outside the shipped faces is replaced with. A middle dot is
# in S_COMPOSED_CHARS, is one cell wide, and reads as "something was here"
# rather than as a word — unlike a tofu box, which reads as a broken panel.
GLYPH_SUBSTITUTE = "·"

# What an undrawable space becomes. The faces carry no NO-BREAK SPACE, and the
# catalog writes the gap after the "=" of a decomposition with one, so dropping
# it runs the two halves of the shape together for nothing: the plain ASCII
# space it meant is a character every face on this board carries.
GLYPH_SPACE = " "

# Visual twins of a mark the faces DO carry, for the cases canonical
# equivalence deliberately does not cover — these are separate characters, not
# compatibility forms of ·, so NFC leaves them alone. At 648x480 in one bit
# they are the same mark, and folding them keeps a separator that substitution
# would have spent on "something was here". Nothing may go in here that is not
# visually the same character: a fold that changes the word is worse than a
# middle dot, because it is wrong without looking wrong.
GLYPH_TWINS = {
    "∙": "·",       # BULLET OPERATOR
    "․": "·",       # ONE DOT LEADER
}


# ---------------------------------------------------------------------------
# The Korean relative span
# ---------------------------------------------------------------------------
#
# Ported from kanjis-front's src/youtube/cards/gradeSwipe.ts `relativeDue`. The
# board and the browser must word the same schedule the same way, and the board
# cannot compute it at all: it has no RTC, so an ISO timestamp is an opaque
# string to it. Every date on the wire is already a span because of that.

MIN_S = 60
HOUR_S = 3600
DAY_S = 86400


def js_round(value):
    """JavaScript's Math.round: a half rounds toward +infinity, not to even.

    Python's round() is banker's rounding, so round(0.5) is 0 and round(2.5) is
    2. The boundaries here land on exact halves often enough — 30 seconds is
    half a minute, 12 hours half a day — that the two would disagree about the
    same card on the panel and in the browser.
    """
    return math.floor(value + 0.5)


def parse_iso(value):
    """A timestamp as an aware datetime, or None if it is not one.

    Deliberately reproduces Date.parse's two conventions, because this is a
    port and not a rewrite: a bare date is UTC, a date-time with no offset is
    LOCAL. The backend always sends `...Z`, so neither branch normally fires —
    they are here so that a card whose timestamp is some other shape lands
    where the web client would put it rather than somewhere new.
    """
    if not isinstance(value, str):
        return None
    text = value.strip()
    if not text:
        return None
    if text.endswith(("Z", "z")):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is not None:
        return parsed
    # Date-only is UTC in ECMA-262; a date-time without an offset is local.
    if len(value.strip()) <= 10:
        return parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone()


def relative_due(value, now=None):
    """"9일 뒤" for an absolute ISO timestamp. The exact port of relativeDue().

    The one deliberate difference: an empty input comes back as "" rather than
    as whatever falsy thing was passed, because the wire field is a string and
    the contract spells the never-scheduled case as "".
    """
    if not value:
        return ""
    due = parse_iso(value)
    if due is None:
        return value.strip() if isinstance(value, str) else ""
    if now is None:
        now = datetime.now(timezone.utc)
    elif now.tzinfo is None:
        now = now.astimezone()
    seconds = js_round((due - now).total_seconds())
    if seconds < 45:
        return "곧"
    if seconds < HOUR_S:
        return f"{js_round(seconds / MIN_S)}분 뒤"
    if seconds < DAY_S:
        return f"{js_round(seconds / HOUR_S)}시간 뒤"
    if seconds < 30 * DAY_S:
        return f"{js_round(seconds / DAY_S)}일 뒤"
    if seconds < 365 * DAY_S:
        return f"{js_round(seconds / (30 * DAY_S))}개월 뒤"
    return f"{js_round(seconds / (365 * DAY_S))}년 뒤"


# ---------------------------------------------------------------------------
# Fitting one field
# ---------------------------------------------------------------------------

def clip(text, max_bytes):
    """Truncate on a UTF-8 character boundary, exactly as kanji_str_copy does.

    No ellipsis: the contract says an over-long field costs its tail and
    nothing else, and three bytes spent on "…" is a whole kanji of the sense
    the learner was trying to read.
    """
    if text is None:
        return ""
    text = str(text)
    encoded = text.encode("utf-8")
    if len(encoded) <= max_bytes:
        return text
    return encoded[:max_bytes].decode("utf-8", errors="ignore")


def clamp(value, low, high):
    """An int inside [low, high]. Anything unnumbered becomes `low`."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return low
    if isinstance(value, float) and value != value:      # NaN
        return low
    return max(low, min(high, int(value)))


# ---------------------------------------------------------------------------
# Projecting one card
# ---------------------------------------------------------------------------
#
# The `back` and `hint` columns are JSON inside a string column, and the board
# does not run a second cJSON pass per card. Everything below mirrors
# kanjis-front's src/api/content.ts + src/api/toKanjiCard.ts, so the board and
# the web agree on what a card *is* — same reading line, same senses, same
# memory hook — rather than being two independent readings of the same row.

def parse_json_column(raw):
    """`back` / `hint` as a dict. {} for anything unusable.

    Resilient on purpose, like parseCardBack(): a card whose content is
    malformed still has a headword worth putting on the glass, and refusing the
    whole card would blank the panel — the one failure a learner notices.
    """
    if isinstance(raw, dict):
        return raw
    if not isinstance(raw, str):
        return {}
    text = raw.strip()
    if not text:
        return {}
    try:
        parsed = json.loads(text)
    except ValueError:
        return {}
    return parsed if isinstance(parsed, dict) else {}


def _readings(back, key):
    entries = back.get(key)
    return [e for e in entries if isinstance(e, dict)] if isinstance(entries, list) else []


def primary_reading(on, kun):
    """On-yomi leads the reading line; kun-yomi carries a kun-only word.

    primaryReading()'s dedupe and ・ join, kept because the join is what makes
    a two-reading card one row instead of two.
    """
    source = on if on else kun
    seen = []
    for entry in source:
        reading = entry.get("reading")
        if isinstance(reading, str) and reading and reading not in seen:
            seen.append(reading)
    return "・".join(seen)


def card_examples(on, kun, limit=EXAMPLES_MAX):
    """The first `limit` 예문, in the order firstExample() looks for its one.

    The web card shows a single example; the panel has three rows, so this
    keeps walking the same on-then-kun order and takes three. The board's first
    row is therefore the web's example, which is what makes them recognisably
    the same card rather than two selections from the same data.
    """
    out = []
    for entry in list(on) + list(kun):
        examples = entry.get("examples")
        if not isinstance(examples, list):
            continue
        for example in examples:
            if not isinstance(example, dict):
                continue
            text = example.get("text")
            if not isinstance(text, str) or not text:
                continue
            out.append({
                "text": clip(text, FRONT_MAX),
                "reading": clip(example.get("reading") or "", READING_MAX),
                "gloss": clip(example.get("gloss") or "", SENSE_MAX),
            })
            if len(out) >= limit:
                return out
    return out


def card_senses(back, limit=SENSES_MAX):
    """The Korean glosses, most important first.

    `back.meaning.senses` verbatim, as cardContent() reads it. The single-line
    `gloss` ("모일 회") is a different thing — a mnemonic name, not a meaning —
    so it is only used when there are no senses at all, which is the same
    fallback order meaningKo uses in reverse.
    """
    meaning = back.get("meaning")
    meaning = meaning if isinstance(meaning, dict) else {}
    raw = meaning.get("senses")
    senses = [s for s in raw if isinstance(s, str) and s] if isinstance(raw, list) else []
    if not senses:
        gloss = meaning.get("gloss")
        senses = [gloss] if isinstance(gloss, str) and gloss else []
    return [clip(s, SENSE_MAX) for s in senses[:limit]]


def raw_card_parts(hint):
    """Every usable source component without display clipping.

    `reading` is canonically required and missing on a few thousand rows
    (the backend's own audit calls it legacy_shape_without_reading), so it is
    kept as an empty field rather than fabricated or dropped.
    """
    shapes = hint.get("shapes")
    if not isinstance(shapes, list):
        return []
    out = []
    for shape in shapes:
        if not isinstance(shape, dict):
            continue
        glyph = shape.get("kanji")
        if not isinstance(glyph, str) or not glyph:
            continue
        out.append({
            "glyph": glyph,
            "meaning": shape.get("meaning") if isinstance(shape.get("meaning"), str) else "",
            "reading": shape.get("reading") if isinstance(shape.get("reading"), str) else "",
        })
    return out


def card_parts(hint, limit=PARTS_MAX):
    """`hint.shapes[]` as the panel's bounded 구성 요소 rows."""
    return [{
        "glyph": clip(part["glyph"], FRONT_MAX),
        "meaning": clip(part["meaning"], SENSE_MAX),
        "reading": clip(part["reading"], READING_MAX),
    } for part in raw_card_parts(hint)[:limit]]


def safe_composition(front, composition_kanji, parts):
    """Return a structural equation and its displayed component rows.

    A single-kanji source often repeats itself in `hint.shapes`; that row is
    not a constituent. A compound source can list every sub-radical, so only
    its top-level headword characters remain. No semantic role is guessed.
    """
    target = composition_kanji if isinstance(composition_kanji, str) else ""
    if not target:
        target = front if isinstance(front, str) else ""
    if not target:
        return "", []
    clean = [part for part in parts if isinstance(part, dict) and
             isinstance(part.get("glyph"), str) and part["glyph"]]
    if len(target) == 1:
        selected = [part for part in clean if part["glyph"] != target]
    else:
        selected = [part for part in clean if part["glyph"] in target]
    equation = " + ".join(part["glyph"] for part in selected)
    return (f"{equation} = {target}" if equation else ""), selected


def _project_card_content(card, back, hint):
    """Project one card after its two JSON columns have been decoded."""
    on = _readings(back, "on_yomi")
    kun = _readings(back, "kun_yomi")
    meaning = back.get("meaning") if isinstance(back.get("meaning"), dict) else {}
    kanji = back.get("kanji") if isinstance(back.get("kanji"), str) else ""
    front = card.get("front") if isinstance(card.get("front"), str) else ""
    front = front or kanji
    raw_parts = raw_card_parts(hint)
    composition, _ = safe_composition(front, kanji or front, raw_parts)
    raw_gloss = meaning.get("gloss")
    gloss = raw_gloss.strip() if isinstance(raw_gloss, str) else ""
    raw_senses = meaning.get("senses") if isinstance(meaning.get("senses"), list) else []
    senses = [sense.strip() for sense in raw_senses
              if isinstance(sense, str) and sense.strip()]
    if not senses and gloss:
        senses = [gloss]
    if not gloss and senses:
        gloss = senses[0]
    on_reading = primary_reading(on, [])
    kun_reading = primary_reading(kun, [])
    principle = hint.get("principle") if isinstance(hint.get("principle"), str) else ""
    reason = hint.get("reason") if isinstance(hint.get("reason"), str) else ""
    description = back.get("shape_explanation") if isinstance(back.get("shape_explanation"), str) else ""
    tags = card.get("tags") if isinstance(card.get("tags"), list) else []
    level = next((tag for tag in tags if isinstance(tag, str) and JLPT_TAG.match(tag)), "")
    return {
        "id": card.get("id") if isinstance(card.get("id"), str) else "",
        "front": front,
        "reading": primary_reading(on, kun),
        "on_reading": on_reading,
        "kun_reading": kun_reading,
        "level": level,
        "gloss": gloss,
        "senses": senses,
        "description": description,
        "hook_title": principle,
        "hook_body": reason,
        "composition": composition,
        "parts": raw_parts,
    }


def project_card_content(card):
    """The complete card-only catalog record, parsed once and never clipped."""
    card = card if isinstance(card, dict) else {}
    return _project_card_content(card, parse_json_column(card.get("back")),
                                 parse_json_column(card.get("hint")))


def jlpt_level(tags):
    """The card's JLPT tag, or "". The same find() the web card's chip uses."""
    if not isinstance(tags, list):
        return ""
    for tag in tags:
        if isinstance(tag, str) and JLPT_TAG.match(tag):
            return clip(tag, LABEL_MAX)
    return ""


def card_fsrs(state, now=None):
    """The learner's scheduler state, already worded.

    stability_days and difficulty_pct are -1 rather than 0 when the scheduler
    has no value. A new card has no stability; a card with a same-day interval
    has one that rounds to zero. Sending 0 for an unscheduled card makes the
    board claim to know something it does not, and the panel prints the two
    differently (— against 0일) precisely so it never does.
    """
    state = state if isinstance(state, dict) else {}
    name = state.get("state")
    name = name if isinstance(name, str) and name else "new"

    stability = state.get("stability")
    difficulty = state.get("difficulty")
    return {
        "state": clip(name, LABEL_MAX),
        "state_label": clip(STATE_LABELS.get(name, name), LABEL_MAX),
        "due": clip(relative_due(state.get("due_at"), now), LABEL_MAX),
        "reps": clamp(state.get("total_reviews"), 0, REPS_MAX),
        "lapses": clamp(state.get("lapses"), 0, REPS_MAX),
        "stability_days": (-1 if not isinstance(stability, (int, float))
                           or isinstance(stability, bool)
                           else clamp(js_round(stability), 0, REPS_MAX)),
        "difficulty_pct": difficulty_pct(difficulty),
    }


def difficulty_pct(difficulty):
    """py-fsrs's 1..10 difficulty as the 0..100 the panel prints. -1 for none."""
    if isinstance(difficulty, bool) or not isinstance(difficulty, (int, float)):
        return -1
    span = FSRS_DIFFICULTY_MAX - FSRS_DIFFICULTY_MIN
    return clamp(js_round((difficulty - FSRS_DIFFICULTY_MIN) / span * 100), 0, 100)


def card_preview(rating_preview, now=None):
    """The four ratings' next-due timestamps, each already a Korean span.

    Rendered against the SERVER's clock, which is the only clock in this system
    that knows what time it is.
    """
    preview = rating_preview if isinstance(rating_preview, dict) else {}
    return {
        word: clip(relative_due(preview.get(word), now), LABEL_MAX)
        for word in GRADE_WORDS
    }


def flatten_comments(nodes, limit=COMMENTS_MAX):
    """The thread as (first `limit` comments, true total).

    Replies are flattened INTO the list rather than dropped: a reply is often
    the line that answers the question the top-level comment asked. The panel
    has three rows and no room for an indent, which inside three rows reads as
    a rendering bug rather than as a conversation. Depth-first, so a reply
    still sits next to what it replies to.
    """
    flat = []

    def walk(node):
        if not isinstance(node, dict):
            return
        body = node.get("body")
        if isinstance(body, str) and body.strip():
            flat.append(node)
        replies = node.get("replies")
        if isinstance(replies, list):
            for reply in replies:
                walk(reply)

    for node in nodes if isinstance(nodes, list) else []:
        walk(node)

    out = []
    for node in flat[:limit]:
        persona = node.get("persona")
        author = persona.get("name") if isinstance(persona, dict) else None
        out.append({
            # A comment with no persona is the learner's own, which is who
            # "나" is. The alternative — an empty author row — costs the same
            # pixels and says less.
            "author": clip(author if isinstance(author, str) and author else "나",
                           AUTHOR_MAX),
            "body": clip(" ".join(str(node.get("body", "")).split()), COMMENT_MAX),
            "likes": clamp(node.get("like_count"), 0, REPS_MAX),
        })
    return out, len(flat)


def flatten_card(card, rating_preview=None, comments=(), comment_total=None, now=None):
    """One StudyCard, its rating preview and its thread as the board's `card`.

    Pure: no clock of its own unless you leave `now` out, no network, no state.
    Everything the device will ever know about a card comes through here, so
    this is the function the tests drive with real rows.
    """
    card = card if isinstance(card, dict) else {}
    back = parse_json_column(card.get("back"))
    hint = parse_json_column(card.get("hint"))
    content = _project_card_content(card, back, hint)
    on = _readings(back, "on_yomi")
    kun = _readings(back, "kun_yomi")

    return {
        "id": clip(content["id"], ID_MAX),
        "front": clip(content["front"], FRONT_MAX),
        "reading": clip(content["reading"], READING_MAX),
        "on_reading": clip(content["on_reading"], READING_MAX),
        "kun_reading": clip(content["kun_reading"], READING_MAX),
        "level": clip(content["level"], LABEL_MAX),
        "gloss": clip(content["gloss"], SENSE_MAX),
        "senses": [clip(sense, SENSE_MAX) for sense in content["senses"][:SENSES_MAX]],
        "examples": card_examples(on, kun),
        "description": clip(content["description"], BODY_MAX),
        "hook_title": clip(content["hook_title"], LABEL_MAX),
        "hook_body": clip(content["hook_body"], BODY_MAX),
        "composition": clip(content["composition"], FORMULA_MAX),
        "parts": [{
            "glyph": clip(part["glyph"], FRONT_MAX),
            "meaning": clip(part["meaning"], SENSE_MAX),
            "reading": clip(part["reading"], READING_MAX),
        } for part in content["parts"][:PARTS_MAX]],
        "comments": list(comments),
        "comment_total": clamp(len(comments) if comment_total is None else comment_total,
                               0, REPS_MAX),
        "fsrs": card_fsrs(card.get("state"), now),
        "preview": card_preview(rating_preview, now),
    }


def flatten_session(session, deck="", level="", complete=False):
    """The session counters as the header chips and the queue line.

    `reviewed_today` and `track` are derived the way kanjis-front derives them
    (sessionDetail.ts / sessionState.ts): solved is the target minus what is
    left, remaining includes the retry pile. The board shows a position in
    today's queue, so `track` counts the card being served, hence the +1.
    """
    session = session if isinstance(session, dict) else {}
    target_new = clamp(session.get("target_new"), 0, COUNT_MAX)
    target_review = clamp(session.get("target_review"), 0, COUNT_MAX)
    left_new = clamp(session.get("left_new"), 0, COUNT_MAX)
    left_review = clamp(session.get("left_review"), 0, COUNT_MAX)
    retry = clamp(session.get("retry_cards"), 0, COUNT_MAX)

    total = min(COUNT_MAX, target_new + target_review)
    remaining = min(COUNT_MAX, left_new + left_review + retry)
    reviewed = max(0, target_new - left_new) + max(0, target_review - left_review)
    track = clamp(total - remaining + 1, 1, total) if total else 0

    return {
        "deck": clip(deck, DECK_MAX),
        "level": clip(level or LEVEL_ALL, LABEL_MAX),
        "streak": clamp(session.get("streak"), 0, COUNT_MAX),
        "reviewed_today": clamp(reviewed, 0, COUNT_MAX),
        "left_new": left_new,
        "left_review": left_review,
        "retry": retry,
        "track": track,
        "track_total": total,
        "complete": bool(complete),
    }


# ---------------------------------------------------------------------------
# Grades
# ---------------------------------------------------------------------------

def parse_grade(value):
    """The `?grade=` word, or None.

    The four wire words and nothing else. The backend's AnswerRequest also
    accepts 1..4 and normalises aliases, but the board never sends a number and
    this proxy is the only thing between an unauthenticated LAN GET and
    somebody's review history — so the accepted set is exactly what
    kanji_grade_wire() emits, lowercased and stripped.
    """
    if not isinstance(value, str):
        return None
    word = value.strip().lower()
    return word if word in GRADE_WORDS else None


# ---------------------------------------------------------------------------
# Glyph coverage
# ---------------------------------------------------------------------------

def load_device_charset():
    """What the shipped fonts can actually draw, from the generator itself.

    Not a second list to keep in step: `gen_fonts.symbol_set()` IS the set the
    faces were built from. Anything outside it reaches the panel as a tofu box,
    and a tofu box is only visible once the firmware is on the glass — which is
    the worst possible place to discover that a card's shape explanation cites
    a simplified-Chinese component form.
    """
    try:
        from gen_fonts import symbol_set
        return symbol_set()
    except Exception as e:                       # noqa: BLE001 - advisory only
        print(f"glyph check unavailable ({e})", file=sys.stderr)
        return None


def normalize_text(text, charset):
    """`text` as the panel will be asked to draw it, before anything is lost.

    Two passes for one reason: a character the faces cannot draw is often a
    spelling of one they can. NFC is the half Unicode defines — U+F90A and
    U+FA66 are compatibility ideographs whose canonical forms are the 金 and 辶
    already in the faces, so composing costs nothing and recovers the glyph.
    GLYPH_TWINS is the half it deliberately leaves alone, and it is applied
    only where the character is undrawable, so text the faces already carry is
    passed through as the catalog wrote it.
    """
    return "".join(ch if ch in charset else GLYPH_TWINS.get(ch, ch)
                   for ch in unicodedata.normalize("NFC", text))


def is_control(ch):
    """A character that carries no glyph because it is not text.

    Cc and Cf only. Every other undrawable character is a glyph that failed and
    is worth marking; a control is data that should not have been in the column,
    and marking one would put a middle dot where the learner sees a mistake.
    """
    return unicodedata.category(ch) in ("Cc", "Cf")


def check_glyphs(payload, charset, warn=True):
    """Report characters the board cannot draw. Returns the offending set.

    Normalised first, so a character substitute_missing() is about to recover is
    not reported as a hole. A space separator IS reported, unlike a control: the
    faces carry no NO-BREAK SPACE and no THIN SPACE, and a report that stays
    quiet about those is how 24 catalog cards lost one without anybody noticing.
    """
    if not charset:
        return set()
    missing = set()

    def walk(node):
        if isinstance(node, str):
            missing.update(c for c in normalize_text(node, charset)
                           if c not in charset and not is_control(c))
        elif isinstance(node, dict):
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(payload)
    if missing and warn:
        shown = " ".join(sorted(missing))
        print(f"warning: {len(missing)} character(s) the board has no glyph for "
              f"and will draw as boxes: {shown}", file=sys.stderr)
    return missing


def substitute_missing(payload, charset, mark=GLYPH_SUBSTITUTE):
    """Rebuild `payload` with every undrawable character replaced.

    The Obsidian scanner only warned, because a note title is the user's own
    and they can rename it. This catalog is not: a handful of its nine thousand
    cards cite component forms and astral-plane radicals that no shipped face
    covers, and there is nothing the learner can do about it. Warning and then
    sending the character anyway would put a tofu box on the glass every time
    that card came up.

    Control characters are dropped rather than marked — a stray one is not a
    glyph that failed, it is data that should not have been there — except for
    the newline, which the 설명 sheet renders as a paragraph break. A space is
    the third case and neither of the other two: an undrawable one is a word
    break the card meant, so it becomes the space the faces can draw rather
    than a mark that reads as a word or a deletion that reads as a typo.
    """
    if not charset:
        return payload
    if mark not in charset:
        # The faces are generated, not hand-maintained, so the mark can fall out
        # of them. Degrade rather than trade one undrawable character for
        # another: a question mark next, and if even that is missing, nothing.
        mark = "?" if "?" in charset else ""
    space = GLYPH_SPACE if GLYPH_SPACE in charset else mark

    def one(ch):
        if ch in charset or ch == "\n":
            return ch
        if is_control(ch):
            return ""
        # Zs, and Zl/Zp with it. Whatever kind of break it was meant to be, one
        # space is the only width of gap this panel has to say it with.
        if unicodedata.category(ch).startswith("Z"):
            return space
        return mark

    def fit(text):
        return "".join(one(ch) for ch in normalize_text(text, charset))

    def walk(node):
        if isinstance(node, str):
            return fit(node)
        if isinstance(node, dict):
            return {k: walk(v) for k, v in node.items()}
        if isinstance(node, list):
            return [walk(v) for v in node]
        return node

    return walk(payload)


# ---------------------------------------------------------------------------
# Talking to the upstream
# ---------------------------------------------------------------------------

class UpstreamError(Exception):
    """An upstream request that did not come back usable."""

    def __init__(self, message, status=0):
        super().__init__(message)
        self.status = status


class ConflictError(Exception):
    """A grade for a card this proxy is not serving."""


def http_json(url, *, method="GET", body=None, headers=None, timeout=15):
    """One JSON request. Raises UpstreamError for everything that goes wrong.

    Errors carry the status and a short server message and nothing else. They
    must never carry the request: its headers hold the bearer token and its
    body, on the sign-in path, holds a password.
    """
    data = json.dumps(body).encode("utf-8") if body is not None else None
    request = urllib.request.Request(url, data=data, method=method)
    request.add_header("Accept", "application/json")
    if data is not None:
        request.add_header("Content-Type", "application/json")
    for key, value in (headers or {}).items():
        request.add_header(key, value)

    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
    except urllib.error.HTTPError as e:
        # `from None`: the chained traceback would hold the Request object, and
        # this exception's text ends up in a log.
        raise UpstreamError(f"{e.code} {_error_detail(e)}", status=e.code) from None
    except urllib.error.URLError as e:
        raise UpstreamError(f"unreachable: {e.reason}") from None
    except OSError as e:
        raise UpstreamError(f"unreachable: {e}") from None

    if not raw:
        return {}
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        raise UpstreamError(f"unparseable response: {e}") from None


def _error_detail(error):
    """The server's own words for a failure, capped and never the whole body."""
    try:
        raw = error.read(512).decode("utf-8", errors="replace")
    except Exception:                            # noqa: BLE001 - advisory only
        return error.reason or ""
    try:
        parsed = json.loads(raw)
    except ValueError:
        return " ".join(raw.split())[:200]
    if isinstance(parsed, dict):
        for key in ("error_description", "msg", "message", "detail", "error"):
            value = parsed.get(key)
            if isinstance(value, str) and value:
                return value[:200]
    return " ".join(raw.split())[:200]


class SupabaseAuth:
    """An email+password Supabase session, refreshed before it expires.

    The token never leaves this object except as an Authorization header:
    nothing here prints it, `__repr__` is not overridden to include it, and the
    only accessor returns it to the one caller that needs it.
    """

    # Refresh this far ahead of expiry, so a poll that starts just before the
    # deadline does not arrive just after it.
    LEEWAY_S = 120

    def __init__(self, url, key, email, password, timeout=15):
        self.url = url.rstrip("/")
        self._key = key
        self._email = email
        self._password = password
        self.timeout = timeout
        self._access = None
        self._refresh = None
        self._expires_at = 0.0
        self.user_id = ""

    def token(self, force=False):
        """A valid access token, signing in or refreshing if it has to."""
        if not force and self._access and time.time() < self._expires_at - self.LEEWAY_S:
            return self._access
        if not force and self._refresh:
            try:
                return self._apply(self._grant("refresh_token",
                                               {"refresh_token": self._refresh}))
            except UpstreamError as e:
                # A revoked or rotated refresh token is not fatal; the password
                # is still here. Say so once rather than failing the poll.
                print(f"refresh failed ({e}); signing in again", file=sys.stderr)
                self._refresh = None
        return self._apply(self._grant("password",
                                       {"email": self._email, "password": self._password}))

    def _grant(self, grant_type, payload):
        return http_json(
            f"{self.url}/auth/v1/token?grant_type={grant_type}",
            method="POST",
            body=payload,
            headers={"apikey": self._key, "Authorization": f"Bearer {self._key}"},
            timeout=self.timeout,
        )

    def _apply(self, response):
        access = response.get("access_token") if isinstance(response, dict) else None
        if not isinstance(access, str) or not access:
            raise UpstreamError("sign-in returned no access token")
        self._access = access
        refresh = response.get("refresh_token")
        self._refresh = refresh if isinstance(refresh, str) and refresh else None
        expires_at = response.get("expires_at")
        expires_in = response.get("expires_in")
        if isinstance(expires_at, (int, float)) and not isinstance(expires_at, bool):
            self._expires_at = float(expires_at)
        elif isinstance(expires_in, (int, float)) and not isinstance(expires_in, bool):
            self._expires_at = time.time() + float(expires_in)
        else:
            self._expires_at = time.time() + 3600.0
        user = response.get("user")
        if isinstance(user, dict) and isinstance(user.get("id"), str):
            self.user_id = user["id"]
        return self._access


# ---------------------------------------------------------------------------
# The proxy
# ---------------------------------------------------------------------------

class StudyProxy:
    """One kanjis.ai study session, adapted to the board's contract.

    Single-client by design, because the board is a single client. The proxy
    remembers exactly one thing across requests — the card it last served — and
    that memory is what makes a repeat poll idempotent and a mutating GET safe.
    """

    def __init__(self, auth, api_base=DEFAULT_API_BASE, level=None, deck=None,
                 timeout=15, charset=None):
        self.auth = auth
        self.api_base = api_base.rstrip("/")
        self.level = level or None
        self.deck_override = deck or None
        self.timeout = timeout
        self.charset = charset
        self._lock = threading.RLock()
        self._session_id = None
        self._deck_names = {}
        self._served_id = None       # the card the board is showing right now
        self._graded_id = None       # and the one it just graded, for retries
        self._payload = None

    # -- upstream ----------------------------------------------------------

    def _call(self, method, path, body=None, retry=True):
        """One authenticated request, re-authenticating once on a 401."""
        try:
            return http_json(self.api_base + path, method=method, body=body,
                             headers={"Authorization": f"Bearer {self.auth.token()}"},
                             timeout=self.timeout)
        except UpstreamError as e:
            if e.status == 401 and retry:
                # The token expired earlier than its own expiry claimed, or was
                # revoked. One forced sign-in, then give up: a loop here would
                # hammer the auth endpoint on a wrong password.
                self.auth.token(force=True)
                return self._call(method, path, body, retry=False)
            raise

    def _start_session(self):
        """Start today's session, or resume it — the backend does both here.

        `study_deck_ids: null` is the daily mix over every unarchived deck,
        which is what a board on a shelf wants: one queue, no deck picker.
        """
        response = self._call("POST", "/study/sessions/start",
                              {"study_deck_ids": None, "mode": "mixed"})
        session = response.get("session") if isinstance(response, dict) else None
        session_id = session.get("id") if isinstance(session, dict) else None
        if not isinstance(session_id, str) or not session_id:
            raise UpstreamError("session start returned no session id")
        self._session_id = session_id
        return response

    def _fetch_session(self):
        """The session's current state, starting the session if there is none.

        A session that has gone away — a new day, a session ended elsewhere —
        comes back as a 404, and the honest response to that is to start the
        new day's session rather than to serve an error the board would badge
        오래됨 for the next five minutes.
        """
        if self._session_id is None:
            return self._start_session()
        try:
            return self._call("GET", self._level_path(
                f"/study/sessions/{self._session_id}/next"))
        except UpstreamError as e:
            if e.status in (404, 410):
                return self._start_session()
            raise

    def _level_path(self, path):
        if not self.level:
            return path
        return f"{path}?level={urllib.parse.quote(self.level)}"

    def _deck_name(self, card, session):
        """The channel line: the deck this card came from.

        The session spans decks, so the card's own deck is the honest answer;
        the session's list is the fallback when there is no card to ask.
        """
        if self.deck_override:
            return self.deck_override
        if not self._deck_names:
            try:
                decks = self._call("GET", "/study/decks")
            except UpstreamError as e:
                print(f"deck names unavailable ({e})", file=sys.stderr)
                decks = []
            if isinstance(decks, list):
                self._deck_names = {
                    d.get("id"): d.get("name") for d in decks
                    if isinstance(d, dict) and isinstance(d.get("name"), str)
                }
        if isinstance(card, dict):
            name = self._deck_names.get(card.get("study_deck_id"))
            if isinstance(name, str) and name:
                return name
        ids = session.get("study_deck_ids") if isinstance(session, dict) else None
        names = [self._deck_names.get(i) for i in ids] if isinstance(ids, list) else []
        return " · ".join(n for n in names if isinstance(n, str) and n)

    def _fetch_comments(self, card_id):
        """The card's thread. A failure here costs the comments, not the card.

        Comments are generated in the background when a session starts, so a
        thread that is not ready yet is normal and must not fail the poll.
        """
        if not card_id:
            return [], 0
        try:
            thread = self._call("GET", f"/cards/{urllib.parse.quote(card_id)}/comments")
        except UpstreamError as e:
            print(f"comments unavailable for {card_id} ({e})", file=sys.stderr)
            return [], 0
        nodes = thread.get("comments") if isinstance(thread, dict) else None
        return flatten_comments(nodes)

    # -- the payload -------------------------------------------------------

    def _present(self, response, now=None):
        """One upstream SessionResponse/AnswerResponse as the board's payload."""
        response = response if isinstance(response, dict) else {}
        session = response.get("session")
        card = response.get("card")
        preview = response.get("rating_preview")
        if card is None:
            # An AnswerResponse names the same two things differently, because
            # from its point of view they are the card AFTER the one answered.
            card = response.get("next_card")
            preview = response.get("next_rating_preview")
        card = card if isinstance(card, dict) else None

        complete = bool(response.get("session_complete"))
        payload = {
            "v": 1,
            "session": flatten_session(session,
                                       deck=self._deck_name(card, session),
                                       level=self.level or LEVEL_ALL,
                                       complete=complete or card is None),
        }
        if card is not None:
            card_id = card.get("id") if isinstance(card.get("id"), str) else ""
            comments, comment_total = self._fetch_comments(card_id)
            payload["card"] = flatten_card(card, preview, comments, comment_total, now)
            self._served_id = card_id or None
        else:
            # No card is not an error and not a blank panel: the board keeps the
            # counters and shows 오늘 학습 완료.
            self._served_id = None

        payload = substitute_missing(payload, self.charset)
        check_glyphs(payload, self.charset)
        self._payload = payload
        return payload

    # -- the two requests --------------------------------------------------

    def poll(self):
        """The card the session is serving right now. Idempotent."""
        with self._lock:
            payload = self._present(self._fetch_session())
            # The retry window closes here. AGAIN puts a card back in the retry
            # pile, so the same id legitimately comes around again — and an id
            # alone cannot tell "the same request twice" from "the same card
            # twice". Only a grade repeated with no poll in between is a retry.
            self._graded_id = None
            return payload

    def grade(self, word, card_id=None, duration_seconds=0):
        """Grade the served card, then serve the next one.

        A `card_id` that names the card just graded is a retried request, not a
        second grade, and gets the payload the first one produced. Without one
        the proxy cannot tell those apart, which is the whole reason the board
        sends the id it is showing.
        """
        with self._lock:
            if card_id and card_id == self._graded_id and self._payload is not None:
                return self._payload
            served = self._served_id
            if served is None:
                raise ConflictError("no card is being served")
            if card_id and card_id != served:
                raise ConflictError(f"serving {served}, not {card_id}")
            if self._session_id is None:
                raise ConflictError("no session")

            response = self._call("POST", self._level_path(
                f"/study/sessions/{self._session_id}/answer"),
                {"study_card_id": served, "rating": word,
                 "duration_seconds": max(0, int(duration_seconds))})
            self._graded_id = served
            return self._present(response)


class FixtureProxy:
    """The committed fixture, served with the same two calls as a real session.

    This is what makes the board testable with no account: `--offline` needs no
    network, no credentials and no card due. A grade is accepted and goes
    nowhere — there is no scheduler behind a fixture — because refusing it
    would badge the panel 오래됨 for pressing a key that works everywhere else.
    """

    def __init__(self, path=DEFAULT_FIXTURE, charset=None):
        self.path = path
        self.charset = charset
        with open(path, "r", encoding="utf-8") as source:
            payload = json.load(source)
        if not isinstance(payload, dict):
            raise ValueError(f"{path}: the fixture's root must be an object")
        self._payload = substitute_missing(payload, charset)
        check_glyphs(self._payload, charset)

    def poll(self):
        """The fixture, unchanged. Always the same card, so kanji_hash() matches
        and a board left running against --offline never refreshes its panel."""
        return self._payload

    def grade(self, word, card_id=None, duration_seconds=0):
        """Accept a rating and discard it, then answer with the same card.

        There is no scheduler behind a fixture, so there is nothing to record
        and nothing to advance to. Answering 200 rather than an error is
        deliberate: KEY1 must feel identical in --offline, and a 502 here would
        badge a perfectly healthy board 오래됨.
        """
        print(f"offline: grade '{word}' accepted and discarded", file=sys.stderr)
        return self._payload


# ---------------------------------------------------------------------------
# Credentials
# ---------------------------------------------------------------------------

CONFIG_KEYS = ("supabase_url", "supabase_key", "email", "password", "api_base")

ENV_KEYS = {
    "supabase_url": "KANJIS_SUPABASE_URL",
    "supabase_key": "KANJIS_SUPABASE_KEY",
    "email": "KANJIS_EMAIL",
    "password": "KANJIS_PASSWORD",
    "api_base": "KANJIS_API_BASE",
}

CREDENTIALS_HELP = """\
Credentials come from the environment, or from a JSON file named with --config,
and the environment wins where both supply a value:

  KANJIS_SUPABASE_URL   https://<project-ref>.supabase.co
  KANJIS_SUPABASE_KEY   the publishable ("anon") key
  KANJIS_EMAIL          the account's email
  KANJIS_PASSWORD       its password
  KANJIS_API_BASE       optional, default https://api.kanjis.ai/api/v1

The file uses the same names lowercased without the prefix. Nothing is ever
taken from the command line: a command line is in the shell history and in ps.

--offline needs none of it."""


class ConfigError(Exception):
    """Credentials that cannot be used, said in a way that names the fix."""


def load_credentials(path=None, environ=None):
    """The five settings, from the file then the environment. Never logged."""
    environ = os.environ if environ is None else environ
    values = {key: "" for key in CONFIG_KEYS}

    if path:
        expanded = os.path.expanduser(path)
        try:
            with open(expanded, "r", encoding="utf-8") as source:
                data = json.load(source)
        except OSError as e:
            raise ConfigError(f"config unreadable: {e}") from None
        except ValueError as e:
            raise ConfigError(f"config is not JSON: {e}") from None
        if not isinstance(data, dict):
            raise ConfigError("config must be a JSON object")
        for key in CONFIG_KEYS:
            value = data.get(key)
            if isinstance(value, str) and value.strip():
                values[key] = value.strip()
        warn_if_world_readable(expanded)

    for key, name in ENV_KEYS.items():
        value = environ.get(name)
        if isinstance(value, str) and value.strip():
            values[key] = value.strip()

    values["api_base"] = values["api_base"] or DEFAULT_API_BASE
    missing = [ENV_KEYS[k] for k in ("supabase_url", "supabase_key", "email", "password")
               if not values[k]]
    if missing:
        raise ConfigError("missing " + ", ".join(missing))
    values["supabase_url"] = values["supabase_url"].rstrip("/")
    return values


def warn_if_world_readable(path):
    """A password on disk that the rest of the machine can read is worth saying.

    Said rather than enforced: refusing to start would strand somebody whose
    home directory is already private for other reasons, and this is their
    machine.
    """
    try:
        mode = os.stat(path).st_mode
    except OSError:
        return
    if mode & (stat.S_IRGRP | stat.S_IROTH):
        print(f"warning: {path} is readable beyond its owner and holds a password "
              f"(chmod 600 {path})", file=sys.stderr)


# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    """The one endpoint the board talks to.

    Every response is JSON, including the failures — the board's parser has one
    shape it understands and `send_error`'s HTML page is not it. The status code
    carries the distinction the firmware acts on: 2xx is a payload, 409 means
    "that is not the card I am serving", 502 means the upstream is unreachable
    and the board should keep what it has. See docs/kanji-contract.md.

    Grading rides on GET rather than POST because the device's HTTP port
    (components/vault_core/http_port.h) is a single function with three
    implementations, and keeping it that way is what lets the host tests BE the
    network. Safe here because the service is single-client, LAN-only, and
    refuses a grade for any card but the one it just served.
    """

    proxy = None

    def _json(self, status, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        # The board polls one host forever; keeping the socket saves a connect
        # per poll.
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        """Poll, or grade-then-poll: `?grade=` is what separates the two.

        The board sends the same request either way, so there is one code path
        and one response shape, and the answer to a grade IS the next card.
        """
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path not in PATHS:
            # JSON and not send_error()'s HTML page, for the same reason every
            # other failure here is JSON: one shape for the board, and one shape
            # for whoever is holding a curl against this and the mock in turn.
            self._json(404, {"error": "not_found", "expected": list(PATHS)})
            return
        query = urllib.parse.parse_qs(parsed.query)
        raw_grade = query.get("grade", [None])[0]
        card_id = query.get("card", [None])[0]

        try:
            if raw_grade is None:
                payload = self.proxy.poll()
            else:
                grade = parse_grade(raw_grade)
                if grade is None:
                    self._json(400, {"error": "bad_grade",
                                     "expected": list(GRADE_WORDS)})
                    return
                payload = self.proxy.grade(grade, card_id)
        except ConflictError as e:
            # 409 and not 400: the request is well formed, it is about a card
            # this proxy is not showing. That distinction is what keeps a
            # mutating GET safe on an unauthenticated LAN.
            self._json(409, {"error": "not_serving_that_card", "detail": str(e)})
            return
        except UpstreamError as e:
            # The board keeps its last card and badges it 오래됨, which beats a
            # blank panel and is exactly what a 502 should mean to it.
            self._json(502, {"error": "upstream", "detail": str(e)})
            return

        self._json(200, payload)

    def log_message(self, fmt, *args):
        """Log to stderr, so stdout stays the card description this prints on
        every poll. Overridden only to drop BaseHTTPRequestHandler's timestamp,
        which is noise next to one line per five minutes."""
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------

def describe(payload):
    """One line for the console: what the board is about to be told."""
    session = payload.get("session", {}) if isinstance(payload, dict) else {}
    card = payload.get("card") if isinstance(payload, dict) else None
    where = f"{session.get('track', 0)}/{session.get('track_total', 0)}"
    if not isinstance(card, dict):
        return (f"{session.get('deck', '')} [{session.get('level', '')}] {where} "
                f"— no card (session complete)")
    fsrs = card.get("fsrs", {})
    return (f"{session.get('deck', '')} [{session.get('level', '')}] {where} "
            f"— {card.get('front', '')} ({card.get('reading', '')}) "
            f"{fsrs.get('state_label', '')}, 다음 {fsrs.get('due', '') or '—'}, "
            f"댓글 {card.get('comment_total', 0)}")


def build_proxy(args, charset):
    """Pick the card source: the committed fixture, or a real kanjis.ai session.

    Credentials are read here and nowhere else, and only on the path that needs
    them — `--offline` and `--fixture` never touch load_credentials(), so the
    board can be brought up end to end by someone with no account.

    Raises ConfigError with something actionable rather than a traceback: the
    usual failure is a missing fixture or an unset environment variable, and
    both have a one-line fix worth printing.
    """
    if args.offline or args.fixture:
        path = args.fixture or DEFAULT_FIXTURE
        if not os.path.exists(path):
            raise ConfigError(
                f"no fixture at {path} — run python3 tools/mock_kanji_server.py "
                f"--write-fixture, or name one with --fixture")
        return FixtureProxy(path, charset=charset)

    credentials = load_credentials(args.config)
    auth = SupabaseAuth(credentials["supabase_url"], credentials["supabase_key"],
                        credentials["email"], credentials["password"],
                        timeout=args.timeout)
    return StudyProxy(auth, api_base=credentials["api_base"], level=args.level,
                      deck=args.deck, timeout=args.timeout, charset=charset)


def main(argv=None):
    """Parse the arguments, build a proxy, and serve until interrupted.

    Returns a process exit status rather than calling sys.exit, so the tests can
    drive it. `--check` is the one mode that does not serve: it logs in, fetches
    one card, prints it and exits, which is how you find out whether the
    credentials work without a board in the loop.
    """
    ap = argparse.ArgumentParser(
        description=__doc__.split("Credentials")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=CREDENTIALS_HELP,
    )
    ap.add_argument("--port", type=int, default=8123)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--config", help="JSON file holding the credentials")
    ap.add_argument("--level", choices=["N1", "N2", "N3", "N4", "N5"],
                    help="study one JLPT level (default: every level)")
    ap.add_argument("--deck", help="deck name shown on the caption line "
                                   "(default: the card's own deck)")
    ap.add_argument("--timeout", type=float, default=15.0,
                    help="seconds to wait on api.kanjis.ai (default: 15)")
    ap.add_argument("--check", action="store_true",
                    help="log in, fetch one card, print it, and exit")
    ap.add_argument("--offline", action="store_true",
                    help="serve the committed fixture and never touch the network")
    ap.add_argument("--fixture", help="serve this payload instead (implies --offline)")
    ap.add_argument("--no-glyph-check", action="store_true",
                    help="skip warning about characters the board cannot draw")
    args = ap.parse_args(argv)

    charset = None if args.no_glyph_check else load_device_charset()

    try:
        proxy = build_proxy(args, charset)
    except ConfigError as e:
        print(f"{e}\n\n{CREDENTIALS_HELP}", file=sys.stderr)
        return 2

    if args.check:
        source = "fixture" if isinstance(proxy, FixtureProxy) else "api.kanjis.ai"
        print(f"source: {source}")
        try:
            payload = proxy.poll()
        except UpstreamError as e:
            print(f"upstream failed: {e}", file=sys.stderr)
            return 1
        print(json.dumps(payload, ensure_ascii=False, indent=2))
        missing = check_glyphs(payload, charset, warn=False)
        if charset is None:
            print("glyphs: not checked")
        elif missing:
            # Substitution already happened, so this cannot normally fire; if it
            # does, the payload and the font disagree and that is worth a shout.
            print(f"glyphs: {len(missing)} still missing after substitution: "
                  f"{' '.join(sorted(missing))}", file=sys.stderr)
            return 1
        else:
            print("glyphs: every character in this payload has a face")
        print(describe(payload))
        return 0

    # Fetch once up front so a slow login happens at startup rather than inside
    # the board's first poll, which would look like an unreachable server.
    try:
        print(describe(proxy.poll()))
    except UpstreamError as e:
        print(f"upstream failed: {e}", file=sys.stderr)
        return 1

    Handler.proxy = proxy
    server = HTTPServer((args.host, args.port), Handler)
    print(f"serving it on http://{args.host}:{args.port}/kanji.json")
    if isinstance(proxy, FixtureProxy):
        print(f"offline — {proxy.path}, and a grade goes nowhere")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
