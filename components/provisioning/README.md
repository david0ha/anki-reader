# provisioning

Wi-Fi provisioning with a captive-portal fallback. On boot the device joins the saved
network; if that fails (or nothing is saved yet) it raises a SoftAP and serves a setup page
where the user enters Wi-Fi credentials and the study URL. The submission is stored in
NVS and the device reboots and connects.

## Boot flow

```
load config (NVS)
      │
有 saved SSID ──yes──▶ try STA connect (timeout) ──ok──▶ return connected → app runs
      │                          │
      no                        fail
      ▼                          ▼
  start SoftAP "AnkiReader-XXXX" (open) + captive portal
      │
  user submits SSID / password / study_url  →  save to NVS  →  reboot
```

The auto-fallback is simply the loop closing on itself: a bad password means the next boot's
STA attempt fails and the portal comes back up.

## Layers

**Pure logic (host-unit-tested, no ESP-IDF dependency):**

| File | Responsibility |
|------|----------------|
| `prov_config.{h,c}` | config model (`ssid` / `password` / `study_url`) + credential and URL validation |
| `form_parse.{h,c}`  | `x-www-form-urlencoded` decode + field extraction |
| `prov_json.{h,c}`   | JSON building + string escaping for the `/api/*` responses |

**Embedded glue (verified by build + on-device):**

| File | Responsibility |
|------|----------------|
| `prov_store.{h,c}`  | NVS load/save/clear (namespace `prov`); `prov_store_save` returns commit status |
| `prov_wifi.{h,c}`   | STA connect (bounded initial retry → then **persistent** reconnect once online), SoftAP, **non-blocking background scan → cache** |
| `prov_portal.{h,c}` | HTTP server + DNS hijack captive portal; `/scan` returns the cache (never scans live), rejects over-length / NUL-injected fields |
| `provisioning.{h,c}`| orchestrator (`provisioning_run`) + public API; only reboots on a confirmed save |
| `net_time.{h,c}`    | one-shot SNTP sync after connect (there is no RTC on this board, so this is the only source the header clock has); deinits when done |
| `portal.html`       | self-contained setup page (embedded via `EMBED_TXTFILES`) |

## HTTP endpoints (SoftAP, 192.168.4.1)

| Method | Path              | Purpose |
|--------|-------------------|---------|
| GET    | `/`               | setup page (network list and saved study URL rendered server-side) |
| POST   | `/save`           | browser form: `ssid=…&password=…&study_url=…` → result page, then reboot |
| GET    | `/api/info`       | `{"deviceId","model","apSsid"}` |
| GET    | `/api/scan`       | `{"networks":[{"ssid","rssi","secure"}, …]}` (served from the cache) |
| POST   | `/api/provision`  | app form body → `202`, then an async connect test |
| GET    | `/api/status`     | `{"state":"idle\|connecting\|connected\|failed","ssid?","reason?"}` |
| *      | *(other)*         | 302 → `http://192.168.4.1/` (OS captive-portal detection) |

`POST /api/provision` reads `ssid` / `ssid_manual` / `password` / `study_url` and nothing else, so a
phone still running the stock-ticker app's build — which POSTs `tickers` / `finnhub_key` / `fmp_key`
/ `econ_url` — has those fields discarded and still completes onboarding. The body allowance stays
generous for the same reason. See [../../docs/app-control.md](../../docs/app-control.md).

A UDP DNS responder on port 53 answers every A query with `192.168.4.1` so phones pop the
captive sheet automatically.

## NVS keys (namespace `prov`)

`ssid` (str) · `pass` (str) · `vurl` (str, the study URL) · `force_ap` (u8, one-shot).

`prov_store_save` also erases `tickers` / `fh_key` / `fmp_key` / `econ_url` — keys the stock-ticker
firmware wrote, one of which held a live API secret. A device upgraded from that build drops them on
its first save rather than leaving them in flash.

## Host tests

The pure logic has a self-contained test harness (no external framework):

```sh
./test/run.sh
```

Compiles `prov_config.c` / `form_parse.c` / `json_build.c` with the tests under `test/`
using UndefinedBehaviorSanitizer and runs them. (AddressSanitizer is intentionally omitted —
its shadow-memory mmap is blocked in the CI sandbox.)
