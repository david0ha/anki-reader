# The vault snapshot contract

The board polls one URL and renders the normalized daily tarot it returns. The current wire contract
remains `schema: 3`; the legacy dashboard and artwork fields remain available for compatibility.

The canonical fixed payload has three checked implementations:

| | |
|---|---|
| `tools/mock_vault_server.py` | reference producer and runnable server |
| `components/vault_core/vault_parse.c` | bounded consumer |
| `components/vault_core/vault_mock.c` | built-in demo artwork |

`test_vault_mock.c` parses the committed reference fixture and requires it to match the built-in C
demo. `tools/vault_server.py` produces the same schema from a real vault and has its own tests.

## Request and empty configuration

```text
GET <vault_url>          every CONFIG_OBSIDIAN_POLL_SECONDS (default 300)
```

The URL may use `http://` or `https://`. Plain HTTP is expected on a trusted home LAN. It is set in
the captive portal or with `POST /api/vault`.

Leaving the URL empty renders `vault_mock.c` as an offline display/design preview. Because its card,
date and copy are fixed test data rather than a live daily selection, the composition says
`운세 디자인 미리보기` and `예시 카드`. Source status remains available through `GET /api/state`.

## Native tarot

`daily_tarot` is optional at the reusable parser boundary for compatibility: an older schema-3
payload still parses and leaves `daily_tarot.valid == false`. The active tarot firmware poller,
however, requires a valid object before replacing the last image. Missing or malformed tarot is
reported as `bad_payload`, while the last usable panel image remains untouched.

```json
"daily_tarot": {
  "date": "2026-08-13",
  "timezone": "Asia/Seoul",
  "card_id": "major-02",
  "orientation": "upright",
  "copy_version": 1,
  "headline": ["고요히 살피면", "속뜻이 보인다"],
  "flow": ["두 기둥 사이 장막이", "감춰진 단서를 품고 있다"],
  "caution": ["모호한 느낌을", "사실로 단정하지 않는다"],
  "action": ["답하기 전에", "침묵 속에서 한 번 읽는다"]
}
```

The stable card IDs are `major-00` through `major-21`, plus `cups-01` through
`cups-14`, `pentacles-01` through `pentacles-14`, `swords-01` through `swords-14`,
and `wands-01` through `wands-14`. Version 1 accepts only `orientation: "upright"`.
`copy_version` is a positive integer. Each copy field is an array of one or two non-empty strings;
a larger array or an embedded ASCII control character invalidates the tarot object. Each C line stores at most
127 UTF-8 bytes and truncates only at a codepoint boundary.

The reference producer loads `tools/tarot_readings_ko.json` and validates that it contains exactly
the 78 unique IDs. Headline rows are limited to 22 display cells and flow/caution/action rows to 32.
Whitespace costs one cell and every inked glyph costs two; the conservative rule covers wide Latin
`W` as well as Hangul. These limits are tested against the actual 28 px/16 px LVGL fonts and fixed
pixel boxes, so accepted copy is not ellipsized.
A source checkout without that file
uses a complete stable fallback, while an existing but invalid file fails producer startup loudly.

Selection is deterministic. The producer converts aware clocks to `Asia/Seoul`, then computes
SHA-256 over `YYYY-MM-DD`, the vault name and the stable device seed separated by newlines. The
digest selects from the sorted 78 IDs, so the same Seoul date/vault/seed always returns the same
card. `--tarot-seed` provisions the device seed; the normalized vault path is the default.

## Legacy native artwork

The panel is a native `648 × 480` monochrome canvas. One inset frame and one vertical hairline form
an editorial diptych:

- left: Quote and Definition;
- right: Note, Backlinks and a Related Notes graph;
- no logo, application header/footer, page dots, clock, refresh cadence, live-status badge, card,
  pill, image, animation or device-side wrapping.

The producer selects the thought and supplies already-broken lines, backlink order and deterministic
graph slots. The ESP32 validates, truncates, copies and draws fixed LVGL objects; it does not perform
NLP, graph physics or prose layout.

## Schema 3 payload

Legacy vault aggregates remain in the payload because the companion app reports them, but only the
bounded `artwork` object changes pixels on the panel.

```json
{
  "schema": 3,
  "vault": "second-brain",
  "generated_at": "21:04",

  "stats": {
    "notes": 1428, "links": 3910, "orphans": 37, "tags": 212,
    "added_today": 6, "added_7d": 41,
    "daily": [3, 9, 12, 4, 0, 7, 6]
  },
  "tags": [{"name": "프로젝트", "count": 186}],
  "agents": [{"name": "indexer", "state": "running", "progress": 78}],
  "graph": {
    "nodes": [{"id": 0, "title": "MOC/연구", "deg": 24}],
    "edges": []
  },
  "recent": [{"time": "21:02", "title": "주간 회고", "links": 12}],
  "inbox": [{"title": "todo: 스펙 정리", "age_days": 3}],
  "inbox_total": 11,

  "daily_tarot": {
    "date": "2026-08-13", "timezone": "Asia/Seoul",
    "card_id": "major-02", "orientation": "upright", "copy_version": 1,
    "headline": ["고요히 살피면", "속뜻이 보인다"],
    "flow": ["두 기둥 사이 장막이", "감춰진 단서를 품고 있다"],
    "caution": ["모호한 느낌을", "사실로 단정하지 않는다"],
    "action": ["답하기 전에", "침묵 속에서 한 번 읽는다"]
  },

  "artwork": {
    "headline": ["기억한 것은", "남아 있다."],
    "definition": {
      "headword": "우연한 연결",
      "meta": "명사",
      "lines": ["서로 멀리 있던 생각이 만나", "새로운 방향을 만드는 순간."]
    },
    "note": {
      "title": "우연한 연결",
      "path": "00 Daily/2026-08-13.md",
      "backlink_total": 6,
      "backlinks": ["아이디어", "MOC/연구", "프로젝트/보드"]
    },
    "graph": {
      "nodes": [
        {"id": 0, "title": "우연한\n연결", "slot": 0},
        {"id": 1, "title": "아이디어", "slot": 1}
      ],
      "edges": [[0, 1]]
    }
  }
}
```

### Artwork fields

| Field | Type | Visible meaning |
|---|---|---|
| `headline` | string[] | Quote; producer-broken, at most 2 rows |
| `definition.headword` | string | Definition headword |
| `definition.meta` | string | short part-of-speech/meta text |
| `definition.lines` | string[] | producer-broken, at most 2 rows |
| `note.title` | string | focus note title; required |
| `note.path` | string | focus note path |
| `note.backlink_total` | non-negative int | full incoming-link count, separate from rows sent |
| `note.backlinks` | string[] | strongest incoming neighbors, at most 3 rows |
| `graph.nodes[].id` | int | wire identifier used only to resolve edges |
| `graph.nodes[].title` | string | visible graph label |
| `graph.nodes[].slot` | int | requested deterministic slot, `0..5` |
| `graph.edges` | `[int,int][]` | pairs of wire node IDs |

Slot `0` is always the focus. The five satellite positions are top, upper-left, upper-right,
lower-left and lower-right. The parser preserves the first owner of each legal slot. If slot `0`
is absent it promotes an unassigned node (or the first node when all declared slots are unique),
then assigns duplicate or invalid slots to the lowest free position. It drops unknown-node and
self edges, canonicalizes both directions to the same pair, deduplicates and keeps the smallest
eight pairs in stable order. The renderer draws edges first, then opaque label boxes, then nodes
and text so an edge cannot cross a word.

### Capacities

Extra rows are dropped. Strings are truncated on a UTF-8 codepoint boundary so a Hangul syllable is
never split.

| Artwork value | Capacity |
|---|---:|
| headline rows | 2 |
| definition rows | 2 |
| backlink rows | 3 |
| graph nodes | 6 |
| graph edges | 8 |
| headline/definition line | 127 UTF-8 bytes |
| headword, note/backlink/node title | 63 UTF-8 bytes |
| definition meta | 31 UTF-8 bytes |
| note path | 127 UTF-8 bytes |

Legacy summary capacities are unchanged: 6 tags, 6 agents, 14 legacy graph nodes, 32 legacy graph
edges, 8 recent notes and 8 inbox rows.

## Validation and failure behavior

A declared schema must be the exact integer `3`. Schema 1/2, a non-integer version, malformed JSON
or a schema-3 payload without drawable artwork is rejected. Drawable artwork requires a non-empty
`note.title` plus at least one headline or definition headword/line.

Individual malformed optional fields degrade to their default, while invalid graph elements are
dropped. Parsing happens into a temporary value and is copied into live state only on success. A
failed fetch or rejected payload therefore preserves the last good panel image.

`vault_tarot_hash()` includes only pixels-driving daily-tarot fields (plus demo/preview mode) and
returns the empty FNV offset for an invalid snapshot. Pollers ignore unrelated counters, timestamps,
legacy artwork and non-rendered producer metadata such as `copy_version`.
`vault_hash()` also includes daily tarot for complete content addressing; `vault_artwork_hash()` is
retained for the legacy artwork renderer. An unchanged tarot fingerprint causes no e-paper refresh.

## Running the reference server

```bash
python3 tools/mock_vault_server.py                 # serve on :8123
python3 tools/mock_vault_server.py --live          # vary non-artwork summary values
python3 tools/mock_vault_server.py --dump           # print the payload
python3 tools/mock_vault_server.py --write-fixture  # refresh the host-test fixture
```

Point the board or native simulator at it:

```bash
curl -X POST http://obsidianboard.local/api/vault \
     -d '{"url":"http://mymac.local:8123/vault.json"}'

VAULT_URL=http://localhost:8123/vault.json ./sim/artwork_sim.sh
```

## Serving a real vault

`tools/vault_server.py` reads `.md` files without modifying them, selects a focus note
deterministically from the highest-degree recent graph node, and emits the same schema-3 artwork.

```bash
python3 tools/vault_server.py ~/Documents/MyVault
python3 tools/vault_server.py ~/Documents/MyVault --dump
python3 tools/vault_server.py ~/Documents/MyVault --tarot-seed DEVICE-STABLE-ID
python3 tools/test_vault_server.py
```

Up to three incoming neighbors become Backlinks; `backlink_total` retains the full count. The focus
and strongest related notes fill the six fixed graph slots. Colliding note names are prefixed with
their folder so labels remain meaningful.

### Optional agent summary

`--agents FILE` adds operational metadata for the companion app. It does not add a panel page or
change the artwork unless the producer's actual artwork selection changes.

```bash
python3 tools/agent_status.py --file ~/agents.json set indexer running --progress 40
python3 tools/vault_server.py ~/Documents/MyVault --agents ~/agents.json
```

### Optional capture

`--allow-capture` enables an unauthenticated LAN endpoint that creates a new Markdown note in the
configured capture folder. It is off by default and is not part of the device contract.

```bash
python3 tools/vault_server.py ~/Documents/MyVault --allow-capture
curl -X POST http://localhost:8123/capture -d 'ring the dentist'
```

The filename is sanitized, an existing file is never overwritten and the body is capped at 8 KB.
The new note can influence a later focus/backlink selection; saving it does not bypass the normal
schema-3 producer or fingerprint rules.

## Writing a producer

Anything that serves the shape above over HTTP can be a producer. `schema: 3` and drawable artwork
are mandatory. Line breaking and selection belong in the producer; the device is intentionally a
bounded renderer.
