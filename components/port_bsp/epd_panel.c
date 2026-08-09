/*
 * epd_panel.c — SSD1680 122x250 e-Paper driver.
 *
 * Command sequence transcribed from Waveshare's reference implementation
 * (waveshareteam/e-Paper @ RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_2in13_V4.c).
 * Deviations from it are deliberate and marked "NOTE:".
 *
 * Transport is esp_lcd's SPI panel-IO rather than bit-banged GPIO: it drives
 * CS/DC for us and, importantly, esp_lcd_panel_io_tx_param() drains any
 * in-flight tx_color() DMA before it runs — which is what makes it safe to
 * follow a 4000-byte RAM burst immediately with the 0x22/0x20 update trigger.
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

/* --- SSD1680 command set (only what we use) ------------------------------ */
#define CMD_DRIVER_OUTPUT      0x01
#define CMD_DEEP_SLEEP         0x10
#define CMD_DATA_ENTRY         0x11
#define CMD_SW_RESET           0x12
#define CMD_TEMP_SENSOR        0x18
#define CMD_UPDATE_CTRL1       0x21
#define CMD_UPDATE_CTRL2       0x22
#define CMD_UPDATE_ACTIVATE    0x20
#define CMD_WRITE_RAM_BW       0x24
#define CMD_WRITE_RAM_PREV     0x26
#define CMD_BORDER_WAVEFORM    0x3C
#define CMD_RAM_X_RANGE        0x44
#define CMD_RAM_Y_RANGE        0x45
#define CMD_RAM_X_COUNTER      0x4E
#define CMD_RAM_Y_COUNTER      0x4F

/* 0x22 payloads: which phases the update sequence runs. */
#define UPD2_FULL              0xF7
#define UPD2_PARTIAL           0xFF

#define BUSY_TIMEOUT_MS        8000

static esp_lcd_panel_io_handle_t s_io;
static uint8_t                  *s_fb;
static epd_pins_t                s_pins;
static bool                      s_ready;
static bool                      s_asleep;
static int                       s_partial_chain;

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

/* Stream the framebuffer into the addressed RAM. tx_color() is the DMA path;
 * the next tx_param() waits for it. */
static void wr_ram(uint8_t cmd)
{
    wr_cmd(cmd);
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(s_io, -1, s_fb, EPD_FB_SIZE));
}

static void hw_reset(void)
{
    gpio_set_level((gpio_num_t)s_pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)s_pins.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level((gpio_num_t)s_pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* BUSY is active HIGH on this controller.
 * NOTE: Waveshare spins forever here. A stuck BUSY means the panel is not
 * wired (or not powered), and hanging the UI task on that is worse than
 * carrying on with a warning — the self-test exists to surface it loudly. */
static bool wait_busy(void)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)BUSY_TIMEOUT_MS * 1000;
    while (gpio_get_level((gpio_num_t)s_pins.busy) == 1) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "BUSY stuck high for %dms — panel wired/powered?", BUSY_TIMEOUT_MS);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    return true;
}

/* Address the whole panel. X is byte-granular (hence >>3), Y is 16-bit. */
static void set_window_full(void)
{
    const uint8_t x[2] = { 0, (EPD_PANEL_W - 1) >> 3 };
    wr_cmd_data(CMD_RAM_X_RANGE, x, sizeof x);

    const uint8_t y[4] = { 0, 0, (EPD_PANEL_H - 1) & 0xFF, ((EPD_PANEL_H - 1) >> 8) & 0xFF };
    wr_cmd_data(CMD_RAM_Y_RANGE, y, sizeof y);

    wr_cmd_u8(CMD_RAM_X_COUNTER, 0);
    const uint8_t yc[2] = { 0, 0 };
    wr_cmd_data(CMD_RAM_Y_COUNTER, yc, sizeof yc);
}

static void driver_output_control(void)
{
    /* MUX = HEIGHT-1 (249 = 0x00F9), gate scan order default. */
    const uint8_t d[3] = { (EPD_PANEL_H - 1) & 0xFF, ((EPD_PANEL_H - 1) >> 8) & 0xFF, 0x00 };
    wr_cmd_data(CMD_DRIVER_OUTPUT, d, sizeof d);
}

static void panel_init_full(void)
{
    hw_reset();
    wait_busy();

    wr_cmd(CMD_SW_RESET);
    wait_busy();

    driver_output_control();
    wr_cmd_u8(CMD_DATA_ENTRY, 0x03);        /* X++, Y++ */
    set_window_full();

    wr_cmd_u8(CMD_BORDER_WAVEFORM, 0x05);

    const uint8_t upd1[2] = { 0x00, 0x80 };
    wr_cmd_data(CMD_UPDATE_CTRL1, upd1, sizeof upd1);

    wr_cmd_u8(CMD_TEMP_SENSOR, 0x80);       /* use the internal sensor */
    wait_busy();

    s_asleep = false;
}

/* Re-arm for a partial update without a SW reset — a SWRESET would wipe the
 * "previous image" RAM that the partial waveform diffs against. */
static void panel_init_partial(void)
{
    gpio_set_level((gpio_num_t)s_pins.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level((gpio_num_t)s_pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(2));

    wr_cmd_u8(CMD_BORDER_WAVEFORM, 0x80);
    driver_output_control();
    wr_cmd_u8(CMD_DATA_ENTRY, 0x03);
    set_window_full();
}

static void turn_on(uint8_t mode)
{
    wr_cmd_u8(CMD_UPDATE_CTRL2, mode);
    wr_cmd(CMD_UPDATE_ACTIVATE);
    wait_busy();
}

/* One-shot connectivity probe. A floating input follows whatever weak pull is
 * applied; a controller output (either polarity) overrides the ~45k internal
 * pulls. Distinguishes "FPC not seated" from "controller present but not the
 * SSD1680 we expect" — both of which look like a dead screen. */
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
                      "Panel not connected: check FPC orientation and latch.");
    } else if (up == 0) {
        ESP_LOGI(TAG, "BUSY driven LOW (SSD1680 idle — or a JD79xxx-family "
                      "panel stuck busy; BWRY panels are NOT SSD1680)");
    } else {
        ESP_LOGE(TAG, "BUSY driven HIGH after init — an SSD1680 idles low. "
                      "BWRY/JD79xxx panel fitted instead of the monochrome one?");
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

    /* Boards like the Seeed EE05 gate the panel's 3.3V behind a load switch
     * with a pulldown on its enable — the panel is dead until this goes HIGH.
     * Power stays on for the life of the app: cutting it would wipe the
     * "previous image" RAM that partial refreshes diff against, and the
     * controller's own deep sleep already gets the panel to ~1uA. */
    if (s_pins.power >= 0) {
        gpio_config_t pwr = {
            .pin_bit_mask = 1ULL << s_pins.power,
            .mode         = GPIO_MODE_OUTPUT,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&pwr), TAG, "power gpio");
        gpio_set_level((gpio_num_t)s_pins.power, 1);
        vTaskDelay(pdMS_TO_TICKS(10));      /* let the panel rail settle */
    }

    gpio_config_t in = {
        .pin_bit_mask = 1ULL << s_pins.busy,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "busy gpio");

    /* DMA-capable: the RAM burst goes out via tx_color(). */
    s_fb = heap_caps_malloc(EPD_FB_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_fb, ESP_ERR_NO_MEM, TAG, "framebuffer");
    memset(s_fb, 0xFF, EPD_FB_SIZE);

    panel_init_full();
    busy_line_probe();
    s_ready = true;

    ESP_LOGI(TAG, "SSD1680 %dx%d up (stride %d, fb %d B)",
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
    if (!s_ready && !s_fb) {
        return;
    }
    if (s_asleep) {
        panel_init_full();
    }
    int64_t t0 = esp_timer_get_time();

    /* Write the image to both the working and the "previous" RAM so the next
     * partial update diffs against what is actually on the glass. */
    wr_ram(CMD_WRITE_RAM_BW);
    wr_ram(CMD_WRITE_RAM_PREV);
    turn_on(UPD2_FULL);

    s_partial_chain = 0;
    ESP_LOGI(TAG, "full refresh %lldms", (esp_timer_get_time() - t0) / 1000);
}

void epd_refresh_partial(void)
{
    if (!s_ready && !s_fb) {
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
    int64_t t0 = esp_timer_get_time();

    panel_init_partial();
    wr_ram(CMD_WRITE_RAM_BW);
    turn_on(UPD2_PARTIAL);

    s_partial_chain++;
    ESP_LOGD(TAG, "partial refresh %lldms (%d/%d)",
             (esp_timer_get_time() - t0) / 1000, s_partial_chain, EPD_PARTIAL_CHAIN_MAX);
}

int epd_partial_chain(void)
{
    return s_partial_chain;
}

void epd_sleep(void)
{
    if (!s_ready || s_asleep) {
        return;
    }
    wr_cmd_u8(CMD_DEEP_SLEEP, 0x01);
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

/* Border + both diagonals: proves the last column (x=121, the padded byte) and
 * the last row are reachable, and shows at a glance if the axes are swapped. */
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

    ESP_LOGI(TAG, "selftest: frame + diagonals");
    pattern_frame();
    epd_refresh_full();

    ESP_LOGI(TAG, "selftest: done, restoring white");
    epd_clear(EPD_WHITE);
    epd_refresh_full();
}
