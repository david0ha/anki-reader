
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <esp_timer.h>
#include <esp_log.h>

#include "epd_panel.h"
#include "lvgl_bsp.h"
#include "saju.h"          /* saju_jdn — see utc_tm_to_epoch below           */
#include "ui_fortune.h"    /* the setup overlay doubles as the status screen */
#include "user_app.h"
#include "user_config.h"
#include "provisioning.h"
#include "net_time.h"
#include "board_io.h"
#include "device_api.h"
#include "sdkconfig.h"

static const char *TAG = "main";

/*
 * LVGL renders RGB565 into its own buffer; this callback binarizes into the
 * panel's 1-bit framebuffer. Keeping LVGL on RGB565 rather than its I1 format
 * costs 122KB of PSRAM and buys every widget, font and anti-aliased shape
 * working exactly as it does in the desktop simulator, which renders through
 * this same threshold.
 *
 * It does NOT refresh the panel. On e-Paper that is a ~2s flashing operation
 * and belongs to whoever knows what changed — see present() in user_app.cpp.
 */
static void Lvgl_FlushCallback(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map)
{
	uint16_t *buffer = (uint16_t *)color_map;
	for (int y = area->y1; y <= area->y2; y++) {
		for (int x = area->x1; x <= area->x2; x++) {
			epd_set_pixel(x, y, (*buffer < 0x7fff) ? EPD_BLACK : EPD_WHITE);
			buffer++;
		}
	}
	lv_disp_flush_ready(drv);
}

// --- Provisioning status, shown on the fortune UI's overlay ----------------

static void SetStatus(const char *title, const char *body)
{
	if (Lvgl_lock(-1)) {
		ui_fortune_set_overlay(title, body);
		Lvgl_unlock();
	}
	Lvgl_RenderNow();
	epd_refresh_full();
}

static void OnProvisioningEvent(prov_event_t event, const char *info, void *user)
{
	(void)user;
	char body[192];
	switch (event) {
	case PROV_EVENT_STA_CONNECTING:
		snprintf(body, sizeof(body), "Connecting to\n%s", info ? info : "");
		SetStatus(FORTUNE_WIFI_LABEL, body);
		break;
	case PROV_EVENT_STA_CONNECTED:
		snprintf(body, sizeof(body), "Connected\n%s", info ? info : "");
		SetStatus(FORTUNE_WIFI_LABEL, body);
		break;
	case PROV_EVENT_PORTAL_STARTED:
		snprintf(body, sizeof(body),
		         "1. Join Wi-Fi:\n%s\n\n2. Stay connected,\nthen open the app",
		         info ? info : "");
		SetStatus(FORTUNE_WIFI_LABEL, body);
		break;
	case PROV_EVENT_CONFIG_SAVED:
		snprintf(body, sizeof(body), "Saved \"%s\"\nrestarting...", info ? info : "");
		SetStatus(FORTUNE_WIFI_LABEL, body);
		break;
	}
}

// struct tm (UTC) -> epoch seconds.
//
// This is what timegm() would do, but newlib keeps timegm() behind
// __GNU_VISIBLE and does not expose it to C++ even with -D_GNU_SOURCE. Rather
// than fight the feature-test macros, reuse saju_jdn(): it is the same
// Fliegel/Van Flandern conversion, and test_saju.c already pins
// saju_jdn(1970,1,1) == 2440588 against published values.
static time_t utc_tm_to_epoch(const struct tm *utc)
{
	long days = saju_jdn(utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday) - 2440588L;
	return (time_t)days * 86400 + utc->tm_hour * 3600 + utc->tm_min * 60 + utc->tm_sec;
}

// Seed the system clock from the battery-backed RTC (UTC) so the screen shows
// the right time — and the right 일진 — immediately, before WiFi/SNTP and
// across brief power loss.
static void SeedClockFromRtc(void)
{
	struct tm utc;
	if (board_io_rtc_get(&utc)) {
		time_t t = utc_tm_to_epoch(&utc);
		struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
		settimeofday(&tv, NULL);
		ESP_LOGI(TAG, "clock seeded from RTC");
	}
}

extern "C" void app_main(void)
{
	UserApp_AppInit();

	// Local timezone for the clock and, more importantly, for the 일진: the
	// day pillar rolls at LOCAL midnight, so the wrong TZ shows the wrong
	// fortune for up to a day.
	setenv("TZ", CONFIG_FORTUNE_TIMEZONE, 1);
	tzset();

	board_io_init();        // I2C bus + RTC + battery ADC
	SeedClockFromRtc();

	const epd_pins_t pins = {
		.sck  = EPD_SCK_PIN,
		.mosi = EPD_MOSI_PIN,
		.cs   = EPD_CS_PIN,
		.dc   = EPD_DC_PIN,
		.rst  = EPD_RST_PIN,
		.busy = EPD_BUSY_PIN,
		.power = EPD_POWER_PIN,
		.host = SPI3_HOST,
	};
	ESP_ERROR_CHECK(epd_init(&pins));
	Lvgl_PortInit(EPD_WIDTH, EPD_HEIGHT, Lvgl_FlushCallback);

	// Build the real UI up front and drive provisioning through its overlay,
	// so there is no separate status screen to allocate and throw away.
	if (Lvgl_lock(-1)) {
		UserApp_UiInit();
		Lvgl_unlock();
	}

	prov_options_t opts;
	provisioning_default_options(&opts);   // AP prefix "Ticker Board", 15s timeout
	opts.event_cb = OnProvisioningEvent;

	prov_config_t cfg;
	bool connected = provisioning_run(&opts, &cfg);  // blocks (and reboots) until configured

	if (connected) {
		ESP_LOGI(TAG, "online — location '%s'", cfg.location);
		net_time_sync(10000);   // set the clock before the first 일진 is computed
		time_t now = time(NULL);
		if (now > 1700000000) {  // sane epoch -> SNTP succeeded
			struct tm utc;
			gmtime_r(&now, &utc);
			board_io_rtc_set(&utc);
		}
		if (Lvgl_lock(-1)) {
			ui_fortune_set_overlay(NULL, NULL);   // dismiss the setup overlay
			Lvgl_unlock();
		}
		UserApp_TaskInit(&cfg);

		// Companion-app control server on the home LAN (HTTP + mDNS
		// "tickerboard.local"), reading and driving the app through the
		// user_app_api bridge.
		device_api_start();
	}
}
