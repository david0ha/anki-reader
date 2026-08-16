/*
 * epd_panel.c — UC8179 648x480 e-Paper driver.
 *
 * Command sequence transcribed from Waveshare's reference implementation for
 * the panel that shares this controller and resolution
 * (waveshareteam/e-Paper @ RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_5in83_V2.c),
 * cross-checked against Seeed's own UC8179 tables in
 * Seeed_GFX/TFT_Drivers/UC8179_{Init,Defines}.h. Deviations are marked "NOTE:".
 *
 * Transport is esp_lcd's SPI panel-IO rather than bit-banged GPIO: it drives
 * CS/DC for us and, importantly, esp_lcd_panel_io_tx_param() drains any
 * in-flight tx_color() DMA before it runs — which is what makes it safe to
 * follow a 38880-byte RAM burst immediately with the 0x12 update trigger.
 */
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "epd_panel.h"

static const char *TAG = "epd";

/* --- UC8179 command set (only what we use) ------------------------------- */
#define CMD_PANEL_SETTING      0x00
#define CMD_POWER_SETTING      0x01
#define CMD_POWER_OFF          0x02
#define CMD_POWER_ON           0x04
#define CMD_BOOSTER_SOFT_START 0x06
#define CMD_DEEP_SLEEP         0x07
#define CMD_WRITE_RAM_OLD      0x10   /* "previous image" plane */
#define CMD_DISPLAY_REFRESH    0x12
#define CMD_WRITE_RAM_NEW      0x13   /* the plane our framebuffer maps to */
#define CMD_DUAL_SPI           0x15
#define CMD_VCOM_INTERVAL      0x50
#define CMD_TCON_SETTING       0x60
#define CMD_RESOLUTION         0x61
#define CMD_GET_STATUS         0x71
#define CMD_PARTIAL_WINDOW     0x90
#define CMD_PARTIAL_IN         0x91
#define CMD_PARTIAL_OUT        0x92
#define CMD_CASCADE_SETTING    0xE0
#define CMD_FORCE_TEMP         0xE5

#define DEEP_SLEEP_MAGIC       0xA5

/* First byte of 0x50 (VCOM and data interval). Its high bits select the border
 * data and the DATA POLARITY — which way a RAM bit maps to black or white — and
 * its low nibble is the interval itself. So this register decides, among other
 * things, whether the panel prints the frame or its negative.
 *
 * 0x21 is what EPD_5IN83_V2_Init() sends for a full refresh, and 0xA9 is what
 * Display_Partial() sends for a windowed one. Note that those two agree in the
 * polarity bits (0x21 -> 0b0010_0001, 0xA9 -> 0b1010_1001) and differ only in
 * border and interval — which is the check that says the pair is right.
 *
 * This driver used to send 0x10 (0b0001_0000) for the full case. It was the
 * only register left in the init sequence that disagreed with the reference,
 * and it disagreed in exactly the bits that choose polarity: every full refresh
 * printed the negative of the card — crisp, correctly laid out, and inverted,
 * white type on a black field. The windowed refresh, using the vendor's own
 * 0xA9, was unaffected, so the dock was the one region on the glass coming out
 * the right way round. */
#define VCOM_FULL_CDI          0x21
#define VCOM_PARTIAL_CDI       0xA9

/* A 5.83" full refresh is seconds, and the controller holds BUSY low for all of
 * it. 30s is "the panel is not there", not "the panel is slow". */
#define BUSY_TIMEOUT_MS        30000

static esp_lcd_panel_io_handle_t s_io;
static uint8_t                  *s_fb;
static epd_pins_t                s_pins;
static bool                      s_ready;
static bool                      s_asleep;
static bool                      s_partial_mode;
static int                       s_partial_chain;
static int                       s_last_full_ms;
static int                       s_last_partial_ms;

/* --- low-level ----------------------------------------------------------- */

static void wr_cmd(uint8_t cmd)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, cmd, NULL, 0));
}

static void wr_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, cmd, data, len));
}

static void wr_cmd_u8(uint8_t cmd, uint8_t v)
{
    wr_cmd_data(cmd, &v, 1);
}

static void hw_reset(void)
{
    gpio_set_level((gpio_num_t)s_pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)s_pins.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(4));
    gpio_set_level((gpio_num_t)s_pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

/*
 * BUSY is active LOW on this controller — the inverse of the SSD1680 this
 * project's driver started as. Waveshare pokes 0x71 (GET STATUS) before each
 * sample; the UC8179 refreshes its BUSY output on that command, and polling the
 * pin alone can sit on a stale level.
 *
 * NOTE: Waveshare spins forever here. A stuck BUSY means the panel is not wired
 * (or not powered), and hanging the UI task on that is worse than carrying on
 * with a warning — the self-test exists to surface it loudly.
 */
static bool wait_busy(void)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)BUSY_TIMEOUT_MS * 1000;
    for (;;) {
        wr_cmd(CMD_GET_STATUS);
        if (gpio_get_level((gpio_num_t)s_pins.busy) == 1) break;   /* idle */
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "BUSY stuck low for %dms — panel wired/powered?", BUSY_TIMEOUT_MS);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* Waveshare's 200ms settle. It is not politeness: the panel's charge pump
     * is still winding down and an immediate next command can be swallowed. */
    vTaskDelay(pdMS_TO_TICKS(200));
    return true;
}

/* Stream `len` bytes into whichever RAM plane was last addressed.
 *
 * The whole 38880-byte plane goes out in one call: esp_lcd splits a large colour
 * buffer into bus-sized chunks itself and holds CS asserted across them, so this
 * does not need chunking here. tx_color() is the DMA path; the next tx_param()
 * drains it before running. */
static void wr_ram(uint8_t cmd, const uint8_t *data, size_t len)
{
    wr_cmd(cmd);
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(s_io, -1, data, len));
}

static void panel_init_full(void)
{
    hw_reset();

    const uint8_t power[4] = { 0x07, 0x07, 0x3F, 0x3F };  /* VGH20 VGL-20 VDH15 VDL-15 */
    wr_cmd_data(CMD_POWER_SETTING, power, sizeof power);

    /* "Enhanced display drive" — Waveshare added this to stop the larger
     * panels ghosting at the far end of the source lines. */
    const uint8_t boost[4] = { 0x17, 0x17, 0x28, 0x17 };
    wr_cmd_data(CMD_BOOSTER_SOFT_START, boost, sizeof boost);

    wr_cmd(CMD_POWER_ON);
    vTaskDelay(pdMS_TO_TICKS(100));
    wait_busy();

    wr_cmd_u8(CMD_PANEL_SETTING, 0x1F);        /* KW mode, LUT from OTP */

    const uint8_t res[4] = {
        (EPD_PANEL_W >> 8) & 0xFF, EPD_PANEL_W & 0xFF,     /* 0x02 0x88 = 648 */
        (EPD_PANEL_H >> 8) & 0xFF, EPD_PANEL_H & 0xFF,     /* 0x01 0xE0 = 480 */
    };
    wr_cmd_data(CMD_RESOLUTION, res, sizeof res);

    wr_cmd_u8(CMD_DUAL_SPI, 0x00);             /* single-SPI source data */

    const uint8_t vcom[2] = { VCOM_FULL_CDI, 0x07 };
    wr_cmd_data(CMD_VCOM_INTERVAL, vcom, sizeof vcom);

    wr_cmd_u8(CMD_TCON_SETTING, 0x22);

    s_asleep       = false;
    s_partial_mode = false;
}

/* Re-arm for partial updates. Unlike the SSD1680 port this replaces, the panel
 * is NOT reset here — a reset would drop the "previous image" plane the partial
 * waveform diffs against, which is the whole point.
 *
 * 0xE0 = 0x02 is TSFIX: stop reading the on-chip temperature sensor and use
 * whatever 0xE5 says. The controller picks its OTP waveform by temperature, so
 * pinning it at 0x6E is what selects the short waveform that makes a partial
 * ~600ms instead of ~4s. Every one of these has to come back off in
 * panel_leave_partial() — see the note there. */
static void panel_enter_partial(void)
{
    if (s_partial_mode) return;
    wr_cmd_u8(CMD_CASCADE_SETTING, 0x02);
    wr_cmd_u8(CMD_FORCE_TEMP, 0x6E);           /* fixed waveform temperature */
    s_partial_mode = true;
}

static void panel_leave_partial(void)
{
    if (!s_partial_mode) return;
    wr_cmd(CMD_PARTIAL_OUT);
    /* Restore the full-refresh VCOM/data interval. Partial mode sets 0xA9,
     * which leaves the un-refreshed area of the panel visibly darkened; leaving
     * it in place means the NEXT full refresh inherits it and comes out with a
     * grey cast that looks like a failing panel rather than a wrong register. */
    const uint8_t vcom[2] = { VCOM_FULL_CDI, 0x07 };
    wr_cmd_data(CMD_VCOM_INTERVAL, vcom, sizeof vcom);

    /* Hand the waveform temperature back to the on-chip sensor. This is the
     * half that used to be missing, and it is the expensive one: 0x92 only
     * closes the window, so with TSFIX still set the panel stayed on the
     * partial waveform and every subsequent FULL refresh ran it too — 699ms
     * where the same call took 3959ms before the first partial. Four hundred
     * milliseconds of drive is nowhere near enough to carry a pixel all the way
     * to black or all the way to white, so the previous frame stayed half
     * printed under the new one: black streaks through white type, and patches
     * of the panel lighter or darker than their neighbours. It reads as a
     * failing panel, and nothing logs it — the only trace is the refresh time
     * in this driver's own log line, which is why epd_refresh_full() now
     * complains when that number collapses.
     *
     * 0xE5 needs no counterpart write: with TSFIX clear its value is ignored. */
    wr_cmd_u8(CMD_CASCADE_SETTING, 0x00);
    s_partial_mode = false;
}

static void turn_on(void)
{
    wr_cmd(CMD_DISPLAY_REFRESH);
    /* Waveshare: "The delay here is necessary, 200uS at least" — the controller
     * needs to raise BUSY before we start believing the pin. */
    vTaskDelay(pdMS_TO_TICKS(100));
    wait_busy();
}

/* One-shot connectivity probe. A floating input follows whatever weak pull is
 * applied; a controller output (either polarity) overrides the ~45k internal
 * pulls. Distinguishes "FPC not seated" from "a controller is there but it is
 * not idling the way a UC8179 does" — both of which look like a dead screen. */
static void busy_line_probe(void)
{
    const gpio_num_t b = (gpio_num_t)s_pins.busy;

    gpio_set_pull_mode(b, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(2));
    int up = gpio_get_level(b);

    gpio_set_pull_mode(b, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(2));
    int dn = gpio_get_level(b);

    gpio_set_pull_mode(b, GPIO_FLOATING);

    if (up == 1 && dn == 0) {
        ESP_LOGE(TAG, "BUSY follows the weak pulls — nothing is driving it. "
                      "Panel not connected: check the 24-pin FPC orientation and latch.");
    } else if (up == 1 && dn == 1) {
        ESP_LOGI(TAG, "BUSY driven HIGH — UC8179 idle, as expected");
    } else {
        ESP_LOGW(TAG, "BUSY driven LOW after init — the controller thinks it is "
                      "still busy. Wrong panel on the FPC, or the 5.83\" ordered "
                      "as a colour variant?");
    }
}

/* --- public -------------------------------------------------------------- */

esp_err_t epd_init(const epd_pins_t *pins)
{
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    s_pins = *pins;

    spi_bus_config_t buscfg = {
        .mosi_io_num     = s_pins.mosi,
        .miso_io_num     = -1,
        .sclk_io_num     = s_pins.sck,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = EPD_FB_SIZE + 64,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(s_pins.host, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num       = s_pins.cs,
        .dc_gpio_num       = s_pins.dc,
        .spi_mode          = 0,
        .pclk_hz           = 10 * 1000 * 1000,
        .trans_queue_depth = 4,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)s_pins.host, &io_config, &s_io),
        TAG, "panel io");

    gpio_config_t out = {
        .pin_bit_mask = 1ULL << s_pins.rst,
        .mode         = GPIO_MODE_OUTPUT,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "rst gpio");

    /* The EE04 gates the panel's 3.3V behind a load switch with a pulldown on
     * its enable — the panel is dead until this goes HIGH. Power stays on for
     * the life of the app: cutting it would wipe the "previous image" plane
     * that partial refreshes diff against, and the controller's own deep sleep
     * already gets the panel to ~1uA. */
    if (s_pins.power >= 0) {
        gpio_config_t pwr = {
            .pin_bit_mask = 1ULL << s_pins.power,
            .mode         = GPIO_MODE_OUTPUT,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&pwr), TAG, "power gpio");
        gpio_set_level((gpio_num_t)s_pins.power, 1);
        vTaskDelay(pdMS_TO_TICKS(20));      /* let the panel rail settle */
    }

    gpio_config_t in = {
        .pin_bit_mask = 1ULL << s_pins.busy,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "busy gpio");

    /* DMA-capable: the whole plane goes out in one tx_color(). 39KB of the
     * S3's internal SRAM, which is the one thing PSRAM cannot stand in for. */
    s_fb = heap_caps_malloc(EPD_FB_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_fb, ESP_ERR_NO_MEM, TAG, "framebuffer");
    memset(s_fb, 0xFF, EPD_FB_SIZE);

    panel_init_full();
    busy_line_probe();
    s_ready = true;

    ESP_LOGI(TAG, "UC8179 %dx%d up (stride %d, fb %d B)",
             EPD_PANEL_W, EPD_PANEL_H, EPD_STRIDE, EPD_FB_SIZE);

    epd_refresh_full();     /* land on a known-clean white panel */
    return ESP_OK;
}

void epd_clear(epd_color_t color)
{
    if (s_fb) {
        memset(s_fb, color == EPD_WHITE ? 0xFF : 0x00, EPD_FB_SIZE);
    }
}

void epd_set_pixel(uint16_t x, uint16_t y, epd_color_t color)
{
    if (x >= EPD_PANEL_W || y >= EPD_PANEL_H) {
        return;
    }
    uint8_t *p    = &s_fb[(size_t)y * EPD_STRIDE + (x >> 3)];
    uint8_t  mask = 0x80 >> (x & 7);
    if (color == EPD_WHITE) {
        *p |= mask;
    } else {
        *p &= (uint8_t)~mask;
    }
}

uint8_t *epd_framebuffer(void)
{
    return s_fb;
}

void epd_refresh_full(void)
{
    if (!s_fb) {
        return;
    }
    if (s_asleep) {
        panel_init_full();
    }
    panel_leave_partial();
    int64_t t0 = esp_timer_get_time();

    /* Both planes get the outgoing frame, which is what EPD_5IN83_V2_Display()
     * does — its 0x10 loop and its 0x13 loop send the same `Image` bytes, and
     * Clear() likewise sends 0xFF to both to land on white.
     *
     * An earlier version of this file primed 0x10 with 0x00 instead, under a
     * comment claiming that was Waveshare's own trick for clearing ghosting.
     * It is not: no 0x00 fill appears anywhere in that driver. The consequence
     * was not a subtle one. In KW mode the OTP LUT is indexed by the (old, new)
     * pair, and because the vendor always writes old == new, only the WW and BB
     * entries are ever exercised by a real full refresh. Priming old to all-zero
     * drove every background pixel down the mixed BW path instead — a path this
     * panel's OTP waveform ends on the opposite rail. The panel therefore built
     * the correct image early, in the phase driven by the 0x13 data, and then
     * spent the rest of the waveform carrying it to its inverse: a sharp,
     * fully-driven, entirely black-backgrounded card. Sharp is the tell. An
     * under-driven panel is streaky; this one was crisp and simply wrong, which
     * is what separates this bug from the partial-waveform one above. */
    wr_ram(CMD_WRITE_RAM_OLD, s_fb, EPD_FB_SIZE);
    wr_ram(CMD_WRITE_RAM_NEW, s_fb, EPD_FB_SIZE);
    turn_on();

    s_partial_chain = 0;
    s_last_full_ms  = (int)((esp_timer_get_time() - t0) / 1000);
    /* The one symptom this failure has. A full refresh that returns early did
     * not fail — it under-drove the panel, and the only place that shows is
     * here. See panel_leave_partial(). */
    if (s_last_full_ms < EPD_FULL_REFRESH_MIN_MS) {
        ESP_LOGE(TAG, "full refresh %dms — under %dms means the waveform is "
                      "still the partial one (0xE0/0xE5 not restored). The "
                      "panel is under-driven and the previous frame will show "
                      "through.", s_last_full_ms, EPD_FULL_REFRESH_MIN_MS);
    }
    ESP_LOGI(TAG, "full refresh %dms", s_last_full_ms);
}

void epd_refresh_partial_area(int x1, int y1, int x2, int y2)
{
    if (!s_fb) {
        return;
    }
    if (s_asleep) {
        panel_init_full();
    }
    if (s_partial_chain >= EPD_PARTIAL_CHAIN_MAX) {
        ESP_LOGI(TAG, "partial chain hit %d — promoting to full", EPD_PARTIAL_CHAIN_MAX);
        epd_refresh_full();
        return;
    }

    /* Clamp, then snap x outward to byte boundaries: the controller addresses
     * source lines in groups of eight, and a window that starts mid-byte comes
     * out shifted rather than clipped. */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > EPD_PANEL_W) x2 = EPD_PANEL_W;
    if (y2 > EPD_PANEL_H) y2 = EPD_PANEL_H;
    x1 &= ~7;
    x2 = (x2 + 7) & ~7;
    if (x2 <= x1 || y2 <= y1) {
        return;
    }

    int64_t t0 = esp_timer_get_time();

    /* 0xA9/0x07: the partial-mode VCOM and data interval Waveshare uses. It
     * differs from the full-refresh 0x10/0x07 — with the full-refresh value the
     * un-refreshed area of the panel visibly darkens. */
    const uint8_t vcom[2] = { VCOM_PARTIAL_CDI, 0x07 };
    wr_cmd_data(CMD_VCOM_INTERVAL, vcom, sizeof vcom);

    panel_enter_partial();
    wr_cmd(CMD_PARTIAL_IN);

    const uint8_t win[9] = {
        (uint8_t)((x1 >> 8) & 0xFF), (uint8_t)(x1 & 0xF8),
        (uint8_t)(((x2 - 1) >> 8) & 0xFF), (uint8_t)((x2 - 1) | 0x07),
        (uint8_t)((y1 >> 8) & 0xFF), (uint8_t)(y1 & 0xFF),
        (uint8_t)(((y2 - 1) >> 8) & 0xFF), (uint8_t)((y2 - 1) & 0xFF),
        0x01,
    };
    wr_cmd_data(CMD_PARTIAL_WINDOW, win, sizeof win);

    /* Only the window's bytes go out, row by row — a partial that streamed the
     * whole framebuffer would land the wrong pixels in the window. */
    const int byte_x = x1 >> 3;
    const int byte_w = (x2 - x1) >> 3;
    wr_cmd(CMD_WRITE_RAM_NEW);
    for (int y = y1; y < y2; y++) {
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(
            s_io, -1, &s_fb[(size_t)y * EPD_STRIDE + byte_x], byte_w));
    }
    turn_on();

    s_partial_chain++;
    s_last_partial_ms = (int)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "partial refresh %dms  x[%d..%d) y[%d..%d)  (%d/%d)",
             s_last_partial_ms, x1, x2, y1, y2, s_partial_chain, EPD_PARTIAL_CHAIN_MAX);
}

void epd_refresh_partial(void)
{
    epd_refresh_partial_area(0, 0, EPD_PANEL_W, EPD_PANEL_H);
}

int epd_partial_chain(void)  { return s_partial_chain; }
int epd_last_full_ms(void)   { return s_last_full_ms; }
int epd_last_partial_ms(void){ return s_last_partial_ms; }

void epd_sleep(void)
{
    if (!s_ready || s_asleep) {
        return;
    }
    panel_leave_partial();
    /* Float the border before powering down, or the panel edge holds a charge
     * and prints a grey frame over the next few hours. */
    wr_cmd_u8(CMD_VCOM_INTERVAL, 0xF7);
    wr_cmd(CMD_POWER_OFF);
    wait_busy();
    wr_cmd_u8(CMD_DEEP_SLEEP, DEEP_SLEEP_MAGIC);
    vTaskDelay(pdMS_TO_TICKS(100));
    s_asleep = true;
}

/* --- self-test ----------------------------------------------------------- */

/* 4x4 ordered-dither thresholds. A 1-bit panel has no grey, so the "ramp" is
 * a density ramp: it makes stuck rows/columns and byte-order mistakes obvious
 * in a way a flat fill cannot. */
static const uint8_t BAYER4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static void pattern_checker(void)
{
    for (int y = 0; y < EPD_PANEL_H; y++) {
        for (int x = 0; x < EPD_PANEL_W; x++) {
            epd_set_pixel(x, y, ((x ^ y) & 1) ? EPD_WHITE : EPD_BLACK);
        }
    }
}

static void pattern_ramp(void)
{
    for (int y = 0; y < EPD_PANEL_H; y++) {
        int level = (y * 16) / EPD_PANEL_H;          /* 0 (white) .. 15 (black) */
        for (int x = 0; x < EPD_PANEL_W; x++) {
            bool on = BAYER4[y & 3][x & 3] < level;
            epd_set_pixel(x, y, on ? EPD_BLACK : EPD_WHITE);
        }
    }
}

/* Border + both diagonals: proves the last column and the last row are
 * reachable, and shows at a glance if the axes are swapped or the source
 * lines are reversed. */
static void pattern_frame(void)
{
    epd_clear(EPD_WHITE);
    for (int x = 0; x < EPD_PANEL_W; x++) {
        epd_set_pixel(x, 0, EPD_BLACK);
        epd_set_pixel(x, EPD_PANEL_H - 1, EPD_BLACK);
        int y = (x * (EPD_PANEL_H - 1)) / (EPD_PANEL_W - 1);
        epd_set_pixel(x, y, EPD_BLACK);
        epd_set_pixel(x, EPD_PANEL_H - 1 - y, EPD_BLACK);
    }
    for (int y = 0; y < EPD_PANEL_H; y++) {
        epd_set_pixel(0, y, EPD_BLACK);
        epd_set_pixel(EPD_PANEL_W - 1, y, EPD_BLACK);
    }
    /* A solid block in the top-left quadrant only: an unambiguous "this corner
     * is the origin", which the symmetric patterns above cannot give. */
    for (int y = 20; y < 60; y++) {
        for (int x = 20; x < 100; x++) epd_set_pixel(x, y, EPD_BLACK);
    }
}

void epd_selftest(void)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "selftest: panel not initialised");
        return;
    }
    ESP_LOGI(TAG, "selftest: white");
    epd_clear(EPD_WHITE);
    epd_refresh_full();

    ESP_LOGI(TAG, "selftest: black");
    epd_clear(EPD_BLACK);
    epd_refresh_full();

    ESP_LOGI(TAG, "selftest: checkerboard");
    pattern_checker();
    epd_refresh_full();

    ESP_LOGI(TAG, "selftest: dither ramp");
    pattern_ramp();
    epd_refresh_full();

    ESP_LOGI(TAG, "selftest: frame + diagonals + origin block");
    pattern_frame();
    epd_refresh_full();

    /* The partial path gets exercised too, and timed — this is where the
     * refresh policy's numbers come from. */
    ESP_LOGI(TAG, "selftest: partial (centre band)");
    for (int y = 200; y < 280; y++) {
        for (int x = 0; x < EPD_PANEL_W; x++) epd_set_pixel(x, y, EPD_WHITE);
    }
    epd_refresh_partial_area(0, 200, EPD_PANEL_W, 280);

    ESP_LOGI(TAG, "selftest: done — full %dms, partial %dms; restoring white",
             s_last_full_ms, s_last_partial_ms);
    epd_clear(EPD_WHITE);
    epd_refresh_full();
}
