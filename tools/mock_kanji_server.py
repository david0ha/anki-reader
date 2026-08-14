#!/usr/bin/env python3
"""
The kanji study contract, as a runnable server.

This is the reference implementation of what the device fetches — the thing
tools/kanji_server.py has to imitate once it is holding a real kanjis.ai
session. It exists for three jobs:

  1. Point a board (or the simulator) at a URL and watch the whole study loop
     work — poll, reveal, grade — with no account, no bearer token and no
     network that has to reach the internet.
  2. Produce the committed fixture the host tests parse
     (components/vault_core/test/host/fixtures/kanji.json).
  3. Pin the contract. The payload here and kanji_mock.c's built-in demo card
     are asserted to be identical by test_kanji_mock.c, so the wire format and
     the screen the board shows when no URL is configured cannot drift apart.

The card is not invented. It is card_templates row
f00c539e-23f9-4294-bee1-c642189b105f of the shipped catalog — 会う, deck "JLPT
N5 Vocabulary" — with its senses, its examples, its shape explanation and its
hint carried across verbatim; the whole of both paragraphs fits inside
KANJI_BODY_MAX, so nothing here is a truncation of anything. What a catalog row
cannot hold is written here instead: the FSRS state, the four rating previews,
the comments and the session counters all belong to a learner and a clock, not
to a card.

Usage
-----
    python3 tools/mock_kanji_server.py                 # serve on :8123
    python3 tools/mock_kanji_server.py --port 9000
    python3 tools/mock_kanji_server.py --dump          # print the payload
    python3 tools/mock_kanji_server.py --write-fixture # refresh the test fixture

    curl 'http://localhost:8123/kanji.json'
    curl 'http://localhost:8123/kanji.json?grade=good'

Then point the board at http://<this machine>.local:8123/kanji.json from the
captive portal. See docs/kanji-contract.md for the field-by-field contract.
"""

import argparse
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE = os.path.join(ROOT, "components", "vault_core", "test", "host",
                       "fixtures", "kanji.json")

# The wire words of kanji_grade_t, in the order the grade dock draws them. The
# proxy forwards these to the backend as-is, so a typo here is a typo there.
GRADES = ("again", "hard", "good", "easy")

# The two paths this serves. `/` is here because a URL typed into the captive
# portal without its file name is the most common way to get this wrong.
PATHS = ("/kanji.json", "/")


def session(grade=None):
    """The session block, optionally advanced by one graded card.

    A grade moves counters rather than cards: this mock owns exactly one card,
    so "the next card" is the same card again. What a board watching the LAN
    needs to see is that the numbers moved at all — that the GET reached a
    scheduler and was not swallowed.

    `again` is the one rating that does not shorten the queue. The card comes
    back before the session ends, so it joins 다시 볼 instead of leaving 복습할;
    a mock that decremented on every rating would teach the panel that a
    forgotten card is a finished card.
    """
    s = {
        "deck": "JLPT N5 Vocabulary",
        "level": "N5",
        "streak": 12,
        "reviewed_today": 34,
        "left_new": 7,
        "left_review": 18,
        "retry": 2,
        "track": 35,
        "track_total": 60,
        "complete": False,
    }
    if grade is None:
        return s
    s["reviewed_today"] += 1
    s["track"] = min(s["track"] + 1, s["track_total"])
    if grade == "again":
        s["retry"] += 1
    else:
        s["left_review"] = max(s["left_review"] - 1, 0)
    return s


def card():
    """The catalog row, flattened the way the proxy has to flatten it.

    `back` and `hint` are JSON inside a TEXT column upstream; the board gets one
    flat object because a second cJSON pass per card buys it nothing. The one
    example the catalog carries for the headword itself (会う / あう / 만나다) is
    dropped: on a 648x480 panel a 예문 row that repeats the hero is a row that
    says nothing.
    """
    return {
        "id": "f00c539e-23f9-4294-bee1-c642189b105f",
        "front": "会う",
        "reading": "あう",
        "level": "N5",
        "senses": ["만나다", "대면하다", "우연히 만나다"],
        "examples": [
            {"text": "出会う", "reading": "であう", "gloss": "우연히 만나다"},
            {"text": "出会い", "reading": "であい", "gloss": "만남"},
        ],
        "description":
            "会는 사람들이 모여 서로 말하고 교류하는 모습을 바탕으로 한 "
            "글자입니다. 위쪽은 모임을 나타내는 형태이고, 아래쪽은 "
            "입(말함)을 연상시키며 '사람들이 모여 말하는(만나는) 모습'에서 "
            "'만나다'라는 뜻이 생겼습니다.",
        # The row has no `principle`, so the sheet's own default heading stands
        # in — ui_strings.h's S_HOOK_DEFAULT, spelled the same on both sides.
        "hook_title": "기억 힌트",
        "hook_body":
            "会는 사람들이 모여 입으로 말을 주고받는 모습을 형상화한 "
            "글자입니다. 위의 구성은 '모임'을, 아래의 모양은 '말함/입'을 "
            "나타내어 '사람들이 모인다/만난다'는 의미가 됩니다.",
        "parts": [
            {"glyph": "会", "meaning": "모이다, 만나다", "reading": "あう (훈독)"},
        ],
        # Invented, because template_card_comments is empty in the shipped
        # catalog and the 댓글 sheet has to be drawn against something. Two of a
        # claimed fourteen: the sheet shows what fits and says 외 12 for the
        # rest, which is only exercised when comment_total exceeds the rows.
        "comments": [
            {"author": "카나 선생",
             "body": "「会う」는 사람을 만날 때 씁니다. 우연히 마주친 경우에는 "
                     "「出会う」를 더 자주 씁니다.",
             "likes": 12},
            {"author": "유키",
             "body": "「友達に会う」처럼 조사는 に를 씁니다. を를 쓰면 어색하게 "
                     "들려요.",
             "likes": 5},
        ],
        "comment_total": 14,
        # A card mid-schedule on purpose. The -1 that means "never scheduled"
        # is the interesting value, but it is the one a demo card must not
        # carry: the FSRS sheet would print — for every number it exists to
        # explain. test_kanji_parse.c owns the -1 case instead.
        "fsrs": {
            "state": "review",
            "state_label": "복습",
            "due": "9일 뒤",
            "reps": 5,
            "lapses": 1,
            "stability_days": 9,
            "difficulty_pct": 47,
        },
        # Pre-rendered spans, not timestamps: the board has no RTC and cannot
        # turn an ISO date into "9일 뒤" even in principle. `good` matches
        # fsrs.due because that is what the scheduler last chose.
        "preview": {
            "again": "10분 뒤",
            "hard": "4일 뒤",
            "good": "9일 뒤",
            "easy": "21일 뒤",
        },
    }


def payload(grade=None):
    """The canonical payload. Must stay identical to kanji_mock.c."""
    return {"v": 1, "session": session(grade), "card": card()}


def encode(obj):
    """The exact bytes that go both into the fixture and onto the wire.

    The fixture is not a prettied copy of the response, it *is* the response.
    test_kanji_mock.c parses the committed file and fingerprints it against the
    C demo card, so any difference between file and response — key order,
    escaping, indentation — would be a difference no test can see.

    ensure_ascii=False because the device is fed UTF-8; \\uXXXX escapes would
    only exercise cJSON's unescaper and hide what the payload actually says
    from anyone reading the fixture.
    """
    return (json.dumps(obj, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    """The contract in docs/kanji-contract.md, from a fixed payload.

    This is the reference producer: it answers the same paths, the same field
    names and the same failure shapes as tools/kanji_server.py, but from one
    committed card instead of a live kanjis.ai session. That makes it the thing
    a firmware change is tested against — no account, no network, no card due —
    and the thing kanji_server.py is diffed against when the two drift.

    It is also the source of truth for components/vault_core/kanji_mock.c: the
    same bytes go into the fixture the host tests parse, and test_kanji_mock.c
    fails if the built-in demo card and this payload stop describing the same
    card.
    """

    def _json(self, status, body):
        """A failure in the shape kanji_server.py's Handler answers with.

        send_error() would answer with Python's HTML error page, and a producer
        that fails differently from the proxy is a producer nobody can use to
        find out how a failure looks. Whoever is holding a curl against this is
        being taught what the other server does.
        """
        encoded = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(encoded)

    def do_GET(self):
        """Poll, or grade-then-poll, exactly as the real proxy does.

        A grade advances the session counters and comes back with a card, so the
        board's whole KEY1 path — submit, wait, redraw on the reply — is
        exercised without an account. Unlike the real proxy this never 409s:
        there is only ever one card to be serving.
        """
        parsed = urlparse(self.path)
        if parsed.path not in PATHS:
            self._json(404, {"error": "not_found", "expected": list(PATHS)})
            return

        # keep_blank_values, so that a "?grade=" from a shell that lost its
        # variable is a 400 rather than a silent plain poll.
        values = parse_qs(parsed.query, keep_blank_values=True).get("grade", [])
        grade = values[-1] if values else None
        if grade is not None and grade not in GRADES:
            # Answering a misspelled rating with a card is the one failure that
            # loses data: the board would draw the next card and the learner
            # would believe the grade landed.
            self._json(400, {"error": "bad_grade", "expected": list(GRADES)})
            return

        # Not _json(): this response IS the committed fixture, indentation and
        # trailing newline included, and test_kanji_mock.c parses the file to
        # assert on the bytes the board receives.
        body = encode(payload(grade))
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        # The device polls one host every five minutes and grades on a keypress;
        # letting it keep the socket saves a connect each time.
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        """Log to stderr, and without the timestamp — matching kanji_server.py,
        so a transcript from one is comparable with a transcript from the other."""
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def main():
    """Serve the payload, print it (`--dump`), or rewrite the host tests'
    fixture from it (`--write-fixture`).

    `--write-fixture` is the intended way to change the demo card: edit the
    payload here, rewrite the fixture, and test_kanji_mock.c will then tell you
    which field of kanji_mock.c no longer matches.
    """
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8123)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--dump", action="store_true", help="print the payload and exit")
    ap.add_argument("--write-fixture", action="store_true",
                    help="rewrite the host tests' fixture from this payload")
    args = ap.parse_args()

    if args.dump:
        sys.stdout.write(encode(payload()).decode("utf-8"))
        return
    if args.write_fixture:
        os.makedirs(os.path.dirname(FIXTURE), exist_ok=True)
        with open(FIXTURE, "wb") as f:
            f.write(encode(payload()))
        print(f"wrote {FIXTURE}")
        return

    srv = HTTPServer((args.host, args.port), Handler)
    print(f"serving the study card on http://{args.host}:{args.port}/kanji.json"
          f"  (grade with ?grade={'|'.join(GRADES)})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
