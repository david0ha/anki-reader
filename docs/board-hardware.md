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
PCF85063A driver and the whole I2C bus are gone, and the header clock comes from SNTP alone. It is
blank (`--:--`) until the first sync, which is a few seconds after Wi-Fi comes up.

**PSRAM is required here.** Two RGB565 draw buffers at 648 × 480 are 622 KB each; that is 1.2 MB,
and there is no internal-RAM fallback that could hold them. The XIAO ESP32-S3 **Plus** has 8 MB, so
this is comfortable — but a plain XIAO ESP32-S3 without PSRAM will not run this firmware. The panel
framebuffer (38,880 bytes) is separate, always internal, and DMA-capable.

**The battery divider needs its switch driven.** GPIO6 gates it; until that is HIGH the ADC reads
noise. `board_io_init()` drives it and leaves it on. A reading below 2.5 V is reported as "no cell
fitted" rather than as a flat battery — a Li-ion whose protection has cut off never presents that
voltage, so it means USB-only operation, and an empty battery icon there is a false alarm.

**Battery reading is defensive.** Every `board_io` getter returns 0 rather than blocking if the ADC
is unavailable, so a depopulated part never wedges the render loop.

## Power

Seeed quotes about three months on a charge for a board that sleeps between refreshes. This firmware
does not sleep — it holds Wi-Fi up so `obsidianboard.local` stays reachable and the poll interval
stays honest. On USB that is the right trade; on battery it is not, and a future revision that wants
battery life should look at light sleep between polls before anything else.

The panel itself is not the problem: `epd_sleep()` gets the controller to about 1 µA, and the
refresh policy already keeps refreshes rare (see [epaper-5in83.md](epaper-5in83.md)).

## Firmware footprint

At the time of writing: **1.7 MB** of an 8 MB app partition (79% free). About 450 KB of that is the
two full 완성형 font faces — which is the point of having 8 MB, and is discussed in
[pages.md](pages.md).
