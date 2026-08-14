# The device HTTP API

A JSON control server on port 80, up once Wi-Fi is connected, advertised over mDNS as
**`obsidianboard.local`**.

Local-network only: no auth, no TLS, no cloud. That is a scope decision, not an oversight — the
device holds no credentials worth stealing. The kanjis.ai session lives in `tools/kanji_server.py`
on your own machine and never reaches the board (see [`kanji-contract.md`](kanji-contract.md)), so
the only things this API can do are "show a different screen" and "fetch from a different URL on
this LAN".

> The hostname is deliberately **not** renamed to follow the board's new job: paired clients and
> already-flashed boards answer to `obsidianboard`. It is **not** `tickerboard` either — that name
> belongs to the fortune board this project forked from, whose shipped app resolves it, and two
> devices answering one discovery probe on the same LAN is a fault nobody can diagnose from the
> phone side.

## Endpoints

| Method | Path | Body | Effect |
|---|---|---|---|
| GET | `/api/info` | — | discovery probe |
| GET | `/api/state` | — | the whole board summary |
| POST | `/api/refresh` | — | poll the study source now |
| POST | `/api/screen` | `{"screen":0..4}` | put the board on one of the five screens |
| POST | `/api/study` | `{"url":"http://..."}` | change the study URL (persisted, live) |
| POST | `/api/display/test` | — | run the e-Paper self-test sweep |

Writes reply `{"ok":true}`, or `{"ok":false,"error":"<code>"}` with a 400. Error codes:

| Code | Meaning |
|---|---|
| `bad_json` | the body did not parse, or the field is missing or the wrong type |
| `too_large` | the body is over 319 bytes |
| `read_error` | the socket died mid-body |
| `screen_range` | `screen` is not 0..4 — or the command queue was full |
| `study_url_invalid` | not an `http://`/`https://` URL with a host, over 128 characters — or the command queue was full |
| `busy` | the command queue was full |

`screen_range` and `study_url_invalid` each cover a full queue as well as a bad value because the
client cannot do anything different about the two, and a bad value is by far the likelier.

Every write posts a command onto the app's queue and returns immediately; the UI task applies it
through the same code path as a button press. Nothing here touches LVGL or the panel directly —
exactly one task is allowed to start a refresh, because a full refresh of this panel takes seconds
and cannot be interleaved with another.

**There is no route that grades a card.** The four ratings are KEY1 only. Grading is the one input
that has to come from the learner in front of the panel, and the proxy answers a grade with the
*next* card — so a phone racing the buttons for it would advance the session twice and leave the
two disagreeing about which card is up.

## `GET /api/info`

```json
{"deviceId":"1A2B","model":"Kanjis Board","fw":"0.1.0","ip":"192.168.0.42"}
```

Four fields, fixed shape: a discovery probe fetches this from every candidate host on the LAN and
reads `ip` to pick the best one. Renaming any of them is a client release, not a firmware change.

## `GET /api/state`

```json
{
  "deviceId": "1A2B", "model": "Kanjis Board", "fw": "0.1.0", "ip": "192.168.0.42",

  "screen": 1, "screenTitle": "정답", "revealed": true, "grade": 3,

  "card": {
    "valid": true, "demo": false,
    "front": "会う", "reading": "あう", "meaning": "만나다",
    "fsrsState": "review", "due": "9일 뒤",
    "reps": 5, "lapses": 1, "stabilityDays": 9, "difficultyPct": 47
  },

  "session": {
    "deck": "JLPT N5 Vocabulary",
    "streak": 12, "reviewedToday": 34,
    "leftNew": 7, "leftReview": 18,
    "track": 35, "trackTotal": 60,
    "complete": false
  },

  "source": {
    "url": "http://mac.local:8123/kanji.json",
    "lastResult": "ok",
    "pollSeconds": 300,
    "ageSeconds": 42,
    "stale": false
  },

  "battery": { "present": true, "percent": 84, "millivolts": 4012 },

  "panel": { "partialChain": 3, "fullRefreshMs": 4120, "partialRefreshMs": 780 }
}
```

That document is 713 bytes. **Every number in it is an integer** — there is not a fraction anywhere
in this API, by design, so a client can parse the lot as ints.

This is a **summary**, not the study snapshot. A client does not get the three senses, the examples,
the shape parts or the comments: it needs to know the board is alive, which card it is on, whether
the answer is showing, and whether the last poll worked. Anything richer is one request away from
the same proxy the board polls, which the client can reach too.

### identity

| Field | Type | Notes |
|---|---|---|
| `deviceId` | string | The last two bytes of the STA MAC, hex. Stable per board. |
| `model` | string | `Kanjis Board`. |
| `fw` | string | Firmware version. |
| `ip` | string | The station IP, or `""` before DHCP has finished. |

### what is on the glass

| Field | Type | Notes |
|---|---|---|
| `screen` | int | `0` 문제, `1` 정답, `2` 설명, `3` 댓글, `4` FSRS. |
| `screenTitle` | string | The same word the footer prints — the Korean above, or `FSRS`. |
| `revealed` | bool | The answer side has been shown for this card. |
| `grade` | int | Where the dock cursor is parked: `1` 다시, `2` 어려움, `3` 보통, `4` 쉬움. Reading it does **not** grade anything. |

`screen` is derived from the interaction state, not stored beside it, so it cannot describe two
things at once: an open sheet always wins, and with no sheet open the screen is `정답` if the answer
is revealed and `문제` if it is not.

### `card`

| Field | Type | Notes |
|---|---|---|
| `valid` | bool | `false` = the session served no card. The board shows the completion screen; it does not blank. |
| `demo` | bool | This is `kanji_mock.c`'s built-in card, because no study URL is configured. |
| `front` | string | The headword. |
| `reading` | string | The かな reading. |
| `meaning` | string | The **first** sense only. The panel shows up to three. |
| `fsrsState` | string | The wire word: `new` / `learning` / `review` / `relearning`. |
| `due` | string | When this card is next due, already worded by the proxy: `"9일 뒤"`, `"곧"`, `""` if never scheduled. |
| `reps`, `lapses` | int | The scheduler's counters. |
| `stabilityDays` | int | Whole days. **`-1` = not scheduled yet**, which the panel prints as `—`. |
| `difficultyPct` | int | 0..100. **`-1` = not scheduled yet.** |

`stabilityDays: 0` and `stabilityDays: -1` are different things. A new card has no stability; a card
with a same-day interval has one that rounds to zero. A client that renders `-1` as a number claims
the board knows something it does not.

The board has no RTC, so `due` is a *string the proxy rendered against the server's clock* rather
than a timestamp. Do not try to parse it into a date — there is no instant behind it to recover.

### `session`

| Field | Type | Notes |
|---|---|---|
| `deck` | string | The deck the session is drawing from. |
| `streak` | int | 연속 일수. |
| `reviewedToday` | int | 오늘 복습. |
| `leftNew`, `leftReview` | int | What is still queued for today. |
| `track`, `trackTotal` | int | 1-based position in today's queue. |
| `complete` | bool | The session is done. The board shows 오늘 학습 완료 and stops asking. |

### `source`

| Field | Type | Notes |
|---|---|---|
| `url` | string | What the board polls. `""` = the built-in demo card. |
| `lastResult` | string | `ok`, `no_url`, `transport`, `http_status`, `bad_payload`. |
| `pollSeconds` | int | The poll interval — 300 unless `CONFIG_OBSIDIAN_POLL_SECONDS` says otherwise. |
| `ageSeconds` | int | Since the last **successful** fetch. **`-1` = never.** |
| `stale` | bool | Nothing has arrived for two whole poll intervals. |

The three failures are separate codes because they send you to three different places: `transport`
is DNS/connect/TLS/timeout (is the machine awake?), `http_status` means the server answered but not
with a 2xx (is the path right?), and `bad_payload` means it answered 2xx with something that is not
a study payload (is that a captive portal?).

`ageSeconds` of `-1` is not "zero seconds ago", and a client that defaults a missing value to `0`
draws a board that just synced when it never has.

### `battery`

| Field | Type | Notes |
|---|---|---|
| `present` | bool | A battery is on the JST connector. |
| `percent` | int | 0..100, from the divider on the ADC. |
| `millivolts` | int | The measured pack voltage. |

### `panel`

**Not decoration.** The refresh policy for this 648 × 480 UC8179 is meant to be set from measurement
rather than inherited from a panel a tenth the size, and these are the measurements. Serving them
means reading them off a phone instead of holding a serial cable to a board on a shelf.

| Field | Type | Notes |
|---|---|---|
| `partialChain` | int | How many partial refreshes have run since the last full one. At `EPD_PARTIAL_CHAIN_MAX` (6) the driver promotes the next partial to a full refresh and resets this to 0. |
| `fullRefreshMs` | int | How long the last full refresh actually took. |
| `partialRefreshMs` | int | The same for the last windowed partial. |

## `POST /api/screen`

```json
{"screen": 2}
```

Moves the same nav state a KEY press moves, so a phone cannot park the board on a screen the buttons
cannot get it out of. A screen change resets the sheet to its first page and costs one full refresh.

A screen with nothing to show is **declined**, and the board stays exactly where it was. It goes
through `kanji_nav_set_screen()` — the same gate BOOT goes through — so the rules are identical from
either control:

| Asked for | Declined when |
|---|---|
| `1` 정답 | the session has no card: there is no answer to reveal, and KEY0 refuses for the same reason |
| `2` 설명 | there is no card, or this one has no shape story, memory hook or components |
| `3` 댓글 | there is no card |
| `0` 문제, `4` FSRS | never — FSRS explains the scheduler, which is what an empty session is most worth reading about |

The POST still answers `{"ok":true}`: it only reports that the command was queued, because the board
applies it on the task that owns the panel. Read `screen` back from `/api/state` rather than
assuming the POST took — the board reports what it did, not what it was asked.

## `POST /api/study`

```json
{"url": "http://mymac.local:8123/kanji.json"}
```

Validated, saved to NVS and applied live — no reboot. `""` switches the board back to the built-in
demo card immediately rather than at the next poll, because with no URL there is no next poll.

Any fetch already in flight against the old URL is discarded when it lands, so a slow response to
the previous proxy cannot overwrite the first card from the new one.

## `POST /api/refresh`

No body. Polls now instead of waiting out `pollSeconds`. The panel is only refreshed if what comes
back differs from what is already on the glass, so this is safe to call repeatedly — which is the
whole reason it can be a button on a phone.

## `POST /api/display/test`

No body. Runs the panel self-test sweep — tens of seconds of full refreshes — on the UI task. The
reply comes back as soon as it is queued, not when it finishes, and the board is unresponsive to
buttons for the duration.

## Examples

```bash
curl http://obsidianboard.local/api/state | jq

curl -X POST http://obsidianboard.local/api/screen -d '{"screen":4}'   # the FSRS sheet
curl -X POST http://obsidianboard.local/api/refresh
curl -X POST http://obsidianboard.local/api/study \
     -d '{"url":"http://mymac.local:8123/kanji.json"}'

# back to the built-in demo card (also stops polling after a reboot)
curl -X POST http://obsidianboard.local/api/study -d '{"url":""}'

# how long a refresh actually takes on this board
curl -s http://obsidianboard.local/api/state | jq .panel

# where the learner is in today's queue
curl -s http://obsidianboard.local/api/state | jq '.session | "\(.track)/\(.trackTotal)"'
```

## Provisioning API

Separate, and only up in AP mode — see
[`components/provisioning/README.md`](../components/provisioning/README.md). The captive portal
collects the Wi-Fi credentials and the study URL, saves them to NVS, and reboots.

## Clients

`components/vault_core/test/host/test_api_json.c` is the executable copy of this document: it pins
every field name above, the integer-only rule, the escaping, and that the worst-case document still
fits the server's buffer. Change a name here and change it there.

```bash
rm -rf /tmp/vt && cmake -S components/vault_core/test/host -B /tmp/vt \
  && cmake --build /tmp/vt --target test_api_json && /tmp/vt/test_api_json
```

`app/src/lib/esp32.ts` is the one file in the React Native client that knows a field name, and it
mirrors this document field for field — including the `-1` sentinels on `stabilityDays`,
`difficultyPct` and `ageSeconds`, which mean "not decided yet" and are not zero. `app/scripts/
mock-esp32.js` serves this same document without a board, so the client can be developed against
the contract rather than against a device on a shelf.
