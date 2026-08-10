#ifndef USER_CONFIG_H
#define USER_CONFIG_H

#include <driver/gpio.h>   /* GPIO_NUM_* used below */

/* ---------------------------------------------------------------------------
 * Board wiring — Seeed XIAO ePaper Display Board EE04 carrying a
 * XIAO ESP32-S3 Plus, driving the 5.83" monochrome e-Paper (UC8179, 648x480)
 * over the board's 24-pin FPC connector.
 *
 * The display half of the EE04 is routed identically to the EE05 (both are
 * "24-pin universal" carriers for the same XIAO footprint); the pins below are
 * transcribed from Seeed's own library rather than inferred — see
 * Seeed_GFX/User_Setups/EPaper_Board_Pins_Setups.h, the
 * USE_XIAO_EPAPER_DISPLAY_BOARD_EE04 branch, which is byte-identical to the
 * EE05 branch.
 *
 *   EE04 net    XIAO pad  GPIO
 *   SPI0_SCL    D8        7
 *   SPI0_MOSI   D10       9
 *   SPI0_CS     D7        44   (UART0 RX by default — console must NOT be UART0)
 *   EDP_DC      D16       10
 *   EDP_RES     D11       38
 *   EDP_BUSY    D3        4    (input, active LOW while the panel is refreshing)
 *   PWR_EN      D6        43   (UART0 TX by default; load switch feeding the
 *                               panel's 3.3V — pulled down, so the panel is
 *                               UNPOWERED until this is driven HIGH)
 *
 * Because GPIO43/44 are the default UART0 pins, the console runs on
 * USB Serial/JTAG (sdkconfig: ESP_CONSOLE_USB_SERIAL_JTAG) — a UART0 console
 * would clock log bytes straight into the panel's power-enable and CS lines.
 *
 * What the EE04 does NOT share with the EE05:
 *
 *   - three user buttons (GPIO2/3/5) instead of one, all active low;
 *   - the battery divider's load switch is on GPIO6 and must be driven HIGH
 *     before the ADC reads anything but noise;
 *   - GPIO5 and GPIO6 are therefore NOT free for I2C, and there is no RTC on
 *     the board at all. The clock is SNTP only.
 * ------------------------------------------------------------------------- */

/* Panel geometry. 648 is a multiple of 8, so a framebuffer row is exactly 81
 * bytes with no off-panel padding — see EPD_STRIDE in epd_panel.h. */
#define EPD_WIDTH      648
#define EPD_HEIGHT     480

#define EPD_SCK_PIN    GPIO_NUM_7
#define EPD_MOSI_PIN   GPIO_NUM_9
#define EPD_CS_PIN     GPIO_NUM_44
#define EPD_DC_PIN     GPIO_NUM_10
#define EPD_RST_PIN    GPIO_NUM_38
#define EPD_BUSY_PIN   GPIO_NUM_4
#define EPD_POWER_PIN  GPIO_NUM_43   /* active HIGH, must be on before init */

/* Buttons, all press-to-GND. KEY0/1/2 are the EE04's three side buttons;
 * BOOT is the button on the XIAO module itself. */
#define BTN_KEY0_PIN   GPIO_NUM_2
#define BTN_KEY1_PIN   GPIO_NUM_3
#define BTN_KEY2_PIN   GPIO_NUM_5
#define BTN_BOOT_PIN   GPIO_NUM_0

/* Battery sensing: a 1:3 divider on GPIO1 (ADC1 channel 0), behind a load
 * switch whose enable is GPIO6. The divider is disconnected — and the reading
 * is meaningless — until that is HIGH. */
#define BATT_ADC_PIN     GPIO_NUM_1
#define BATT_ENABLE_PIN  GPIO_NUM_6

#endif
