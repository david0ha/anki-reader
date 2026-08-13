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
I provisioning: setup portal ready — join Wi-Fi 'Obsidian Board-XXXX' and open http://192.168.4.1
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
I net_time: time synced
I device_api: control server up on port 80
I device_api: mDNS advertising http://obsidianboard.local
```

`sntp sync timeout` does not change the artwork layout; snapshot staleness is measured monotonically
and is unaffected. From here the board is reachable:

```bash
curl -s http://obsidianboard.local/api/info
```

If mDNS does not resolve — some routers and most corporate networks block it — use the IP from the
`got IP` line. The companion app has a host override in Settings for exactly this.

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
| `partialRefreshMs` | records self-test behavior; normal artwork changes use a full refresh |
| `battery.millivolts` vs a multimeter on the cell | corrects `BATT_DIVIDER` in `components/board_io/board_io.c` |

`BATT_DIVIDER` is 3.0 **from the documentation, never measured**. It is the kind of constant that
fails quietly — a wrong ratio gives a percentage that looks entirely plausible and is wrong every
time you glance at the panel. Scale it by the ratio between the two readings, then record here that
it has been checked, and the question is closed.

Also watch one unchanged poll and one changed `daily_tarot` payload before deciding the refresh
policy. An unchanged tarot fingerprint must leave the panel untouched; a changed card or reading
gets one full refresh.

## 5. Point it at a real vault

```bash
python3 tools/vault_server.py ~/Documents/MyVault      # on the machine holding the vault
curl -X POST http://obsidianboard.local/api/vault -d '{"url":"http://mymac.local:8123/vault.json"}'
```

There is deliberately no `DEMO` badge, header or footer on the artwork. Confirm the source through
`GET /api/state`: `source.lastResult` becomes `ok`, while its three failure codes each send you
somewhere different — [vault-contract.md](vault-contract.md) has them. On glass, the full-height
card and the right-hand reading change only when the visible `daily_tarot` fingerprint changes.

## Buttons

| | |
|---|---|
| KEY0 | reserved (artwork unchanged) |
| KEY1 | poll now |
| KEY2 | tap → reserved · **hold 5 s → reboot into Wi-Fi setup** |
| BOOT | reserved (artwork unchanged) |

The KEY2 hold is the escape hatch for a board stuck on a network that no longer exists. It keeps the
saved config so the portal pre-fills, and only the Wi-Fi needs re-entering.
