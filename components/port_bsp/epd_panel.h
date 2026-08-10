/*
 * epd_panel.h — UC8179 648x480 monochrome e-Paper panel port.
 *
 * The panel is the Seeed 5.83" monochrome (648x480) on an EE04 driver board.
 * Its controller is the UC8179 — the same part, at the same resolution, as
 * Waveshare's 5.83inch e-Paper V2, so the command sequence here is transcribed
 * from that vendor driver (waveshareteam/e-Paper, EPD_5in83_V2.c) rather than
 * guessed from the datasheet. Deviations are marked "NOTE:".
 *
 * Two traps carried over from the 2.13"/SSD1680 board this code came from, both
 * of which change here:
 *
 *   1. BUSY is **active LOW** on the UC8179 — idle is HIGH. That is the exact
 *      opposite of the SSD1680. Getting it backwards does not fail loudly; it
 *      makes every wait return instantly and every refresh come out torn.
 *   2. 648 is a multiple of 8, so unlike 122 there is no off-panel padding in
 *      the last byte of a row. The stride arithmetic is plain.
 *
 * The important behavioural property is unchanged: **a refresh is not free.**
 * Drawing and presenting are separate. The LVGL flush callback only fills the
 * framebuffer; the application decides when to spend a refresh and which kind:
 *
 *   ... update widgets ...
 *   Lvgl_RenderNow();         // renders -> flush_cb -> epd_set_pixel()
 *   epd_refresh_full();       // or epd_refresh_partial_area(...)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>
#include <driver/spi_master.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_PANEL_W   648
#define EPD_PANEL_H   480

/* 648 / 8 = 81 exactly — every bit in the framebuffer is a real pixel. */
#define EPD_STRIDE    ((EPD_PANEL_W + 7) / 8)      /* 81    */
#define EPD_FB_SIZE   (EPD_STRIDE * EPD_PANEL_H)   /* 38880 */

/* Framebuffer bit convention, matching the controller's 0x13 plane:
 * 1 = white, 0 = black. (Waveshare's Clear() writes 0xFF to make it white.) */
typedef enum {
    EPD_BLACK = 0,
    EPD_WHITE = 1,
} epd_color_t;

/* Ghosting accumulates across partial refreshes, so every Nth partial is
 * promoted to a full one. Lower than the 2.13" board's 10: this panel is ten
 * times the area, and residue on a big white dashboard is far more visible
 * than on a dense fortune slip. */
#define EPD_PARTIAL_CHAIN_MAX  6

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

/* Full update: flashes, clears ghosting, re-bases the panel for later partial
 * updates. Resets the partial chain counter. Expect seconds, not milliseconds
 * — epd_last_full_ms() reports what it actually took on this board. */
void epd_refresh_full(void);

/* Partial update of one rectangle. No flash, leaves faint ghosting.
 *
 * x is snapped outward to a byte boundary (the controller addresses source
 * lines in groups of 8), so the refreshed area may be up to 7 px wider on each
 * side than asked for. That is harmless — the framebuffer content there is
 * already correct — but it is why callers should not rely on the rectangle
 * being exact.
 *
 * Promotes itself to a full refresh once EPD_PARTIAL_CHAIN_MAX partials have
 * accumulated, so ghosting still gets cleared without anyone tracking it. */
void epd_refresh_partial_area(int x1, int y1, int x2, int y2);

/* Partial update of the whole panel. */
void epd_refresh_partial(void);

/* How many partial refreshes have run since the last full one (0..N-1). */
int epd_partial_chain(void);

/* How long the last refresh of each kind actually took, in milliseconds; 0 if
 * that kind has not run yet.
 *
 * These exist because the refresh policy for this panel is meant to be decided
 * from measurement, not from what was true on a panel a tenth the size — and
 * exposing them through /api/info means the measurement can be read off a
 * phone instead of a serial cable. */
int epd_last_full_ms(void);
int epd_last_partial_ms(void);

/* Deep sleep (~1uA). The next refresh transparently re-initialises. */
void epd_sleep(void);

/* Cycle a set of test patterns — white, black, 1px checkerboard, dither ramp,
 * and a border+diagonal frame — each with a full refresh, then restore white.
 * Verifies wiring, pixel addressing and orientation. Blocks for tens of
 * seconds, so call it from a task, never from an HTTP handler. */
void epd_selftest(void);

#ifdef __cplusplus
}
#endif
