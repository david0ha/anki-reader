# Pinout

The board is a **Seeed XIAO ePaper Display Board EE04** carrying a **XIAO ESP32-S3 Plus**; the
5.83" panel plugs into the EE04's 24-pin FPC connector. These are the carrier's fixed routing, not
defaults — but they still live in one place, `main/user_config.h`, and nothing else in the firmware
hardcodes a GPIO. Even the buttons are passed to `user_app` as data, for the same reason
`epd_init()` takes the panel's pins.

## e-Paper (UC8179, 648 × 480, via the EE04 FPC)

| EE04 net | XIAO pad | GPIO | `user_config.h` | Notes |
|---|---|---|---|---|
| SPI0_MOSI | D10 | 9 | `EPD_MOSI_PIN` | SPI MOSI |
| SPI0_SCL | D8 | 7 | `EPD_SCK_PIN` | SPI SCLK, 10 MHz |
| SPI0_CS | D7 | 44 | `EPD_CS_PIN` | driven by `esp_lcd` |
| EDP_DC | D16 | 10 | `EPD_DC_PIN` | driven by `esp_lcd` |
| EDP_RES | D11 | 38 | `EPD_RST_PIN` | output |
| EDP_BUSY | D3 | 4 | `EPD_BUSY_PIN` | **input, active LOW while refreshing** |
| PWR_EN | D6 | 43 | `EPD_POWER_PIN` | **panel power gate, active HIGH** |

SPI host is `SPI3_HOST`. MISO is unused (`-1`).

These are transcribed from Seeed's own library rather than measured or inferred — see
[`Seeed_GFX/User_Setups/EPaper_Board_Pins_Setups.h`](https://github.com/Seeed-Studio/Seeed_GFX/blob/master/User_Setups/EPaper_Board_Pins_Setups.h),
the `USE_XIAO_EPAPER_DISPLAY_BOARD_EE04` branch. It is byte-identical to the EE05 branch, which is
why this fork inherited the display wiring unchanged from the board it came from.

Three traps, all fatal to the display if ignored:

- **BUSY is active LOW on the UC8179** — the panel is idle when the pin is HIGH. That is the exact
  inverse of the SSD1680 this codebase's driver started as, and getting it backwards does not fail
  loudly: every wait returns immediately and every refresh comes out torn. The driver also issues
  `0x71` (GET STATUS) before each sample, because the controller refreshes its BUSY output on that
  command and polling the pin alone can sit on a stale level.
- **PWR_EN gates the panel's 3.3V** through a load switch with a pulldown on its enable. The panel
  is unpowered until GPIO43 is driven HIGH — `epd_init()` does this before the first reset and
  leaves it on (cutting power would wipe the previous-image plane that partial refreshes diff
  against).
- **GPIO43/44 are the ESP32-S3's default UART0 pins.** The console therefore runs on USB
  Serial/JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in `sdkconfig.defaults`); a UART0 console
  would clock every log byte into PWR_EN and CS.

## Buttons

| Button | GPIO | `user_config.h` | Action |
|---|---|---|---|
| KEY0 | 2 | `BTN_KEY0_PIN` | 문제: reveal · 정답: **다시** (again) |
| KEY1 | 3 | `BTN_KEY1_PIN` | 문제: reveal · 정답: **어려움** (hard) |
| KEY2 | 5 | `BTN_KEY2_PIN` | 문제: refresh · 정답: **보통** (good); **held 5 s → reboot into Wi-Fi setup** |
| BOOT | 0 | `BTN_BOOT_PIN` | 문제: reveal · 정답: **쉬움** (easy) |

All four do different things on the two faces: on 정답 each one IS a rating, which is why a grade costs one press. That mapping is not here and not in the
button driver — it is a pure state machine in `components/vault_core/kanji_nav.c`, host-tested from
every button in every reachable state. The driver's whole job is to turn an edge into a
`button_event_t`; what the press *means* is decided somewhere a laptop can check it.

KEY0–2 are the EE04's three side buttons; all are press-to-GND with the internal pull-up enabled.
BOOT is the button on the XIAO module itself — also the bootloader strap pin, so holding it while
pressing RESET enters download mode, which is unrelated to the firmware's use of it.

The long press is detected by *sampling the pin*, not by timing two edges: the driver interrupts on
the press only, so a release generates no event to time against. It is also caught in `user_app`
before the state machine sees anything, because the state machine has no notion of duration.

## Battery

| Signal | XIAO pad | GPIO | `user_config.h` |
|---|---|---|---|
| BAT_ADC | A0 | 1 | `BATT_ADC_PIN` |
| ADC_EN | D5 | 6 | `BATT_ENABLE_PIN` |

A 1:3 divider on GPIO1 (ADC1_CH0) behind a load switch. **The divider is disconnected — and the
reading meaningless — until GPIO6 is driven HIGH**, which `board_io_init()` does and leaves on. A
reading below 2.5 V is reported as "no cell fitted" rather than as a flat battery: a Li-ion whose
protection circuit has cut off never presents that voltage, so it means USB-only operation, and
showing an empty battery there is a false alarm somebody will chase.

The JST 2.0 battery input also passes a hardware slide switch; it must be ON for battery operation.

## No I2C, no RTC

The EE05 this project forked from routed GPIO5/GPIO6 to an I2C side header. On the **EE04 those
same two pins are KEY2 and the battery divider's enable**, and the board has no RTC of its own. The
I2C bus and the PCF85063A driver are therefore gone entirely.

Nothing misses them. This UI prints no clock and computes no interval: every span on the glass —
`9일 뒤`, `10분 뒤`, `복습` — is worded by `tools/kanji_server.py` against the *server's* clock and
arrives as a string. Adding an RTC would give the board a number it has no use for.

## Unused

No audio codec, SD card, touch controller or backlight — e-Paper has no backlight, and none of the
rest is wired on this build. The EE04's 50-pin FPC connector (for Spectra6 colour panels) is
unpopulated in this configuration.
