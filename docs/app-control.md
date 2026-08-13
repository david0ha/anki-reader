# The device HTTP API

A JSON control server on port 80, up once Wi-Fi is connected, advertised over mDNS as
**`obsidianboard.local`**.

Local-network only: no auth, no TLS, no cloud. That is a scope decision, not an oversight — the
device holds no credentials worth stealing, and the only actions are "refresh the artwork" and
"fetch from a different URL on this LAN".

> The hostname is **not** `tickerboard`. That name belongs to the fortune board this project forked
> from, whose shipped app resolves it. Two devices answering one discovery probe on the same LAN is
> a fault nobody can diagnose from the phone side.

## Endpoints

| Method | Path | Body | Effect |
|---|---|---|---|
| GET | `/api/info` | — | discovery probe |
| GET | `/api/state` | — | board-status summary |
| POST | `/api/refresh` | — | poll the vault source now |
| POST | `/api/page` | `{"page":0}` | compatibility no-op for the single artwork composition |
| POST | `/api/vault` | `{"url":"http://..."}` | change the snapshot URL (persisted, live) |
| POST | `/api/display/test` | — | run the e-Paper self-test sweep |

Writes reply `{"ok":true}` or `{"ok":false,"error":"<code>"}` with a 400. Error codes:
`bad_json`, `too_large`, `read_error`, `page_range`, `vault_url_invalid`, `busy`.

Every write posts a command onto the app's queue and returns immediately; the UI task applies it
through the same code path as a button press. Nothing here touches LVGL or the panel directly —
exactly one task is allowed to start a refresh, because a full refresh of this panel takes seconds
and cannot be interleaved with another.

There is exactly one panel page: `Artwork` at index `0`. The endpoint is retained for compatible
clients; it does not select another view or force a refresh. The artwork itself is a native
648 × 480 composition with no header/footer chrome: a 272 × 464 tarot card on the left and its
daily headline, flow, caution and action inside a cut-corner reading frame on the right.

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
  "page": 0, "pageTitle": "Artwork",

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

`page` is always `0` and `pageTitle` is always `Artwork`. A client may display that state but must
not offer page navigation.

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

curl -X POST http://obsidianboard.local/api/page -d '{"page":0}'
curl -X POST http://obsidianboard.local/api/refresh
curl -X POST http://obsidianboard.local/api/vault \
     -d '{"url":"http://mymac.local:8123/vault.json"}'

# back to the persisted built-in design preview (also disables polling after reboot)
curl -X POST http://obsidianboard.local/api/vault -d '{"url":""}'

# how long a refresh actually takes on this board
curl -s http://obsidianboard.local/api/state | jq .panel
```

## Provisioning API

Separate, and only up in AP mode — see [`components/provisioning/README.md`](../components/provisioning/README.md).
The captive portal collects the Wi-Fi credentials and the vault URL, saves them to NVS, and reboots.

## The companion app

`app/` implements this contract — see [`../app/README.md`](../app/README.md). `app/src/lib/esp32.ts`
is the TypeScript mirror of this document and the only file in the app that knows a field name, so
a change here is a change there.

Two things in the app are worth knowing about when changing this contract:

- **`source.ageSeconds` of `-1`** is parsed as "never synced" and rendered as `never`. A client that
  defaults a missing `ageSeconds` to `0` draws a board that just synced when it never has, so the
  app defaults it to `-1` and a test pins that.
- **An unrecognised `lastResult`** is mapped to `unknown` rather than passed through, because the
  UI switches on it. Adding a code here is therefore safe; it degrades to a neutral chip until the
  app learns it.

`app/scripts/mock-esp32.js` implements this whole contract in Node, including really fetching a
snapshot URL and summarising it — so the app can be developed against it, and the contract has a
second implementation to disagree with.
