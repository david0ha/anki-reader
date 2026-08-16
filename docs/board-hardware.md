# Hardware

A **Seeed XIAO ePaper Display Board EE04** carrying a **XIAO ESP32-S3 Plus**, with a 5.83"
monochrome e-Paper panel on the 24-pin FPC connector. Unlike the hand-wired board this project
forked from, this is an off-the-shelf combination — the wiring is the carrier's, not yours, and
[pinout.md](pinout.md) records it rather than proposing it.

| Item | Specification |
|------|------|
| Carrier | [XIAO ePaper Display Board EE04](https://wiki.seeedstudio.com/epaper_ee04/) |
| SoC | ESP32-S3 (Xtensa LX7 dual-core, up to 240 MHz) |
| Flash / PSRAM | 16 MB Flash / 8 MB Octal PSRAM |
| Display | [5.83" monochrome e-Paper, **648 × 480**](https://www.seeedstudio.com/5-83-Monochrome-ePaper-Display-with-648x480-Pixels-p-5785.html), **UC8179**, 4-wire SPI + BUSY |
| RTC | **none** — the clock is SNTP only |
| Wireless | Wi-Fi 802.11 b/g/n, BLE 5.0 (unused) |
| USB | Type-C — power, programming, native USB Serial/JTAG |
| Buttons | KEY0/1/2 (GPIO2/3/5) on the carrier, BOOT (GPIO0) on the XIAO |
| Power | 5 V USB-C, optional Li-ion on JST 2.0 + slide switch, voltage via ADC divider |

The EE04 also has a 50-pin FPC connector for Spectra6 six-colour panels, unpopulated here.

## Notes that matter in practice

**No backlight.** e-Paper is reflective; it is unreadable in the dark and excellent in daylight.
This is the opposite trade from an LCD and it drives the whole visual design — heavy strokes, high
contrast, no greys, nothing that depends on a hairline surviving a threshold.

**No RTC, and the two pins that used to carry I2C are taken.** The EE05 routed GPIO5/GPIO6 to an
I2C side header; on the EE04 they are KEY2 and the battery divider's load-switch enable. So the
PCF85063A driver and the whole I2C bus are gone. Nothing on the glass needs them: this UI prints no
clock, and every interval it shows arrives from the proxy already worded. SNTP still runs, for
logs — a board whose timestamps all read 1970 is unpleasant to debug — but a `sntp sync timeout`
changes nothing a learner can see.

**PSRAM is required here.** Two RGB565 draw buffers at 648 × 480 are 622 KB each; that is 1.2 MB,
and there is no internal-RAM fallback that could hold them. The XIAO ESP32-S3 **Plus** has 8 MB, so
this is comfortable — but a plain XIAO ESP32-S3 without PSRAM will not run this firmware. The panel
framebuffer (38,880 bytes) is separate, always internal, and DMA-capable.

**The battery divider needs its switch driven.** GPIO6 gates it; until that is HIGH the ADC reads
noise. `board_io_init()` drives it and leaves it on. A reading below 2.5 V is reported as "no cell
fitted" rather than as a flat battery — a Li-ion whose protection has cut off never presents that
voltage, so it means USB-only operation, and an empty battery icon there is a false alarm.

**`BATT_DIVIDER` is 3.0 on the strength of the documentation, not a measurement.** It has never been
checked against this board, and it is the kind of constant that fails quietly: a wrong ratio gives a
percentage that looks entirely plausible and is wrong every single time you glance at the panel —
the same failure shape as a wrong calendar anchor in the project this forked from.

Check it once, the first time a cell is fitted: read `battery.millivolts` from `GET /api/state`,
compare against the cell measured with a multimeter, and scale `BATT_DIVIDER` in
`components/board_io/board_io.c` by the ratio. If the two agree, write that down here and the
question is closed.

**Battery reading is defensive.** Every `board_io` getter returns 0 rather than blocking if the ADC
is unavailable, so a depopulated part never wedges the render loop.

## Power

Seeed quotes about three months on a charge for a board that sleeps between refreshes. This firmware
does not sleep — it holds Wi-Fi up so `ankireader.local` stays reachable and the poll interval
stays honest. On USB that is the right trade; on battery it is not, and a future revision that wants
battery life should look at light sleep between polls before anything else.

The panel itself is not the problem: `epd_sleep()` gets the controller to about 1 µA, and the
refresh policy already keeps refreshes rare (see [epaper-5in83.md](epaper-5in83.md)).

## Firmware footprint

**0x53acb0 bytes (5.23 MiB)** of the 8 MB app partition, leaving **35%**.

The fonts are most of that. Four faces, measured with `xtensa-esp32s3-elf-size` on the objects the
IDF build produces:

| Face | `.rodata` |
|---|---|
| `ui_font_kr_16` | 387 KiB |
| `ui_font_kr_20` | 531 KiB |
| `ui_font_kr_28` | 860 KiB |
| `ui_font_jp_56` | 2,194 KiB |

Just under 3.9 MiB of the 5.23, and it is not padding: three of those faces carry 9,242 glyphs each
— 완성형 Hangul, ASCII, every kana, both JIS X 0208 kanji levels and the 158 component forms the
설명 sheet's shape stories cite — because the strings on this board arrive from kanjis.ai at runtime
and there is nothing safe to subset from. The hero face carries 6,713 and is Japanese-only, for the
same reason in reverse: Hangul at 56 px would be another ~790 KB for glyphs a Japanese headword
cannot contain.

`ui_font_jp_56` is also why the build needs `CONFIG_LV_FONT_FMT_TXT_LARGE=y`. LVGL packs a glyph's
bitmap offset into 20 bits otherwise, which caps one face at 1 MB of bitmap; this one has 2.02 MB.
Setting it widens every glyph descriptor in the build from 8 bytes to 16, which costs the three body
faces about 213 KB between them. It fails loudly — `lv_font_conv` writes an `#error` into the
generated file — but only if your `sdkconfig` was regenerated after the option landed in
`sdkconfig.defaults`; see [esp-idf-development.md](esp-idf-development.md#sdkconfigdefaults-is-read-once-and-only-once).

## Offline catalog and rating flash

The 16 MiB flash ends with two raw 4 KiB-aligned partitions after the 8 MiB
factory application: the read-only `catalog` partition occupies `0x810000`–
`0xF7FFFF` (7.44 MiB), and writable `study_state` occupies `0xF80000`–
`0xFFFFFF` (512 KiB). The catalog is block-compressed and decoded through
PSRAM workspaces with a normal-heap fallback; only one compressed block, one
96 KiB raw block, and the compact per-card rating summaries are resident.

Configure the catalog source when the sibling backend is not at the default
path, then generate or flash it with ESP-IDF 5.4.3:

```sh
idf.py -DKANJI_CATALOG_DB=/absolute/path/to/kanjis-backend.sqlite3 catalog_image
idf.py catalog-flash
```

`KANJI_CATALOG_USER_ID` optionally selects a source user and
`KANJI_CATALOG_SEED` defaults to `0`. A normal `idf.py flash` depends on the
same deterministic generator and registers the resulting image only for the
`catalog` partition. `idf.py app-flash` updates firmware without updating the
catalog. There is deliberately no generated or registered `study_state`
image, so ordinary firmware/catalog flashing physically leaves those bytes
untouched. App-only flashing also preserves the catalog and therefore its
usable progress. After a catalog update, records replay only when the stored
catalog ID matches the active catalog; an ID change starts fresh progress
without erasing the old state bytes. `idf.py erase-flash` still erases the
entire device.

Rating persistence uses two 256 KiB append-journal banks. A normal grade
programs one verified 20-byte record without an erase. A torn tail is ignored
on replay; because partially programmed NOR bytes cannot be reused, the next
grade compacts the latest summary for each reviewed card into the inactive
bank. Ordinary compaction happens when a bank fills, commits the replacement
bank before the old bank is erased, and therefore trades an occasional full
bank erase/write for power-loss recovery. Repeated power cuts during record
writes can trigger earlier compactions and additional erase wear, but never
make a torn record authoritative.
