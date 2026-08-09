/*
 * epd_panel.h — SSD1680 122x250 monochrome e-Paper panel port.
 *
 * Replaces the ST7305/ST7306 reflective-LCD port this project started from.
 * The command sequence follows Waveshare's own 2.13" V4 reference driver
 * (waveshareteam/e-Paper, EPD_2in13_V4.c) — see docs/epaper-2in13.md for why
 * that source was chosen over the esp_lcd_ssd1681 component.
 *
 * The important behavioural difference from an LCD: **a refresh is not free.**
 * A full refresh takes ~2s and flashes the panel; a partial refresh is ~0.3s
 * and silent but leaves ghosting that accumulates. So drawing and presenting
 * are separate here: the LVGL flush callback only fills the framebuffer, and
 * the application decides when (and how) to push it to the glass.
 *
 *   ... update widgets ...
 *   lv_refr_now(NULL);        // renders -> flush_cb -> epd_set_pixel()
 *   epd_refresh_full();       // or epd_refresh_partial()
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>
#include <driver/spi_master.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_PANEL_W   122
#define EPD_PANEL_H   250

/* The controller's RAM row is byte-granular and 122 px needs 16 bytes; the
 * high 6 bits of the last byte in every row are off-panel padding. */
#define EPD_STRIDE    ((EPD_PANEL_W + 7) / 8)      /* 16 */
#define EPD_FB_SIZE   (EPD_STRIDE * EPD_PANEL_H)   /* 4000 */

/* Framebuffer bit convention, matching the controller: 1 = white, 0 = black. */
typedef enum {
    EPD_BLACK = 0,
    EPD_WHITE = 1,
} epd_color_t;

/* Ghosting accumulates across partial refreshes, so every Nth partial is
 * silently promoted to a full one. */
#define EPD_PARTIAL_CHAIN_MAX  10

typedef struct {
    int sck;
    int mosi;
    int cs;
    int dc;
    int rst;
    int busy;
    int power;              /* panel power-enable GPIO, active HIGH; -1 if the
                               panel is hardwired to 3.3V */
    spi_host_device_t host;
} epd_pins_t;

/* Bring up SPI + GPIO, reset and initialise the controller, and clear the
 * panel to white. Safe to call once; returns ESP_ERR_INVALID_STATE after. */
esp_err_t epd_init(const epd_pins_t *pins);

/* --- framebuffer (no panel traffic) ------------------------------------- */

void epd_clear(epd_color_t color);
void epd_set_pixel(uint16_t x, uint16_t y, epd_color_t color);

/* Raw framebuffer access, for the self-test patterns and unit-style checks. */
uint8_t *epd_framebuffer(void);

/* --- presenting ---------------------------------------------------------- */

/* Full update: ~2s, flashes, clears ghosting. Also re-arms the panel's
 * "previous image" RAM so subsequent partial updates have a correct base.
 * Resets the partial chain counter. */
void epd_refresh_full(void);

/* Partial update: ~0.3s, no flash, leaves faint ghosting. Automatically
 * promotes itself to a full refresh once EPD_PARTIAL_CHAIN_MAX partials have
 * accumulated since the last full one. */
void epd_refresh_partial(void);

/* How many partial refreshes have run since the last full one (0..N-1).
 * Exposed so the UI task can log/verify the refresh policy. */
int epd_partial_chain(void);

/* Deep sleep (~1uA). The next refresh transparently re-initialises. */
void epd_sleep(void);

/* Cycle a set of test patterns — white, black, 1px checkerboard, dither ramp,
 * and a border+diagonal frame — each with a full refresh, then restore white.
 * Verifies wiring, pixel addressing and orientation. Blocks for ~10s, so call
 * it from a task, never from an HTTP handler. */
void epd_selftest(void);

#ifdef __cplusplus
}
#endif
