#!/usr/bin/env python3
"""
The vault snapshot contract, as a runnable server.

This is the reference implementation of what the device fetches — the thing a
real Obsidian plugin or a cron script has to imitate. It exists for three jobs:

  1. Point a board (or the simulator) at a URL and watch real polling work,
     without writing anything that touches a vault.
  2. Produce the committed fixture the host tests parse
     (components/vault_core/test/host/fixtures/vault.json).
  3. Pin the contract. The payload here and vault_mock.c's built-in demo
     snapshot are asserted to be identical by test_vault_mock.c, so the wire
     format and the screen the board shows when nothing is configured cannot
     drift apart.

Usage
-----
    python3 tools/mock_vault_server.py                 # serve on :8123
    python3 tools/mock_vault_server.py --port 9000
    python3 tools/mock_vault_server.py --live          # numbers drift each poll
    python3 tools/mock_vault_server.py --dump          # print the payload
    python3 tools/mock_vault_server.py --write-fixture # refresh the test fixture

Then set the board's URL to http://<this machine>.local:8123/vault.json, either
in the captive portal or with

    curl -X POST http://obsidianboard.local/api/vault \\
         -d '{"url":"http://mymac.local:8123/vault.json"}'
"""

import argparse
import json
import os
import random
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from zoneinfo import ZoneInfo

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE = os.path.join(ROOT, "components", "vault_core", "test", "host",
                       "fixtures", "vault.json")

# The node list, in the order the device will draw it: degree descending. The
# parser re-sorts anyway (it will not trust a producer to have done it), but
# emitting them sorted keeps this file readable as the picture it becomes.
NODES = [
    ("MOC/연구",      24),
    ("데일리/2026",   19),
    ("프로젝트/보드", 17),
    ("아이디어",      14),
    ("논문",          12),
    ("ESP32",         11),
    ("회의록",         9),
    ("e-Paper",        8),
    ("Obsidian",       7),
    ("에이전트",       6),
    ("독서",           5),
    ("루틴",           4),
    ("레시피",         3),
    ("여행",           2),
]

EDGES = [
    (0, 1), (0, 2), (0, 3), (0, 4), (0, 5), (0, 6), (0, 8),
    (1, 2), (1, 6), (1, 11), (1, 12),
    (2, 5), (2, 7), (2, 9), (2, 3),
    (3, 4), (3, 9), (3, 10),
    (4, 8), (4, 10),
    (5, 7), (5, 9),
    (6, 11),
    (7, 8),
    (9, 10),
    (11, 13), (12, 13),
]


def snapshot(generated_at="21:04", date="2026-08-13"):
    """The canonical payload. Must stay identical to vault_mock.c."""
    return {
        "schema": 3,
        "vault": "second-brain",
        "generated_at": generated_at,
        "stats": {
            "notes": 1428,
            "links": 3910,
            "orphans": 37,
            "tags": 212,
            "added_today": 6,
            "added_7d": 41,
            # A zero day is in here deliberately: a bar chart that divides by
            # the value rather than the peak renders it as a blank column, or
            # divides by zero.
            "daily": [3, 9, 12, 4, 0, 7, 6],
        },
        "tags": [
            {"name": "프로젝트", "count": 186},
            {"name": "데일리",   "count": 141},
            {"name": "영역",     "count": 88},
            {"name": "자료",     "count": 63},
            {"name": "아카이브", "count": 41},
            {"name": "회의",     "count": 29},
        ],
        "agents": [
            {"name": "indexer", "state": "running", "last_run": "20:55",
             "processed": 1428, "queued": 3, "progress": 78,
             "note": "새 노트 6건 임베딩 중"},
            {"name": "linker", "state": "running", "last_run": "20:52",
             "processed": 910, "queued": 0, "progress": 41,
             "note": "백링크 후보 생성 중"},
            {"name": "summarizer", "state": "idle", "last_run": "18:30",
             "processed": 412, "queued": 0, "progress": -1, "note": ""},
            {"name": "archiver", "state": "error", "last_run": "17:02",
             "processed": 0, "queued": 12, "progress": -1,
             "note": "볼트 잠금이 해제되지 않음"},
            {"name": "tagger", "state": "done", "last_run": "16:10",
             "processed": 1428, "queued": 0, "progress": 100, "note": ""},
        ],
        "graph": {
            "nodes": [{"id": i, "title": t, "deg": d}
                      for i, (t, d) in enumerate(NODES)],
            "edges": [list(e) for e in EDGES],
        },
        "recent": [
            {"time": "21:02", "title": "주간 회고 2026-W32",         "links": 12},
            {"time": "20:41", "title": "UC8179 드라이버 정리",        "links": 4},
            {"time": "19:58", "title": "옵시디언 보드 설계",          "links": 18},
            {"time": "18:12", "title": "e-Paper 부분 갱신 실험",      "links": 6},
            {"time": "16:44", "title": "에이전트 오케스트레이션 노트", "links": 9},
            {"time": "15:20", "title": "읽기: 세컨드 브레인",          "links": 3},
            {"time": "13:05", "title": "ESP32-S3 PSRAM 메모",          "links": 7},
            {"time": "11:31", "title": "데일리/2026-08-10",            "links": 21},
        ],
        "inbox": [
            {"title": "todo: 스펙 정리하기",           "age_days": 3},
            {"title": "회의록 미정리 (8/7 스탠드업)",  "age_days": 2},
            {"title": "읽기: e-Paper LUT 논문",        "age_days": 5},
            {"title": "링크 끊김 확인 — 논문 폴더",    "age_days": 1},
            {"title": "태그 정리: 영역 vs 자료",       "age_days": 9},
            {"title": "웹클리핑 정리",                 "age_days": 12},
            {"title": "todo: 폰트 서브셋 재생성",      "age_days": 1},
            {"title": "아이디어 덤프 분류",            "age_days": 4},
        ],
        # More than the eight sent: the board shows what fits and reports the
        # real backlog in the header. Sending a window of a longer queue is the
        # normal case for a real producer, so the contract carries it from the
        # start rather than growing it later.
        "inbox_total": 11,
        "daily_tarot": {
            "date": date,
            "timezone": "Asia/Seoul",
            "card_id": "major-02",
            "orientation": "upright",
            "copy_version": 1,
            "headline": ["고요히 살피면", "속뜻이 보인다"],
            "flow": ["두 기둥 사이 장막이", "감춰진 단서를 품고 있다"],
            "caution": ["모호한 느낌을", "사실로 단정하지 않는다"],
            "action": ["답하기 전에", "침묵 속에서 한 번 읽는다"],
        },
        "artwork": {
            "headline": ["기억한 것은", "남아 있다."],
            "definition": {
                "headword": "우연한 연결",
                "meta": "명사",
                "lines": ["서로 멀리 있던 생각이 만나", "새로운 방향을 만드는 순간."],
            },
            "note": {
                "title": "우연한 연결",
                "path": "00 Daily/2026-08-13.md",
                "backlink_total": 6,
                "backlinks": ["아이디어", "MOC/연구", "프로젝트/보드"],
            },
            "graph": {
                "nodes": [
                    {"id": 0, "title": "우연한\n연결", "slot": 0},
                    {"id": 1, "title": "아이디어", "slot": 1},
                    {"id": 2, "title": "MOC/연구", "slot": 2},
                    {"id": 3, "title": "프로젝트/보드", "slot": 3},
                    {"id": 4, "title": "논문", "slot": 4},
                    {"id": 5, "title": "ESP32", "slot": 5},
                ],
                "edges": [
                    [0, 1], [0, 2], [0, 3], [0, 4],
                    [0, 5], [1, 2], [2, 4], [3, 5],
                ],
            },
        },
    }


def live_snapshot(state, clock, date="2026-08-13"):
    """The canonical payload with the numbers nudged, for --live.

    Exists to exercise the one behaviour a static payload cannot: the device
    only refreshes the panel when the content actually changed. Watching this
    drift is how you confirm the board is polling AND that an unchanged poll
    stays silent.
    """
    s = snapshot(generated_at=clock, date=date)
    state["notes"] += random.randint(0, 2)
    state["links"] += random.randint(0, 6)
    s["stats"]["notes"] = state["notes"]
    s["stats"]["links"] = state["links"]
    s["stats"]["added_today"] += state["notes"] - 1428
    s["stats"]["daily"][-1] = s["stats"]["added_today"]
    for a in s["agents"]:
        if a["state"] == "running" and a["progress"] >= 0:
            a["progress"] = (a["progress"] + 7) % 101
    return s


class Handler(BaseHTTPRequestHandler):
    live = False
    state = {"notes": 1428, "links": 3910}

    def do_GET(self):
        if self.path.split("?")[0] not in ("/vault.json", "/"):
            self.send_error(404)
            return
        import datetime
        now = datetime.datetime.now(ZoneInfo("Asia/Seoul"))
        clock = now.strftime("%H:%M")
        date = now.date().isoformat()
        payload = (live_snapshot(self.state, clock, date) if self.live
                   else snapshot(generated_at=clock, date=date))
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        # The device polls one host repeatedly; letting it keep the socket saves
        # a connect per poll and, on an https:// URL, a whole TLS handshake.
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8123)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--live", action="store_true",
                    help="nudge the numbers on every request")
    ap.add_argument("--dump", action="store_true", help="print the payload and exit")
    ap.add_argument("--write-fixture", action="store_true",
                    help="rewrite the host tests' fixture from this payload")
    args = ap.parse_args()

    if args.dump:
        print(json.dumps(snapshot(), ensure_ascii=False, indent=2))
        return
    if args.write_fixture:
        os.makedirs(os.path.dirname(FIXTURE), exist_ok=True)
        with open(FIXTURE, "w", encoding="utf-8") as f:
            json.dump(snapshot(), f, ensure_ascii=False, indent=2)
            f.write("\n")
        print(f"wrote {FIXTURE}")
        return

    Handler.live = args.live
    srv = HTTPServer((args.host, args.port), Handler)
    print(f"serving the vault snapshot on http://{args.host}:{args.port}/vault.json"
          + ("  (live)" if args.live else ""))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
