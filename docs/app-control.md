# The device HTTP API

A JSON control server on port 80, up once Wi-Fi is connected, advertised over mDNS as
**`obsidianboard.local`**.

Local-network only: no auth, no TLS, no cloud. That is a scope decision, not an oversight — the
device holds no credentials worth stealing, and the only actions are "show a different page" and
"fetch from a different URL on this LAN".

> The hostname is **not** `tickerboard`. That name belongs to the fortune board this project forked
> from, whose shipped app resolves it. Two devices answering one discovery probe on the same LAN is
> a fault nobody can diagnose from the phone side.

## Endpoints

| Method | Path | Body | Effect |
|---|---|---|---|
| GET | `/api/info` | — | discovery probe |
| GET | `/api/state` | — | the full snapshot |
| POST | `/api/refresh` | — | poll the vault source now |
| POST | `/api/page` | `{"page":0..3}` | switch page (full refresh) |
| POST | `/api/vault` | `{"url":"http://..."}` | change the snapshot URL (persisted, live) |
| POST | `/api/display/test` | — | run the e-Paper self-test sweep |

Writes reply `{"ok":true}` or `{"ok":false,"error":"<code>"}` with a 400. Error codes:
`bad_json`, `too_large`, `read_error`, `page_range`, `vault_url_invalid`, `busy`.

Every write posts a command onto the app's queue and returns immediately; the UI task applies it
through the same code path as a button press. Nothing here touches LVGL or the panel directly —
exactly one task is allowed to start a refresh, because a full refresh of this panel takes seconds
and cannot be interleaved with another.

## `GET /api/info`

```json
{"deviceId":"1A2B","model":"Obsidian Board","fw":"0.1.0","ip":"192.168.0.42"}
```

Four fields, fixed shape: a discovery probe fetches this from every candidate host on the LAN and
reads `ip` to pick the best one. Renaming any of them is a client release, not a firmware change.

## `GET /api/state`

```json
{
  "deviceId": "1A2B", "model": "Obsidian Board", "fw": "0.1.0", "ip": "192.168.0.42",
  "page": 2, "pageTitle": "에이전트",

  "vault": {
    "valid": true, "demo": false,
    "name": "second-brain", "generatedAt": "21:04",
    "notes": 1428, "links": 3910, "orphans": 37, "tags": 212,
    "addedToday": 6, "added7d": 41,
    "agents": 5, "agentsRunning": 2,
    "recent": 8, "inbox": 11
  },

  "source": {
    "url": "http://mac.local:8123/vault.json",
    "lastResult": "ok",
    "pollSeconds": 300,
    "ageSeconds": 42,
    "stale": false
  },

  "battery": { "present": true, "percent": 84, "millivolts": 4012 },

  "panel": { "partialChain": 3, "fullRefreshMs": 4120, "partialRefreshMs": 780 }
}
```

This is a **summary**, not the vault snapshot. A client does not need the graph edges or eight note
titles — it needs to know the board is alive, what it is showing, and whether the last poll worked.
The full snapshot is available from the same URL the board polls, which the client can reach too.

`source.lastResult` is one of `ok`, `no_url`, `transport`, `http_status`, `bad_payload` — the same
strings the serial log uses. `transport` means DNS/connect/TLS/timeout; `http_status` means the
server answered but not with a 2xx; `bad_payload` means it answered 2xx with something that is not a
vault snapshot. Those three point at three different mistakes, which is why they are not one code.

`source.ageSeconds` is `-1` when no fetch has ever succeeded — which is different from "zero seconds
ago", and a client that treats it as a number will otherwise draw a board that just synced.

**`panel` is not decoration.** The refresh policy for this 648 × 480 UC8179 is meant to be set from
measurement rather than inherited from a panel a tenth the size, and these are the measurements.
Serving them means reading them off a phone instead of holding a serial cable to a board on a shelf.

## Examples

```bash
curl http://obsidianboard.local/api/state | jq

curl -X POST http://obsidianboard.local/api/page -d '{"page":1}'
curl -X POST http://obsidianboard.local/api/refresh
curl -X POST http://obsidianboard.local/api/vault \
     -d '{"url":"http://mymac.local:8123/vault.json"}'

# back to the built-in demo screen
curl -X POST http://obsidianboard.local/api/vault -d '{"url":""}'

# how long a refresh actually takes on this board
curl -s http://obsidianboard.local/api/state | jq .panel
```

## Provisioning API

Separate, and only up in AP mode — see [`components/provisioning/README.md`](../components/provisioning/README.md).
The captive portal collects the Wi-Fi credentials and the vault URL, saves them to NVS, and reboots.

## The companion app

`app/` still contains the fortune board's React Native app, unported. It is deliberately deferred:
the HTTP contract above is its entire surface, and porting it before that contract had been
exercised on real hardware would have meant doing it twice. See
`docs/specs/2026-08-10-obsidian-board-design.md` §Scope.
