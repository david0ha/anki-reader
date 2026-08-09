# References

Primary sources. When something about the hardware or the 일진 is uncertain, check here rather than
guessing — a wrong guess in either area produces plausible-looking wrong output rather than an error.

## Display

- **Waveshare 2.13" e-Paper reference driver** —
  [`EPD_2in13_V4.c`](https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_2in13_V4.c) ·
  [`EPD_2in13_V4.h`](https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_2in13_V4.h)
  The init sequence in `epd_panel.c` is transcribed from these. 122 × 250, BUSY active high.
- [Waveshare 2.13inch e-Paper wiki](https://www.waveshare.com/wiki/2.13inch_e-Paper_HAT)
- [`espressif/esp_lcd_ssd1681`](https://components.espressif.com/components/espressif/esp_lcd_ssd1681) —
  the nearest official component (200 × 200); near-identical command set, useful for cross-checking.
  There is no official `esp_lcd_ssd1680`.
- [ESP-IDF LCD peripheral docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/index.html)

## 60갑자 / 일진

- **Liu Y.T., "Sexagenary Cycle (六十干支)"** —
  https://ytliu0.github.io/ChineseCalendar/sexagenary.html
  The anchor: *"the sexagenary date of January 27, 2019 was jiǎ zǐ"*, JD<sub>noon</sub> = 2458511.
- **만세력 일진표** — https://www.sazasaju.com/saju/manseryeok/iljin/2026/8
  Independent Korean cross-check; agrees with the above day for day.
- [한국천문연구원 음양력 변환](https://astro.kasi.re.kr/life/pageView/5) — for further verification.

## Weather

- [Open-Meteo forecast API](https://open-meteo.com/en/docs) — free, no key, no attribution required.
- [Open-Meteo geocoding API](https://open-meteo.com/en/docs/geocoding-api)
- [WMO weather interpretation codes](https://open-meteo.com/en/docs) (in the forecast docs) —
  collapsed to four glyphs by `wx_from_wmo()`.

## Fonts

- **Noto Serif KR**, SIL Open Font License 1.1 —
  https://github.com/notofonts/noto-cjk (`Serif/SubsetOTF/KR/NotoSerifKR-Regular.otf`).
  License text bundled at `components/fortune_core/fonts/OFL.txt`.
- [`lv_font_conv`](https://github.com/lvgl/lv_font_conv) — invoked by `tools/gen_fonts.py`.

## Framework

- [ESP-IDF programming guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/)
- [LVGL v9 docs](https://docs.lvgl.io/master/)
- [PCF85063A datasheet](https://www.nxp.com/docs/en/data-sheet/PCF85063A.pdf)
