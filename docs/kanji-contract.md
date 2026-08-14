# The JSON the device polls

The board holds no credentials and speaks to exactly one URL on the local network. That URL is
served by `tools/kanji_server.py`, which holds the kanjis.ai session and does everything the board
cannot: TLS to `api.kanjis.ai`, the Supabase bearer token, the second JSON parse of the card's
`back`/`hint` columns, and every date-to-Korean conversion — the board has no RTC, so it cannot
compute "9일 뒤" from an ISO timestamp even in principle.

```
ESP32-S3                    tools/kanji_server.py             api.kanjis.ai
   │  GET http://pc:8123/kanji.json      │                          │
   ├────────────────────────────────────►│  Authorization: Bearer   │
   │                                     ├─────────────────────────►│
   │  one flat card, all text            │                          │
   │◄────────────────────────────────────┤◄─────────────────────────┤
   │                                     │                          │
   │  GET .../kanji.json?grade=good      │  POST /study/sessions/   │
   ├────────────────────────────────────►│       {id}/answer        │
   │  the NEXT card, same shape          ├─────────────────────────►│
   │◄────────────────────────────────────┤                          │
```

This is the same split `tools/vault_server.py` already used for Obsidian, and the same split
`masterham-esp32` uses for its own backend: **the device carries a device identity, never a user
credential, and the server maps one to the other.** Nothing on the board can leak an account.

## The two requests

| Request | Meaning |
|---|---|
| `GET {KANJI_URL}` | The card the session is serving right now. Idempotent — the five-minute poll and KEY2 both use it. |
| `GET {KANJI_URL}?grade=again\|hard\|good\|easy&card={id}` | Grade the card the board is showing, then return the **next** one in the same shape. |

`card` names the card being rated, and it is what makes a mis-grade detectable. The proxy advances
whenever *anything* grades — including the web app in another tab — so a rating that arrives without
an id is applied to whatever the proxy happens to be serving by then, which after the session has
moved on is not the card the learner read. With the id, that request is a **409** instead, and the
board leaves the answer on the glass. A repeat of the id just graded is recognised as a retry and
answered with the payload the first one produced, rather than counted twice.

The board omits `card` when it has no id to send (the built-in demo card), and the proxy then falls
back to grading what it is serving. `components/vault_core/kanji_service.c` also drops an id holding
anything outside `[A-Za-z0-9._~-]` rather than percent-encoding it — a real id is a UUID, and an id
carrying a `&` would silently change which parameters the proxy reads.

Grading is a GET on purpose. `components/vault_core/include/http_port.h` exposes exactly one call —
`char *http_get(const char *url, int *out_status)` — and it has three implementations (ESP-IDF,
libcurl for the simulator, and the host test standing in for itself). Adding a method, a body and
headers to all three to move one enum across a LAN buys nothing; keeping the port at one function
means `test_kanji_service.c` can still *be* the network. The endpoint is a single-client service on
a private network, and the proxy rejects a `grade` for a card it is not currently serving, so the
usual reasons not to mutate on GET do not apply here.

`{KANJI_URL}` is whatever the captive portal stored in NVS. Anything that is not a 200 with parseable
JSON leaves the previous card on the glass, badged 오래됨.

## The payload

```json
{
  "v": 1,
  "session": {
    "deck": "JLPT N5 Vocabulary",
    "level": "N5",
    "streak": 12,
    "reviewed_today": 34,
    "left_new": 7,
    "left_review": 18,
    "retry": 2,
    "track": 35,
    "track_total": 60,
    "complete": false
  },
  "card": {
    "id": "f00c539e-23f9-4294-bee1-c642189b105f",
    "front": "会う",
    "reading": "あう",
    "on_reading": "",
    "kun_reading": "あう",
    "level": "N5",
    "gloss": "만날 회",
    "senses": ["만나다", "대면하다", "우연히 만나다"],
    "examples": [
      { "text": "出会う", "reading": "であう", "gloss": "우연히 만나다" },
      { "text": "出会い", "reading": "であい", "gloss": "만남" }
    ],
    "description": "会는 사람들이 모여 서로 말하고 교류하는 모습을 바탕으로 한 글자입니다.",
    "hook_title": "기억 힌트",
    "hook_body": "위의 구성은 모임을, 아래의 모양은 말함을 나타냅니다.",
    "composition": "人 + 云 = 会",
    "parts": [
      { "glyph": "会", "meaning": "모이다, 만나다", "reading": "あう (훈독)" }
    ],
    "comments": [
      { "author": "카나 선생", "body": "「会う」는 사람을 만날 때 씁니다.", "likes": 12 }
    ],
    "comment_total": 12,
    "fsrs": {
      "state": "review",
      "state_label": "복습",
      "due": "9일 뒤",
      "reps": 5,
      "lapses": 1,
      "stability_days": 9,
      "difficulty_pct": 47
    },
    "preview": { "again": "10분 뒤", "hard": "4일 뒤", "good": "9일 뒤", "easy": "21일 뒤" }
  }
}
```

### session

| Field | Type | Notes |
|---|---|---|
| `deck` | string | The "channel" on the caption line. |
| `level` | string | The active JLPT filter, or `"전체"`. |
| `streak` | int | 연속 chip. Clamped to 0..9999. |
| `reviewed_today` | int | 오늘 chip. Clamped to 0..9999. |
| `left_new`, `left_review`, `retry` | int | The queue counters. Clamped to 0..9999. |
| `track`, `track_total` | int | 1-based position in today's queue. `track` is clamped to `track_total`. |
| `complete` | bool | The session is done; the board shows 오늘 학습 완료 and stops asking. |

### card

Omit `card` entirely (or send `null`) to say "no card". The board keeps the session block and shows
the completion screen; it does **not** blank.

| Field | Type | Cap | Notes |
|---|---|---|---|
| `id` | string | 39 B | Routing only. Deliberately excluded from the refresh fingerprint. |
| `front` | string | 39 B | The headword — the hero. 10 CJK characters is the catalog's longest. |
| `reading` | string | 143 B | Collapsed display reading; on-yomi leads and readings are `・`-joined. |
| `on_reading` | string | 143 B | Optional structured on-yomi display string. Empty when absent. |
| `kun_reading` | string | 143 B | Optional structured kun-yomi display string. Empty when absent. |
| `level` | string | 23 B | `N5`…`N1`. |
| `gloss` | string | 143 B | Optional short source gloss, separate from `senses`. |
| `senses` | string[] | 5 × 143 B | Korean glosses, most important first. |
| `examples` | object[] | 3 | `{text, reading, gloss}`, each capped like `front`/`reading`/a sense. |
| `description` | string | 831 B | Full `back.shape_explanation`. |
| `hook_title` | string | 23 B | Optional source `hint.principle`; empty when absent and never inferred. |
| `hook_body` | string | 831 B | Full `hint.reason`. |
| `composition` | string | 95 B | Optional safe source-component equation. |
| `parts` | object[] | 6 | Every measured `hint.shapes[]` row as `{glyph, meaning, reading}`; a missing reading is `""`. |
| `comments` | object[] | 3 | `{author, body, likes}`. Replies flattened away. |
| `comment_total` | int | | The server's real count; ≥ `comments.length`. |
| `fsrs` | object | | See below. |
| `preview` | object | | The four ratings' next-due spans, already worded. |

Every string is truncated by the parser on a UTF-8 character boundary only at these model limits,
so the measured catalog's 819-byte explanation, 615-byte mnemonic, five senses, and six components
arrive intact. Every array takes its first N only beyond those full-fidelity limits. **A field the
board cannot use is never a reason to reject the payload** — only malformed JSON is.

### Source kind

`kanji_t.source` is local model metadata, not a wire key. A successful network or catalog envelope
parse begins as `KANJI_SOURCE_REMOTE`; the catalog reader may reclassify a validated snapshot as
`KANJI_SOURCE_CATALOG`, and the built-in fallback is `KANJI_SOURCE_DEMO`. `KANJI_SOURCE_NONE` means
no usable snapshot. The source kind participates in the snapshot fingerprint so a source transition
always refreshes the display.

### card.fsrs

| Field | Type | Notes |
|---|---|---|
| `state` | string | The wire word: `new` / `learning` / `review` / `relearning`. |
| `state_label` | string | The Korean the FSRS sheet prints: 새 카드 / 학습 중 / 복습 / 다시 학습. |
| `due` | string | When this card is next due, as a span: `"9일 뒤"`, `"곧"`, `""` if never scheduled. |
| `reps`, `lapses` | int | Clamped to 0..99999. |
| `stability_days` | int | Rounded. **`-1` means "not scheduled yet"**, which the sheet prints as `—`. |
| `difficulty_pct` | int | 0..100. `-1` means not scheduled yet. |

`stability_days: 0` and `stability_days: -1` are different things and the panel prints them
differently. A new card has no stability; a card with a same-day interval has one that rounds to
zero. Sending `0` for an unscheduled card makes the board claim to know something it does not.

### card.preview

The four keys `again` / `hard` / `good` / `easy`, each a **pre-rendered Korean span** computed by
the proxy against the *server's* clock from the backend's `rating_preview` ISO timestamps, using the
same rounding as `kanjis-front`'s `relativeDue()`:

| Span from now | Rendered |
|---|---|
| < 45 s | `곧` |
| < 1 h | `N분 뒤` |
| < 1 d | `N시간 뒤` |
| < 30 d | `N일 뒤` |
| < 365 d | `N개월 뒤` |
| otherwise | `N년 뒤` |

## How it fails

| What happens | What the board does |
|---|---|
| Wi-Fi down, DNS fails, connection refused | Keep the last card. Header badge → 오프라인. |
| Non-200 | Keep the last card. Badge → 오래됨. |
| Body is not JSON, or the root is not an object | Keep the last card. Badge → 오래됨. |
| `card` missing / `null` | Show the completion screen, keep the session counters. |
| A field is the wrong type | That field takes its default; the rest of the payload is kept. |
| No `KANJI_URL` is configured at all | Show `kanji_mock.c`'s built-in card, badged DEMO. |

The rule that matters: **`kanji_parse()` writes `*out` only on success.** Blanking the panel is the
one failure a learner actually notices, and a stale card badged 오래됨 beats an empty one.

## Glyph coverage

Every string in this payload is drawn from a font that carries 완성형 Hangul, ASCII, kana and
JIS X 0208 kanji — and nothing else. `tools/kanji_server.py` imports `symbol_set()` straight from
`tools/gen_fonts.py` and checks every string it is about to send, substituting anything outside the
face rather than shipping a tofu box to the glass. The catalog does contain a handful of characters
no shipped face covers — simplified-Chinese component forms and astral-plane radical glyphs inside
`hint.shapes[].kanji` — and those are exactly what the check catches.

## The reference producer

`tools/mock_kanji_server.py` serves this contract from a fixed payload and is the reference
implementation. `components/vault_core/kanji_mock.c` must produce a byte-identical snapshot;
`test_kanji_mock.c` asserts it by parsing the server's committed fixture and comparing fingerprints.
Change one and the test names the field that diverged, then run:

```bash
python3 tools/mock_kanji_server.py --write-fixture
```
