# References

Primary sources. When something about the hardware is uncertain, check here rather than guessing —
on this board a wrong guess about the panel produces a blank screen with nothing in the log, which
is the most expensive kind of wrong.

## The panel and the carrier

- **Waveshare 5.83" e-Paper V2 reference driver** —
  [`EPD_5in83_V2.c`](https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_5in83_V2.c)
  Same controller (UC8179), same resolution (648 × 480). The command sequence in `epd_panel.c` is
  transcribed from this. **BUSY is active LOW.**
- **Seeed_GFX UC8179 tables** —
  [`UC8179_Init.h`](https://github.com/Seeed-Studio/Seeed_GFX/blob/master/TFT_Drivers/UC8179_Init.h) ·
  [`UC8179_Defines.h`](https://github.com/Seeed-Studio/Seeed_GFX/blob/master/TFT_Drivers/UC8179_Defines.h)
  Independent cross-check of the same sequence, plus the partial-refresh window commands.
- **EE04 pin routing** —
  [`EPaper_Board_Pins_Setups.h`](https://github.com/Seeed-Studio/Seeed_GFX/blob/master/User_Setups/EPaper_Board_Pins_Setups.h),
  the `USE_XIAO_EPAPER_DISPLAY_BOARD_EE04` branch. This is where `main/user_config.h` comes from —
  not from measurement, not from the EE05 by assumption (though the two branches are identical).
- **Panel/board combination id** —
  [`Setup503_Seeed_XIAO_EPaper_5inch83.h`](https://github.com/Seeed-Studio/Seeed_GFX/blob/master/User_Setups/Setup503_Seeed_XIAO_EPaper_5inch83.h)
  (`BOARD_SCREEN_COMBO 503`), which is what confirms 648 × 480 / UC8179 for this exact panel.
- [Seeed wiki: XIAO ePaper Display Board EE04](https://wiki.seeedstudio.com/epaper_ee04/) —
  buttons, battery ADC, supported panel list.
- [Seeed wiki: ePaper driver board overview](https://wiki.seeedstudio.com/xiao_epaper_display_board_overview/)
- [Seeed store: 5.83" monochrome 648 × 480](https://www.seeedstudio.com/5-83-Monochrome-ePaper-Display-with-648x480-Pixels-p-5785.html)
- [ESP-IDF LCD peripheral docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/index.html)

## Fonts

- **Noto Sans KR**, Regular and Medium, SIL Open Font License 1.1 —
  https://github.com/notofonts/noto-cjk (`Sans/SubsetOTF/KR/`). License text bundled at
  `components/vault_core/fonts/OFL.txt`.
- **KS X 1001 완성형** — the 2350 syllables are derived at generation time from Python's `euc-kr`
  codec (lead `0xB0..0xC8` × trail `0xA1..0xFE`), so there is no table in this repo to fall out of
  date. See `tools/gen_fonts.py`.
- [`lv_font_conv`](https://github.com/lvgl/lv_font_conv) — invoked by `tools/gen_fonts.py`.

## Framework

- [ESP-IDF programming guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/)
- [LVGL v9 docs](https://docs.lvgl.io/master/)

## The project this forked from

- `saju_omi_esp32` — a 2.13" fortune-slip board on an EE05. This repo inherited its structure: the
  draw-and-present split, the captive-portal provisioning, the device HTTP API, the desktop
  simulator, and the habit of writing a host test before believing anything. It shares no content
  code, and deliberately not its mDNS name or AP prefix.
