# Pinout

The board is a **Seeed XIAO ePaper Display Board EE05** carrying a **XIAO ESP32-S3 Plus**; the
2.13" panel plugs into the EE05's 24-pin FPC connector. The routing below is fixed by the EE05
(schematic `XIAO_ePaper_Display_Board_Ex05_V1.0.pdf`), so unlike a hand-wired breakout these are
facts, not defaults. They still live in one place — `main/user_config.h` — and nothing else in the
firmware hardcodes an e-Paper GPIO.

## e-Paper (SSD1680, 122 × 250, via the EE05 FPC)

| EE05 net | XIAO pad | GPIO | `user_config.h` | Notes |
|---|---|---|---|---|
| SPI0_MOSI | D10 | 9 | `EPD_MOSI_PIN` | SPI MOSI |
| SPI0_SCL | D8 | 7 | `EPD_SCK_PIN` | SPI SCLK, 10 MHz |
| SPI0_CS | D7 | 44 | `EPD_CS_PIN` | driven by `esp_lcd` |
| EDP_DC | D16 | 10 | `EPD_DC_PIN` | driven by `esp_lcd` |
| EDP_RES | D11 | 38 | `EPD_RST_PIN` | output |
| EDP_BUSY | D3 | 4 | `EPD_BUSY_PIN` | **input, active HIGH while refreshing** |
| PWR_EN | D6 | 43 | `EPD_POWER_PIN` | **panel power gate, active HIGH** |

SPI host is `SPI3_HOST`. MISO is unused (`-1`).

Two EE05-specific traps, both fatal to the display if ignored:

- **PWR_EN gates the panel's 3.3V** through a TPS22916 load switch with a 1MΩ pulldown on its
  enable. The panel is unpowered until GPIO43 is driven HIGH — `epd_init()` does this before the
  first reset and leaves it on (cutting power would wipe the previous-image RAM that partial
  refreshes diff against).
- **GPIO43/44 are the ESP32-S3's default UART0 pins.** The console therefore runs on USB
  Serial/JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in `sdkconfig.defaults`); a UART0 console
  would clock every log byte into PWR_EN and CS.

## I2C — side header only

| Signal | XIAO pad | GPIO | `user_config.h` |
|---|---|---|---|
| SDA | D4 | 5 | `ESP32_I2C_SDA_PIN` |
| SCL | D5 | 6 | `ESP32_I2C_SCL_PIN` |

The EE05 has **no onboard RTC**. `board_io` probes address `0x51` (PCF85063A) at boot and runs
without one if nothing ACKs — the clock then comes from SNTP alone, seeded at first Wi-Fi connect.

## Buttons

| Button | GPIO | Action |
|---|---|---|
| USER | 2 | short press → draw a new fortune |
| BOOT | 0 | short press → switch page (오미쿠지 ↔ 홈) |
| USER + BOOT | — | held 5s → reboot into Wi-Fi setup (AP) mode |

USER is the EE05's side button 1 (net BOTTON1, external 10K pull-up); the board's other two side
buttons (GPIO3, GPIO8) are unused. BOOT is the button on the XIAO module itself — also the
bootloader strap pin: holding it while pressing RESET enters download mode, which is unrelated to
the firmware's use of it.

## Battery

BAT_ADC is XIAO D0 = GPIO1 (ADC1_CH0), a 10K/10K divider behind a TPS22916 load switch whose
enable (net ADC_EN) the firmware does not drive yet — so the reading sits near 0V until that is
wired up. Harmless on USB power. The JST 2.0 battery input also passes a hardware slide switch;
it must be ON for battery operation.

## Unused

No audio codec, SD card, touch controller or backlight — e-Paper has no backlight, and none of the
rest is wired on this build. The nRF52840-only NFC nets on the EE05 are unconnected with a XIAO
ESP32-S3 Plus fitted.
