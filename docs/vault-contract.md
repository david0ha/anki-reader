# The vault snapshot contract

The device polls one URL and draws whatever it returns. This is that format.

The whole contract has exactly three implementations, and they are checked against each other:

| | |
|---|---|
| `tools/mock_vault_server.py` | the reference producer, and a runnable server |
| `components/vault_core/vault_parse.c` | the consumer |
| `components/vault_core/vault_mock.c` | the built-in demo snapshot |

(`tools/vault_server.py` is a fourth, but it produces whatever a real vault happens to contain, so
it cannot be pinned against a fixture the way these three are. It has its own tests instead.)

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

## Serving a real vault

`tools/vault_server.py` walks an actual vault on disk and serves this contract from it. It is
read-only — it opens `.md` files and writes nothing.

```bash
python3 tools/vault_server.py ~/Documents/MyVault        # serve on :8123
python3 tools/vault_server.py ~/Documents/MyVault --dump # print the payload
python3 tools/test_vault_server.py                       # its tests
```

The definitions it uses are Obsidian's where Obsidian has one, and stated in the script's docstring
where it does not. The two worth knowing here:

- **`links` counts distinct directed pairs between notes that exist.** Two `[[B]]`s in A are one
  link, and a link to a note nobody has created yet is zero — the graph on the panel has nowhere to
  draw it. `![[embeds]]` count. On the panel, a reciprocal pair is one line.
- **A colliding note name is prefixed with its folder** (`docs/README`), because a real vault has
  three `README`s and three identical rows on a 648-pixel panel say nothing.

Rescanning is incremental — an unchanged file is not reopened — because the board polls forever.

### Agents

The board's agent page reports on work that something *else* is doing, so this server does not
invent it. Point `--agents FILE` at a JSON file that your own tooling writes:

```json
[ { "name": "indexer", "state": "running", "last_run": "20:55",
    "processed": 1428, "queued": 3, "progress": 78, "note": "embedding 6 new notes" } ]
```

`state` is `running | idle | error | done`; `progress` is `0..100`, or `-1` for a task with no
measurable progress. No file means no agents, which the board draws as an empty board rather than
as something pretending.

`tools/agent_status.py` writes that file, so reporting is one line from any script, cron job or
hook — which is what stops the agents page being permanently empty:

```bash
A=tools/agent_status.py
$A set indexer running --note "reindexing" --progress 40
./do-the-work && $A set indexer done --progress 100 || $A set indexer error --note "failed"
$A list        # what the board would show; entries past the sixth are marked
```

It writes atomically (temp file + rename), so the server can never read a half-written file no
matter how often it is called, and it updates an agent **in place** — the board draws the first six
in file order, and an agent that jumped to the end on every status change would fall off the panel
the moment a seventh existed.

### Keeping it running

The board polls forever, so the server has to be up for longer than a terminal window. On macOS,
`~/Library/LaunchAgents/local.obsidianboard.vault.plist`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>local.obsidianboard.vault</string>
  <key>ProgramArguments</key><array>
    <string>/usr/bin/python3</string>
    <string>/Users/YOU/Documents/obsidian_board_esp32/tools/vault_server.py</string>
    <string>/Users/YOU/Documents/MyVault</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardErrorPath</key><string>/tmp/obsidianboard-vault.log</string>
</dict></plist>
```

```bash
launchctl load ~/Library/LaunchAgents/local.obsidianboard.vault.plist
```

Two things to know before you do: the board can only reach it while the machine is **awake** — a
sleeping laptop is a `transport` error on the panel, correctly — and the mDNS name the board is
pointed at (`mymac.local`) has to keep resolving, so a DHCP reservation or the machine's IP is
steadier than its hostname on some routers.

### Capture

A wall display showing an inbox you cannot add to is half a loop. `--allow-capture` adds one
endpoint that closes it:

```bash
python3 tools/vault_server.py ~/Documents/MyVault --allow-capture
curl -X POST http://localhost:8123/capture -d 'ring the dentist'
```

That writes `Inbox/ring the dentist.md` — the memo's first line is the filename, so it is also the
title the board draws; the date goes in the frontmatter, because the panel already shows the age.
The board picks it up on its next poll, or immediately with `POST /api/refresh`.

**It is off unless you ask for it, and it is not part of the device contract** — nothing in the
firmware knows this endpoint exists. Enabling it means an unauthenticated service on your LAN can
create files in your notes. When on, it can do exactly one thing: create a new `.md` inside the
capture folder. The filename is built from a sanitised slug so a request cannot name a path
(`../../etc/passwd` becomes `etc passwd.md` inside the folder), an existing file is never
overwritten, and the body is capped at 8 KB.

| result | meaning |
|---|---|
| `201 {"ok":true,"path":"Inbox/…"}` | written |
| `403 capture_disabled` | the endpoint exists, `--allow-capture` was not given |
| `400 empty` / `400 too_large` / `400 bad_json` | the request |
| `500 write_failed` | the disk said no |

### Glyphs

On startup and on every request the server checks the payload against the character set the shipped
fonts were built from (it imports `gen_fonts.symbol_set()` — not a second list to keep in step) and
warns about anything the board cannot draw. The faces carry the full 완성형 set plus ASCII, so a
Korean or English title is safe; an emoji or a hanja in a note title is not, and would otherwise
reach you as a tofu box on the glass. `--no-glyph-check` turns it off.

## Writing your own producer

Anything that can serve JSON over HTTP. The shape above is the whole interface — there is no
authentication, no handshake and no versioning beyond `schema`, which the parser currently ignores
(it is there so a future format change has somewhere to declare itself).

A plugin inside Obsidian, a cron job over the vault directory, and a shell script that greps
`*.md` are all equally valid; the device cannot tell the difference and does not want to.
