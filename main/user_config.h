#ifndef USER_CONFIG_H
#define USER_CONFIG_H

#include <driver/gpio.h>   /* GPIO_NUM_* used below */

/* ---------------------------------------------------------------------------
 * Board wiring — Seeed XIAO ePaper Display Board EE05 carrying a
 * XIAO ESP32-S3 Plus, driving the 2.13" monochrome e-Paper (SSD1680, 122x250)
 * over the board's 24-pin FPC connector.
 *
 * These pins are the EE05's fixed routing (schematic
 * XIAO_ePaper_Display_Board_Ex05_V1.0.pdf, sheet "03 XIAO"), expressed as raw
 * ESP32-S3 GPIO numbers. The XIAO silk name is noted for cross-reference.
 *
 *   EE05 net    XIAO pad  GPIO
 *   SPI0_SCL    D8        7
 *   SPI0_MOSI   D10       9
 *   SPI0_CS     D7        44   (UART0 RX by default — console must NOT be UART0)
 *   EDP_DC      D16       10
 *   EDP_RES     D11       38
 *   EDP_BUSY    D3        4    (input, active HIGH while the panel is refreshing)
 *   PWR_EN      D6        43   (UART0 TX by default; TPS22916 load switch feeding
 *                               the panel's 3.3V — 1M pulldown, so the panel is
 *                               UNPOWERED until this is driven HIGH)
 *
 * Because GPIO43/44 are the default UART0 pins, the console runs on
 * USB Serial/JTAG (sdkconfig: ESP_CONSOLE_USB_SERIAL_JTAG) — a UART0 console
 * would clock log bytes straight into the panel's power-enable and CS lines.
 * ------------------------------------------------------------------------- */

/* Panel geometry. 122 is not a multiple of 8: the controller's RAM row is
 * 16 bytes (128 px) wide, so the top 6 bits of each row are padding. See
 * EPD_STRIDE in epd_panel.h. */
#define EPD_WIDTH      122
#define EPD_HEIGHT     250

#define EPD_SCK_PIN    GPIO_NUM_7
#define EPD_MOSI_PIN   GPIO_NUM_9
#define EPD_CS_PIN     GPIO_NUM_44
#define EPD_DC_PIN     GPIO_NUM_10
#define EPD_RST_PIN    GPIO_NUM_38
#define EPD_BUSY_PIN   GPIO_NUM_4
#define EPD_POWER_PIN  GPIO_NUM_43   /* active HIGH, must be on before init */

/* I2C — routed to the EE05's side header (XIAO D4/D5). The EE05 has no
 * onboard RTC; board_io probes and runs without one (SNTP is the clock). */
#define ESP32_I2C_SDA_PIN   GPIO_NUM_5
#define ESP32_I2C_SCL_PIN   GPIO_NUM_6

#endif
