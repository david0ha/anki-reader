# The vault snapshot contract

The device polls one URL and draws whatever it returns. This is that format.

The whole contract has exactly three implementations, and they are checked against each other:

| | |
|---|---|
| `tools/mock_vault_server.py` | the reference producer, and a runnable server |
| `components/vault_core/vault_parse.c` | the consumer |
| `components/vault_core/vault_mock.c` | the built-in demo snapshot |

`test_vault_mock.c` parses the server's committed output
(`components/vault_core/test/host/fixtures/vault.json`) and asserts it fingerprints identically to
the C demo snapshot. The wire format and the screen an unconfigured board shows therefore cannot
drift apart without a test failing and naming the field that moved.

## The request

```
GET <vault_url>          every CONFIG_OBSIDIAN_POLL_SECONDS (default 300)
```

Any `http://` or `https://` URL. On a home LAN this is plain HTTP, and that is the expected case:
this is one machine talking to another inside the user's own network, and requiring a certificate
for it would mean requiring a certificate authority. The URL is set in the captive portal or with
`POST /api/vault`.

**Leaving it empty is a complete configuration.** The board then renders `vault_mock.c` with a
`DEMO` badge in the header — a finished product with no PC running at all.

## The payload

```json
{
  "schema": 1,
  "vault": "second-brain",
  "generated_at": "21:04",

  "stats": {
    "notes": 1428, "links": 3910, "orphans": 37, "tags": 212,
    "added_today": 6, "added_7d": 41,
    "daily": [3, 9, 12, 4, 0, 7, 6]
  },

  "tags": [ { "name": "프로젝트", "count": 186 } ],

  "agents": [
    { "name": "indexer", "state": "running", "last_run": "20:55",
      "processed": 1428, "queued": 3, "progress": 78,
      "note": "새 노트 6건 임베딩 중" }
  ],

  "graph": {
    "nodes": [ { "id": 0, "title": "MOC/연구", "deg": 24 } ],
    "edges": [ [0, 1], [0, 2] ]
  },

  "recent": [ { "time": "21:02", "title": "주간 회고", "links": 12 } ],
  "inbox":  [ { "title": "todo: 스펙 정리", "age_days": 3 } ],
  "inbox_total": 11
}
```

### Fields

| Field | Type | Notes |
|---|---|---|
| `vault` | string | shown in the header, ellipsized to fit |
| `generated_at` | string | free text; shown as "마지막 동기화". `"21:04"` reads best |
| `stats.daily` | int[] | **right-aligned**: the last entry is today. Fewer than 7 is fine |
| `tags[].name` | string | up to `VAULT_TAGS_MAX` (6) shown |
| `agents[].state` | string | `running` \| `idle` \| `error` \| `done`. Case-insensitive; `failed` is an alias for `error` |
| `agents[].progress` | int | 0–100, or **omit / send −1 for "no bar"** — which is different from 0% |
| `graph.nodes[].id` | int | the producer's own ids. May be sparse, unordered, huge |
| `graph.nodes[].deg` | int | link degree. Drives node size, and the parser sorts by it |
| `graph.edges` | [int,int][] | pairs of node **ids**, not array indices |
| `inbox_total` | int | the real backlog. The list shows what fits; the header shows this |

### Capacities

Display capacities, not protocol limits — send more and the extra is dropped, not rejected.

| | |
|---|---|
| `tags` | 6 |
| `agents` | 6 |
| `graph.nodes` | 14 |
| `graph.edges` | 32 |
| `recent` | 8 |
| `inbox` | 8 |
| any title | 64 bytes of UTF-8 (~21 Hangul syllables) |

Titles are truncated **on a character boundary**. Cutting a 3-byte Hangul syllable in half does not
render as "this was long"; it renders as a tofu box, and can walk LVGL's decoder past the NUL.

## What the parser does with bad input

The producer is somebody's script on their laptop. The error policy is deliberately lopsided:

**An individual bad field is not an error.** Wrong type, missing, negative, out of range — it
becomes the default and the rest of the snapshot is used. Rejecting a whole payload because one
producer wrote a string for `orphans` would blank the board over nothing.

**A payload with no vault content at all is rejected**, and rejection means *the previous snapshot
stays on the glass*, badged 오래됨. That covers: not JSON, a truncated response (the laptop closed
its lid), an error envelope, a captive-portal login page, and `{}`. A dashboard showing
stale-but-labelled data beats a blank one, and blanking is the one failure a user actually notices.

Also handled, each with a test: edges naming a node that was truncated away (dropped), an edge to
itself (dropped), the same pair twice (deduplicated), nodes arriving unsorted (sorted, with the edge
list translated to follow), and `daily` arriving with more or fewer than seven entries.

## Running the reference server

```bash
python3 tools/mock_vault_server.py                 # serve on :8123
python3 tools/mock_vault_server.py --live          # numbers drift on every request
python3 tools/mock_vault_server.py --dump          # print the payload
python3 tools/mock_vault_server.py --write-fixture # refresh the test fixture
```

Then point the board at it:

```bash
curl -X POST http://obsidianboard.local/api/vault \
     -d '{"url":"http://mymac.local:8123/vault.json"}'
```

or point the simulator at it, which renders the identical pixels the panel would:

```bash
VAULT_URL=http://localhost:8123/vault.json ./sim/sim.sh
```

`--live` exists to exercise the one behaviour a static payload cannot: watching the board refresh
when the numbers move, and *stay silent* when they do not.

## Writing a real producer

Anything that can serve JSON over HTTP. The shape above is the whole interface — there is no
authentication, no handshake and no versioning beyond `schema`, which the parser currently ignores
(it is there so a future format change has somewhere to declare itself).

A plugin inside Obsidian, a cron job over the vault directory, and a shell script that greps
`*.md` are all equally valid; the device cannot tell the difference and does not want to.
