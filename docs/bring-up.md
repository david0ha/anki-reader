# First power-on

What to do the first time this firmware meets this board, and how to read the boot log. Everything
here is written against hardware that has **not yet been powered on** — the firmware builds clean
and every layer that can be tested without a panel passes, but no line below has been observed on a
real board. Confirm each one as you go and correct this file where reality differs.

The point of the procedure is that the three things most likely to be wrong on a first boot —
the FPC seating, the BUSY polarity, and the panel power gate — each produce a *different* line in
the log, and none of them produces a Python traceback or a crash. A blank screen looks identical in
all three cases. The log is the only place they are distinguishable.

## 1. Flash

```bash
. ~/esp/v5.4.3/esp-idf/export.sh    # once per shell
idf.py build
./tools/flash.sh                    # finds the port, flashes, opens the monitor
```

If `flash.sh` says no port appeared, find out **which** of the two failures it is before touching
anything, because they have nothing in common:

```bash
ls /dev/cu.*                 # is there a serial port?
ioreg -p IOUSB               # is there a USB device at all?
```

A healthy board shows a `/dev/cu.usbmodem*` (native USB Serial/JTAG) and a device hanging off one of
the `AppleT8132USBXHCI` controllers.

| `ioreg` shows | meaning |
|---|---|
| controllers only, no child devices | the host is not seeing a device. **Charge-only USB cable** is by far the most common cause — the data pairs are absent, so the Mac cannot tell it from an empty port, and the board's LED still lights either way. Then: not plugged in, a dead hub port, or a dead board. |
| a device, but no `/dev/cu.*` for it | it enumerates but exposes no serial interface — usually stuck in download mode from a previous attempt, or a bridge chip whose driver is missing |

Swap the cable for one you have moved data over before doing anything else. If a device appears and
flashing still fails, force download mode: hold **BOOT**, tap **RESET**, release BOOT, retry.

If the *build* stops instead, on `Too large font or glyphs in UI_FONT_JP_56`, the checkout's
`sdkconfig` predates `CONFIG_LV_FONT_FMT_TXT_LARGE=y`. `sdkconfig` is generated once and never
re-derived from `sdkconfig.defaults`, so `rm sdkconfig && idf.py build` is the fix.

## 2. Read the boot log in this order

Each line below is the checkpoint for one subsystem. They appear in this sequence; the first one
missing is where to stop and look.

### `board_io` — battery

```
I board_io: battery ADC on GPIO1 (unit 0 ch 0): 4.05V
I board_io: battery ADC on GPIO1 (unit 0 ch 0): 0.02V — no cell fitted, USB power
```

On USB with no cell, the second form is correct and expected. `adc calibration unavailable` is a
warning, not a fault — it means an uncalibrated voltage, which is still enough to bring up a panel.

### `epd` — the panel, and the three traps

This is the part that matters. `busy_line_probe()` runs once after the init sequence and prints
exactly one of three lines:

| line | meaning | what to do |
|---|---|---|
| `BUSY driven HIGH — UC8179 idle, as expected` | the panel is there and idle | continue |
| `BUSY follows the weak pulls — nothing is driving it` | nothing is on the other end of the FPC | reseat the 24-pin FPC — check orientation (contacts down) and that the latch is closed |
| `BUSY driven LOW after init — the controller thinks it is still busy` | *something* is there, but it does not idle like a UC8179 | wrong panel on the connector, or a colour variant instead of the monochrome 5.83" |

The second and third are the ones worth knowing apart. Both give you a blank screen; only one of
them is fixed by pushing the cable in.

Then:

```
I epd: UC8179 648x480 up (stride 81, fb 38880 B)
I epd: full refresh 3xxx ms
```

**If `full refresh` never appears** and instead you get
`BUSY stuck low for 30000ms — panel wired/powered?`, the driver waited its full timeout and gave up
rather than hanging. Two candidates: the power gate (GPIO43) is not actually reaching the panel, or
BUSY polarity is inverted for this particular panel revision. `epd_init()` drives GPIO43 HIGH before
touching the controller, so if the rail is dead the fault is in the carrier or the FPC, not in this
code. Polarity is [documented and load-bearing](epaper-5in83.md#busy-is-active-low) — it is the
inverse of the SSD1680 family, and getting it backwards fails *silently* rather than with this
message, so this timeout actually argues **against** a polarity problem.

### `LvglPort` — memory

```
I LvglPort: Install LVGL tick timer
```

If instead you see `need 2 x 622080 B of PSRAM for the draw buffers` followed by
`this firmware needs a XIAO ESP32-S3 *Plus*`, the module in the socket is a plain XIAO ESP32-S3
without PSRAM. There is no configuration that makes this firmware fit; it needs the Plus.

### `provisioning` — Wi-Fi

First boot has nothing stored:

```
I provisioning: no stored network — starting setup portal
I provisioning: setup portal ready — join Wi-Fi 'Kanjis Board-XXXX' and open http://192.168.4.1
```

The panel should now show the setup overlay with that same SSID on it. **That is the first
end-to-end confirmation that the display works** — text you chose, rendered by LVGL, binarized, and
pushed through a full refresh. If the log says the portal is ready and the glass is still blank, the
fault is in the panel path, not in Wi-Fi.

Join that network from a phone or laptop, fill in the form, and expect:

```
I prov_wifi: got IP 192.168.x.x
I provisioning: restarting to apply confirmed configuration
```

It reboots. On the second boot: `stored network 'X' — attempting to connect`.

### `net_time`, `device_api` — online

```
I app: online — study URL '...'
I net_time: time synced
I device_api: control server up on port 80
I device_api: mDNS advertising http://obsidianboard.local
```

`sntp sync timeout` changes nothing on the glass. This board prints no clock and computes no
interval: every span it shows — `9일 뒤`, `10분 뒤` — is worded by the proxy against the *server's*
clock and arrives as a string. Card staleness is measured monotonically and is unaffected. From here
the board is reachable:

```bash
curl -s http://obsidianboard.local/api/info
```

If mDNS does not resolve — some routers and most corporate networks block it — use the IP from the
`got IP` line. The companion app has a host override in Settings for exactly this. The full route
list is in [app-control.md](app-control.md).

## 3. Run the self-test

```bash
curl -X POST http://obsidianboard.local/api/display/test
```

Six patterns, tens of seconds, each failing differently on purpose — what each one proves is in
[epaper-5in83.md](epaper-5in83.md#self-test). Watch the glass rather than the log for this one. The
one to pay attention to is the **top-left solid block**: it is the only pattern that tells you which
corner the origin is, because every other pattern is symmetric enough to look right upside down.

## 4. Record three numbers

These are the measurements the firmware was deliberately built not to guess at.

```bash
curl -s http://obsidianboard.local/api/state | jq '.panel, .battery'
```

| number | where it goes |
|---|---|
| `fullRefreshMs` | decides nothing on its own, but see the table in [epaper-5in83.md](epaper-5in83.md#when-the-numbers-arrive) |
| `partialRefreshMs` | this one is load-bearing now: it is what a KEY0 press on the answer screen costs. See below. |
| `battery.millivolts` vs a multimeter on the cell | corrects `BATT_DIVIDER` in `components/board_io/board_io.c` |

`BATT_DIVIDER` is 3.0 **from the documentation, never measured**. It is the kind of constant that
fails quietly — a wrong ratio gives a percentage that looks entirely plausible and is wrong every
time you glance at the panel. Scale it by the ratio between the two readings, then record here that
it has been checked, and the question is closed.

## 5. Walk the study loop on the glass

This is the acceptance test the simulator cannot run, because it is about time rather than pixels.

1. **The demo card.** With no study URL the board shows 会う badged `DEMO` within a second of boot.
   Press KEY0: it reveals. Press KEY0 again three times and watch the grade cursor walk
   보통 → 쉬움 → 다시 → 어려움. **Only the dock strip should flash** — if the whole panel refreshes,
   the dock rectangle and the drawn dock have drifted apart; `test_kanji_layout.c` covers the
   geometry, so look at what `present_dock()` was handed.
2. **Ghosting.** Keep pressing KEY0. The driver promotes the sixth partial in a row to a full
   refresh (`EPD_PARTIAL_CHAIN_MAX`). If residue is visible before that, lower it.
3. **A real session.** Start `tools/kanji_server.py` on your machine and give the board its URL —
   from the portal, or over the network per [app-control.md](app-control.md). The `DEMO` badge goes;
   `source.lastResult` in `GET /api/state` becomes `ok`.
4. **An unchanged poll.** Leave it five minutes. The panel must not move — the log says
   `study: unchanged, panel untouched` at debug level. This is the single most common outcome in the
   device's life and the one that must not cost a refresh.
5. **A grade.** Press KEY1 on the answer side. The panel stays on the answer you just rated until
   the next card actually arrives; that is deliberate, not a hang. The next card comes back as the
   *response* to the grade, so anything drawn sooner would be a guess.
6. **A dead proxy.** Stop `kanji_server.py` and press KEY2. The card stays; the header badge becomes
   오프라인. Nothing blanks. The three failure codes each send you somewhere different —
   [kanji-contract.md](kanji-contract.md#how-it-fails) has them.

## Buttons

| | 문제 | 정답 | inside a sheet |
|---|---|---|---|
| KEY0 | reveal the answer | walk the grade cursor | next page |
| KEY1 | open 설명 | commit the rating | close the sheet |
| KEY2 | refresh, from anywhere · **hold 5 s → reboot into Wi-Fi setup** | | |
| BOOT | open FSRS | open 설명 | next sheet |

The KEY2 hold is the escape hatch for a board stuck on a network that no longer exists. It keeps the
saved config so the portal pre-fills, and only the Wi-Fi needs re-entering. It is caught before the
nav state machine sees the press, because the state machine has no notion of how long a press was.
