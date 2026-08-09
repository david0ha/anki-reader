# The e-Paper panel: driver, refresh policy, self-test

The display is a 2.13" monochrome e-Paper module, **122 × 250**, driven by an **SSD1680** over
4-wire SPI plus a BUSY line. `components/port_bsp/epd_panel.c` is the only file in the project that
talks to it.

## Why this driver, and not a component

Three options were compared:

| Option | Verdict |
|---|---|
| `espressif/esp_lcd_ssd1681` | Official-ish and has an LVGL e-Paper example, but it is built for a 200×200 panel. Adapting it means patching resolution, RAM window and waveform anyway — i.e. all the work of writing it, plus a dependency. |
| `tuanpmt/esp_epaper` | LVGL 9 integration with partial refresh built in, but a third-party component with its own panel table to match. |
| **Port Waveshare's own init sequence** ✅ | The project already drives the panel through `esp_lcd_panel_io_spi` and owns its framebuffer. Adding the SSD1680 command sequence is ~150 lines and zero dependencies, and the refresh policy — the part that actually matters here — has to be written by hand regardless. |

There is **no** official `espressif/esp_lcd_ssd1680` component; that is worth knowing before
searching for one.

The command sequence is transcribed from Waveshare's reference driver,
[`EPD_2in13_V4.c`](https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_2in13_V4.c).
Deviations from it are marked `NOTE:` in the source. There is exactly one: Waveshare spins forever
waiting on BUSY, which hangs the UI task when the panel is unplugged; we time out after 8s, log
loudly, and carry on.

## Geometry

122 is not a multiple of 8. The controller's RAM row is **16 bytes (128 px)**, so the top 6 bits of
every row are off-panel padding.

```
framebuffer = EPD_STRIDE × EPD_PANEL_H = 16 × 250 = 4000 bytes
byte index  = y * 16 + (x >> 3)
bit mask    = 0x80 >> (x & 7)
bit value   = 1 -> white, 0 -> black     (the controller's convention)
```

Allocated with `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`, because the whole 4000 bytes go out in one
`esp_lcd_panel_io_tx_color()` burst.

## Transport

`esp_lcd`'s SPI panel-IO rather than bit-banged GPIO: it drives CS/DC, and — importantly —
`esp_lcd_panel_io_tx_param()` drains any in-flight `tx_color()` DMA before it runs. That is what
makes it safe to follow the 4000-byte RAM burst immediately with the `0x22`/`0x20` update trigger.

## Refresh policy

This is the part with no equivalent on the reflective LCD this project started from, which simply
redrew every 15 seconds.

| Trigger | Mode | Cost |
|---|---|---|
| New fortune, page change, self-test | **full** (`0x22 = 0xF7`) | ~2s, flashes, clears ghosting |
| Clock / battery / weather tick | **partial** (`0x22 = 0xFF`) | ~0.3s, silent, ghosts a little |
| Every 10th partial | promoted to full, automatically | — |

Two rules enforce it:

- **`EPD_PARTIAL_CHAIN_MAX = 10`** in `epd_panel.c`. `epd_refresh_partial()` counts its own calls
  and promotes itself, so nothing upstream has to track ghosting. `epd_partial_chain()` exposes the
  counter, and it is reported in `GET /api/state` as `partialChain` — the policy is observable
  without a serial cable.
- **`MIN_PARTIAL_MS = 55s`** in `user_app.cpp`. Weather, battery and the clock all land on the same
  60s tick; without a floor a burst of updates would strobe the panel.

A full refresh writes the image to **both** the working RAM (`0x24`) and the "previous image" RAM
(`0x26`), so the next partial update diffs against what is actually on the glass. A partial update
re-arms the controller with a short reset pulse and re-sends the window — but deliberately **not**
a `SWRESET`, which would wipe that previous-image RAM.

## Drawing vs presenting

The LVGL flush callback (`main.cpp`) writes pixels into the framebuffer and **does not refresh the
panel**. The caller decides when a change is worth two seconds:

```c
ui_fortune_set_omikuji(&r);   /* and any other setters */
Lvgl_RenderNow();             /* synchronous render -> flush_cb -> framebuffer */
epd_refresh_full();
```

`Lvgl_RenderNow()` exists because LVGL normally renders whenever its task gets around to it, and on
e-Paper the caller has to know the framebuffer is complete before triggering the refresh.

Exactly one task (`UiTask`) may do this. The weather worker and the HTTP server post commands.

## Self-test

`epd_selftest()` sweeps six patterns, each with a full refresh (~10s total):

white → black → 1px checkerboard → 4×4 ordered-dither ramp → border + both diagonals → white.

The ramp and the frame are the interesting ones. A 1-bit panel has no grey, so the "ramp" is a
*density* ramp — it makes a stuck row or column, or a byte-order mistake, obvious in a way a flat
fill cannot. The frame draws the last column (x=121, inside the padded byte) and the last row, which
is exactly where an off-by-one in the stride shows up.

Run it with `POST /api/display/test`. It blocks the UI task, so the handler only queues it.

## LVGL colour format

LVGL renders **RGB565** and the flush callback binarizes at `px < 0x7FFF`. Native 1-bit (`I1`) would
save 122KB of PSRAM, and was not done: RGB565 keeps every widget, font and anti-aliased shape
behaving exactly as it does in the desktop simulator, which renders through the same threshold. The
122KB is affordable; a rendering difference between the simulator and the device is not.

## Still to verify on hardware

- `sdkconfig.defaults` still enables a custom mbedTLS certificate bundle
  (`certs/globalsign_root_ca.pem`). It existed for finnhub.io, which this firmware no longer
  contacts. Open-Meteo is the only HTTPS host left and should validate against IDF's default
  bundle — **confirm that on real hardware, then delete both the setting and the .pem.** Removing it
  untested would break the weather with a TLS error that looks like a network fault.
- The BUSY pin default (GPIO6) is a suggestion, not a measurement. Check it against your wiring.
