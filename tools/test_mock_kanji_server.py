#!/usr/bin/env python3
"""
Tests for tools/mock_kanji_server.py, against the server actually running.

    python3 tools/test_mock_kanji_server.py

No framework and no fixtures of its own: it binds an ephemeral port, fetches the
card the way the board does, and checks the response against
docs/kanji-contract.md field by field. The reference producer is the only
written-down copy of the wire format that can be executed, so the thing worth
pinning is that it still answers what the contract says it answers.

The byte budgets are not repeated here. They are read out of
components/vault_core/include/kanji_model.h, which is the struct the parser
copies into — a field that outgrows its buffer loses its tail on the glass, so
the test that catches it has to follow the header rather than a second list that
can rot.
"""

import json
import os
import re
import sqlite3
import sys
import threading
import urllib.error
import urllib.request
from http.server import HTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mock_kanji_server as mock  # noqa: E402
from gen_fonts import symbol_set  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL_H = os.path.join(ROOT, "components", "vault_core", "include", "kanji_model.h")

# The catalog the card was taken from. It is a different repository and is not
# required to run these tests, but where it exists the card is checked against
# it: "real data, not invented" is a claim, and this is the only thing that can
# check it.
CATALOG = os.environ.get(
    "KANJIS_CATALOG",
    os.path.expanduser("~/Documents/kanjis-backend/data/kanjis-backend.sqlite3"))
CARD_ID = "f00c539e-23f9-4294-bee1-c642189b105f"

FAILURES = []
CHECKS = [0]


def check(cond, label, got=None, want=None):
    CHECKS[0] += 1
    if not cond:
        detail = "" if got is None and want is None else f"  (got {got!r}, want {want!r})"
        FAILURES.append(label + detail)
        print(f"  FAIL {label}{detail}")


def eq(got, want, label):
    check(got == want, label, got, want)


# ---------------------------------------------------------------------------
# The server, and the board's half of the conversation
# ---------------------------------------------------------------------------

class Quiet(mock.Handler):
    """The real handler, minus the access log — 30 lines of noise around a
    result line is a test whose failures get skimmed past."""

    def log_message(self, fmt, *args):
        pass


class Server:
    """mock_kanji_server on a port the OS picked, for the length of a `with`."""

    def __enter__(self):
        self.srv = HTTPServer(("127.0.0.1", 0), Quiet)
        self.port = self.srv.server_address[1]
        self.thread = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.srv.shutdown()
        self.srv.server_close()
        self.thread.join()

    def url(self, path="/kanji.json", query=""):
        return f"http://127.0.0.1:{self.port}{path}{query}"

    def get(self, path="/kanji.json", query=""):
        """(status, body bytes). A 4xx is an answer here, not an exception."""
        try:
            with urllib.request.urlopen(self.url(path, query)) as r:
                return r.status, r.read(), r.headers
        except urllib.error.HTTPError as e:
            return e.code, e.read(), e.headers


def caps():
    """The #define'd buffer sizes from kanji_model.h.

    A cap in the contract is stated in bytes-without-the-NUL; the header states
    the buffer, so everything here is size - 1.
    """
    with open(MODEL_H, encoding="utf-8") as f:
        src = f.read()
    return {name: int(value)
            for name, value in re.findall(r"#define\s+(KANJI_\w+)\s+(\d+)", src)}


CAP = caps()


def fits(text, buffer_name, label):
    limit = CAP[buffer_name] - 1
    size = len(text.encode("utf-8"))
    check(size <= limit, f"{label} fits {buffer_name} ({size} of {limit} bytes)")


def strings(node, out):
    """Every string that the payload carries, keys excluded."""
    if isinstance(node, str):
        out.append(node)
    elif isinstance(node, list):
        for item in node:
            strings(item, out)
    elif isinstance(node, dict):
        for value in node.values():
            strings(value, out)
    return out


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_the_card_is_served_as_utf8_json(server):
    status, body, headers = server.get()
    eq(status, 200, "GET /kanji.json is a 200")
    eq(headers["Content-Type"], "application/json; charset=utf-8",
       "the response declares UTF-8 JSON")
    eq(int(headers["Content-Length"]), len(body),
       "Content-Length matches the body the board will read")
    # The device reads the socket into a buffer and hands cJSON the bytes; if
    # those bytes are not decodable UTF-8, every Korean label is a tofu box.
    body.decode("utf-8")
    check(True, "the body decodes as UTF-8")
    eq(server.get(path="/")[0], 200, "the bare root serves the same card")


def test_every_documented_field_is_present_with_the_documented_type(server):
    payload = json.loads(server.get()[1])
    eq(sorted(payload), ["card", "session", "v"], "the root has exactly v/session/card")
    check(isinstance(payload["v"], int), "v is an integer")

    s = payload["session"]
    eq(sorted(s), ["complete", "deck", "left_new", "left_review", "level", "retry",
                   "reviewed_today", "streak", "track", "track_total"],
       "session has exactly its documented fields")
    for key in ("deck", "level"):
        check(isinstance(s[key], str), f"session.{key} is a string")
    for key in ("streak", "reviewed_today", "left_new", "left_review", "retry",
                "track", "track_total"):
        check(isinstance(s[key], int) and not isinstance(s[key], bool),
              f"session.{key} is an integer", s[key])
        check(0 <= s[key] <= 9999, f"session.{key} is inside the parser's clamp", s[key])
    check(isinstance(s["complete"], bool), "session.complete is a bool")
    check(s["track"] <= s["track_total"], "track is inside track_total",
          s["track"], s["track_total"])

    c = payload["card"]
    eq(sorted(c), ["comment_total", "comments", "description", "examples", "front",
                   "fsrs", "hook_body", "hook_title", "id", "level", "parts",
                   "preview", "reading", "senses"],
       "card has exactly its documented fields")
    for key in ("id", "front", "reading", "level", "description", "hook_title",
                "hook_body"):
        check(isinstance(c[key], str) and c[key], f"card.{key} is a non-empty string")
    check(all(isinstance(x, str) and x for x in c["senses"]),
          "card.senses is a list of non-empty strings")
    for name, keys in (("examples", ("text", "reading", "gloss")),
                       ("parts", ("glyph", "meaning", "reading"))):
        for row in c[name]:
            eq(sorted(row), sorted(keys), f"a {name} row has exactly {keys}")
            check(all(isinstance(row[k], str) and row[k] for k in keys),
                  f"every {name} field is a non-empty string", row)
    for row in c["comments"]:
        eq(sorted(row), ["author", "body", "likes"], "a comment row has exactly its fields")
        check(isinstance(row["likes"], int) and row["likes"] >= 0,
              "a comment's likes is a count", row["likes"])
    check(isinstance(c["comment_total"], int), "card.comment_total is an integer")
    check(c["comment_total"] >= len(c["comments"]),
          "comment_total is never less than the comments that came",
          c["comment_total"], len(c["comments"]))

    f = c["fsrs"]
    eq(sorted(f), ["difficulty_pct", "due", "lapses", "reps", "stability_days",
                   "state", "state_label"],
       "fsrs has exactly its documented fields")
    check(f["state"] in ("new", "learning", "review", "relearning"),
          "fsrs.state is one of the four wire words", f["state"])
    check(isinstance(f["state_label"], str) and f["state_label"],
          "fsrs.state_label is the Korean the sheet prints")
    check(isinstance(f["due"], str), "fsrs.due is a pre-rendered span")
    for key in ("reps", "lapses"):
        check(isinstance(f[key], int) and 0 <= f[key] <= 99999,
              f"fsrs.{key} is inside the parser's clamp", f[key])
    # -1 is "not scheduled yet" and the sheet prints it as —; 0 is a real
    # same-day interval. Anything below -1 is neither.
    check(isinstance(f["stability_days"], int) and f["stability_days"] >= -1,
          "fsrs.stability_days is a day count or -1", f["stability_days"])
    check(isinstance(f["difficulty_pct"], int) and -1 <= f["difficulty_pct"] <= 100,
          "fsrs.difficulty_pct is a percentage or -1", f["difficulty_pct"])


def test_the_four_rating_previews_are_all_worded(server):
    preview = json.loads(server.get()[1])["card"]["preview"]
    eq(sorted(preview), ["again", "easy", "good", "hard"],
       "preview has exactly the four ratings")
    for key in mock.GRADES:
        check(isinstance(preview[key], str) and preview[key],
              f"preview.{key} is a pre-rendered span, not a timestamp", preview.get(key))
        # The board has no RTC. A digit-and-colon span here would mean the proxy
        # sent an ISO date and the panel would print it raw.
        check("T" not in preview[key] and ":" not in preview[key],
              f"preview.{key} is not an ISO timestamp", preview[key])


def test_no_field_outgrows_the_struct_it_is_copied_into(server):
    payload = json.loads(server.get()[1])
    s, c = payload["session"], payload["card"]

    fits(s["deck"], "KANJI_DECK_MAX", "session.deck")
    fits(s["level"], "KANJI_LABEL_MAX", "session.level")
    fits(c["id"], "KANJI_ID_MAX", "card.id")
    fits(c["front"], "KANJI_FRONT_MAX", "card.front")
    fits(c["reading"], "KANJI_READING_MAX", "card.reading")
    fits(c["level"], "KANJI_LABEL_MAX", "card.level")
    fits(c["description"], "KANJI_BODY_MAX", "card.description")
    fits(c["hook_title"], "KANJI_LABEL_MAX", "card.hook_title")
    fits(c["hook_body"], "KANJI_BODY_MAX", "card.hook_body")
    for i, sense in enumerate(c["senses"]):
        fits(sense, "KANJI_SENSE_MAX", f"senses[{i}]")
    for i, ex in enumerate(c["examples"]):
        fits(ex["text"], "KANJI_FRONT_MAX", f"examples[{i}].text")
        fits(ex["reading"], "KANJI_READING_MAX", f"examples[{i}].reading")
        fits(ex["gloss"], "KANJI_SENSE_MAX", f"examples[{i}].gloss")
    for i, part in enumerate(c["parts"]):
        fits(part["glyph"], "KANJI_FRONT_MAX", f"parts[{i}].glyph")
        fits(part["meaning"], "KANJI_SENSE_MAX", f"parts[{i}].meaning")
        fits(part["reading"], "KANJI_READING_MAX", f"parts[{i}].reading")
    for i, com in enumerate(c["comments"]):
        fits(com["author"], "KANJI_AUTHOR_MAX", f"comments[{i}].author")
        fits(com["body"], "KANJI_COMMENT_MAX", f"comments[{i}].body")
    for key, span in c["preview"].items():
        fits(span, "KANJI_LABEL_MAX", f"preview.{key}")
    for key in ("state", "state_label", "due"):
        fits(c["fsrs"][key], "KANJI_LABEL_MAX", f"fsrs.{key}")


def test_no_list_outgrows_the_rows_the_panel_has(server):
    c = json.loads(server.get()[1])["card"]
    # The parser drops the overflow rather than failing, so an over-long list
    # here is not a crash — it is content the reference producer emits and the
    # device silently never shows.
    for name, cap in (("senses", "KANJI_SENSES_MAX"),
                      ("examples", "KANJI_EXAMPLES_MAX"),
                      ("parts", "KANJI_PARTS_MAX"),
                      ("comments", "KANJI_COMMENTS_MAX")):
        check(0 < len(c[name]) <= CAP[cap],
              f"card.{name} fits {cap} ({CAP[cap]})", len(c[name]))
    check(not any(ex["text"] == c["front"] for ex in c["examples"]),
          "no 예문 row merely repeats the headword")


def test_a_grade_advances_the_session_and_returns_a_card(server):
    base = json.loads(server.get()[1])["session"]
    for grade in mock.GRADES:
        status, body, _ = server.get(query=f"?grade={grade}")
        eq(status, 200, f"?grade={grade} is a 200")
        graded = json.loads(body)
        eq(graded["session"]["reviewed_today"], base["reviewed_today"] + 1,
           f"?grade={grade} counts a review")
        eq(graded["session"]["track"], base["track"] + 1,
           f"?grade={grade} advances the position in today's queue")
        # The board draws the next card from the same response. A grade that
        # answered without one would blank the panel on every keypress.
        check(graded["card"]["front"], f"?grade={grade} still answers with a card")
    again = json.loads(server.get(query="?grade=again")[1])["session"]
    good = json.loads(server.get(query="?grade=good")[1])["session"]
    eq(again["retry"], base["retry"] + 1, "다시 puts the card back in the retry queue")
    eq(again["left_review"], base["left_review"], "다시 does not shorten 복습할")
    eq(good["left_review"], base["left_review"] - 1, "보통 takes the card off 복습할")
    eq(good["retry"], base["retry"], "보통 does not touch 다시 볼")


def test_a_grade_the_backend_does_not_know_is_a_400(server):
    """Refused, and refused in the shape kanji_server.py refuses it.

    The status alone is not the contract. This is the reference producer, so a
    failure here is what the proxy's failure has to look like — and asserting
    only the code is how the two came to answer the same refusal with JSON on
    one side and Python's HTML error page on the other.
    """
    for query in ("?grade=maybe", "?grade=AGAIN", "?grade=1", "?grade="):
        status, body, headers = server.get(query=query)
        eq(status, 400, f"{query} is refused")
        eq(headers["Content-Type"], "application/json; charset=utf-8",
           f"{query} is refused as JSON, not as an HTML error page")
        eq(json.loads(body), {"error": "bad_grade", "expected": list(mock.GRADES)},
           f"{query} names the error and the four words it would have taken")
    # Refused, and refused without a card: a 400 carrying a payload would be
    # parsed by a lenient client as a successful grade.
    body = server.get(query="?grade=maybe")[1]
    check(b"front" not in body, "a refused grade answers with no card")

    status, body, headers = server.get(path="/nope.json")
    eq(status, 404, "an unknown path is a 404")
    eq(headers["Content-Type"], "application/json; charset=utf-8",
       "and it is a 404 in JSON too")
    eq(json.loads(body), {"error": "not_found", "expected": list(mock.PATHS)},
       "naming the paths this does serve")


def test_a_plain_poll_never_changes_what_it_answers(server):
    first = server.get()[1]
    server.get(query="?grade=easy")
    server.get(query="?grade=again")
    eq(server.get()[1], first,
       "grading does not mutate the reference payload behind the fixture")


def test_the_committed_fixture_is_what_the_server_serves(server):
    with open(mock.FIXTURE, "rb") as f:
        committed = f.read()
    served = server.get()[1]
    eq(served, committed,
       "components/vault_core/test/host/fixtures/kanji.json is stale — "
       "run python3 tools/mock_kanji_server.py --write-fixture")
    # The host tests parse the file, the board parses the response. Identical
    # bytes is what makes the first a test of the second.
    eq(mock.encode(mock.payload()), committed, "--write-fixture writes those same bytes")


def test_every_character_in_the_payload_is_drawable(server):
    """Nothing may reach the glass that the shipped faces do not carry.

    The faces hold 완성형 Hangul, ASCII, kana, JIS X 0208 kanji and the curated
    punctuation of ui_strings.h — and nothing else. A codepoint outside that is
    a tofu box after a two-second refresh, where nobody is watching, so the
    check belongs on a laptop with the offending character printed.
    """
    covered = symbol_set()
    used = set("".join(strings(json.loads(server.get()[1]), [])))
    missing = sorted(c for c in used if c not in covered)
    eq(missing, [], "every character of the payload exists in the device font")


def test_the_card_is_the_catalog_row_and_not_a_rewrite():
    """The senses, examples, shape explanation and hint come from the catalog.

    Skipped where the catalog is not checked out — it is another repository —
    but where it is, this is the only thing standing between "real data" and a
    plausible-sounding paraphrase.
    """
    if not os.path.exists(CATALOG):
        print(f"  note: catalog absent, card-vs-catalog check skipped ({CATALOG})")
        return
    db = sqlite3.connect(f"file:{CATALOG}?mode=ro", uri=True)
    try:
        row = db.execute("SELECT c.front, c.back, c.hint, c.tags_json, d.name "
                         "FROM card_templates c JOIN deck_templates d "
                         "ON d.id = c.template_deck_id WHERE c.id = ?",
                         (CARD_ID,)).fetchone()
    finally:
        db.close()
    if row is None:
        print(f"  note: {CARD_ID} not in this catalog, card check skipped")
        return

    front, back, hint, tags, deck = row
    back, hint, tags = json.loads(back), json.loads(hint), json.loads(tags)
    card = mock.card()
    kun = back["kun_yomi"][0]

    eq(card["front"], front, "the headword is the catalog's")
    eq(card["reading"], kun["reading"], "the reading is the catalog's")
    eq(card["senses"], back["meaning"]["senses"], "every sense is the catalog's")
    eq(card["examples"], [e for e in kun["examples"] if e["text"] != front],
       "the examples are the catalog's, less the one repeating the headword")
    eq(card["description"], back["shape_explanation"],
       "the shape explanation is carried whole")
    eq(card["hook_body"], hint["reason"], "the hint's reason is carried whole")
    eq(card["parts"], [{"glyph": s["kanji"], "meaning": s["meaning"],
                        "reading": s["reading"]} for s in hint["shapes"]],
       "the parts are the hint's shapes")
    eq(mock.session()["deck"], deck, "the deck name is the catalog's")
    check(card["level"] in tags, "the level is one of the card's tags", card["level"], tags)


def main():
    with Server() as server:
        test_the_card_is_served_as_utf8_json(server)
        test_every_documented_field_is_present_with_the_documented_type(server)
        test_the_four_rating_previews_are_all_worded(server)
        test_no_field_outgrows_the_struct_it_is_copied_into(server)
        test_no_list_outgrows_the_rows_the_panel_has(server)
        test_a_grade_advances_the_session_and_returns_a_card(server)
        test_a_grade_the_backend_does_not_know_is_a_400(server)
        test_a_plain_poll_never_changes_what_it_answers(server)
        test_the_committed_fixture_is_what_the_server_serves(server)
        test_every_character_in_the_payload_is_drawable(server)
    test_the_card_is_the_catalog_row_and_not_a_rewrite()

    print(f"\nmock_kanji_server: {CHECKS[0]} checks, {len(FAILURES)} failures")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
