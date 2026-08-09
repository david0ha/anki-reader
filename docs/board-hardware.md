# Hardware

An ESP32-S3 module with a 2.13" monochrome e-Paper breakout, a battery-backed RTC, and an optional
Li-ion cell. There is no single vendor board this corresponds to — see [pinout.md](pinout.md) for the
wiring, which is yours to change.

| Item | Specification |
|------|------|
| SoC | ESP32-S3 (Xtensa LX7 dual-core, up to 240 MHz) |
| Flash / PSRAM | 16 MB Flash / 8 MB Octal PSRAM |
| Display | 2.13" monochrome e-Paper, **122 × 250**, SSD1680 controller, 4-wire SPI + BUSY |
| RTC | PCF85063A (I2C 0x51), battery-backed |
| Wireless | WiFi 802.11 b/g/n, BLE 5.0 (unused) |
| USB | Type-C — power, programming, native USB Serial/JTAG |
| Buttons | BOOT (GPIO0), USER (GPIO18) |
| Power | 5V USB-C, optional 18650 + charging circuit, battery voltage via ADC divider |

## Notes that matter in practice

**No backlight.** e-Paper is reflective; it is unreadable in the dark and excellent in daylight. This
is the opposite trade from an LCD and it drives the whole visual design — heavy strokes, high
contrast, no greys.

**PSRAM is used but not required.** Two RGB565 draw buffers at 122 × 250 are 61 KB each. `lvgl_bsp`
prefers PSRAM and falls back to internal RAM, so a PSRAM-less S3 still runs. The panel framebuffer
(4000 bytes) is always internal and DMA-capable.

**The RTC is the reason the first screen is correct.** The clock is seeded from it before Wi-Fi comes
up, so the time — and therefore the 일진, which rolls at local midnight — is right immediately rather
than after SNTP. Once online the SNTP-synced time is written back.

**Battery reading is defensive.** Every `board_io` getter returns false/0 rather than blocking if a
device is missing or NAKs, so a depopulated part never wedges the render loop.

## Firmware footprint

At the time of writing: **~1.5 MB** of an 8 MB app partition (82% free). The four subset CJK fonts
are ~69 KB of that; a full Korean face would have been megabytes.
