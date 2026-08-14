
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <esp_timer.h>
#include <esp_log.h>

#include "epd_panel.h"
#include "lvgl_bsp.h"
#include "ui_kanji.h"
#include "ui_strings.h"
#include "user_app.h"
#include "startup_delivery.h"
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
 * costs about 1.2 MB of PSRAM for the two full-screen draw buffers and buys
 * every widget, font and anti-aliased shape working exactly as it does in the
 * desktop simulator, which renders through this same threshold.
 *
 * It does NOT refresh the panel. On e-Paper that is a multi-second flashing
 * operation and belongs to whoever knows what changed — see user_app.cpp.
 */
static void Lvgl_FlushCallback(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map)
{
	uint16_t *buffer = (uint16_t *)color_map;
	for (int y = area->y1; y <= area->y2; y++) {
		for (int x = area->x1; x <= area->x2; x++) {
			/* FULL mode always passes the base of the entire logical 648x480
			 * framebuffer, even when only a smaller invalidated area is flushed.
			 * The stride is the PANEL's width, from the driver that owns it —
			 * taking it from a UI header made the flush silently wrong the day
			 * that header stopped being about the whole screen. */
			uint16_t pixel = buffer[y * EPD_PANEL_W + x];
			epd_set_pixel(x, y, (pixel < 0x7fff) ? EPD_BLACK : EPD_WHITE);
		}
	}
	lv_disp_flush_ready(drv);
}

// --- Provisioning status, queued to the sole LVGL/panel owner (UiTask) -----

static void OnProvisioningEvent(prov_event_t event, const char *info, void *user)
{
	(void)user;
	char body[192];
	switch (event) {
	case PROV_EVENT_STA_CONNECTING:
		snprintf(body, sizeof(body), S_WIFI_CONNECTING, info ? info : "");
		if (!UserApp_SetOverlay(S_WIFI_TITLE, body)) {
			ESP_LOGE(TAG, "could not queue Wi-Fi connecting overlay");
		}
		break;
	case PROV_EVENT_STA_CONNECTED:
		snprintf(body, sizeof(body), S_WIFI_CONNECTED, info ? info : "");
		if (!UserApp_SetOverlay(S_WIFI_TITLE, body)) {
			ESP_LOGE(TAG, "could not queue Wi-Fi connected overlay");
		}
		break;
	case PROV_EVENT_PORTAL_STARTED:
		snprintf(body, sizeof(body), S_WIFI_PORTAL, info ? info : "");
		if (!UserApp_SetOverlay(S_WIFI_TITLE, body)) {
			ESP_LOGE(TAG, "could not queue Wi-Fi portal overlay");
		}
		break;
	case PROV_EVENT_CONFIG_SAVED:
		snprintf(body, sizeof(body), S_WIFI_SAVED, info ? info : "", S_RESTARTING);
		if (!UserApp_SetOverlay(S_WIFI_TITLE, body)) {
			ESP_LOGE(TAG, "could not queue Wi-Fi saved overlay");
		}
		break;
	}
}

extern "C" void app_main(void)
{
	UserApp_AppInit();

	// Keep the configured timezone for timestamped diagnostics. Nothing on the
	// glass is a clock: the board has no RTC, and every span the UI prints
	// ("9일 뒤") is worded by the proxy against the server's clock.
	setenv("TZ", CONFIG_OBSIDIAN_TIMEZONE, 1);
	tzset();

	board_io_init(BATT_ADC_PIN, BATT_ENABLE_PIN);

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
	/* The UI is laid out in the panel's native 648x480 coordinate system —
	 * see components/vault_core/ui_kanji_layout.c, which is where every
	 * rectangle on the glass comes from. */
	Lvgl_PortInit(EPD_WIDTH, EPD_HEIGHT, Lvgl_FlushCallback);

	// Build the real UI up front and drive provisioning through its overlay,
	// so there is no separate status screen to allocate and throw away.
	if (Lvgl_lock(-1)) {
		UserApp_UiInit();
		Lvgl_unlock();
	}

	prov_options_t opts;
	provisioning_default_options(&opts);   // AP prefix, 15s timeout
	opts.event_cb = OnProvisioningEvent;

	// Start the responsive catalog UI before even looking for saved Wi-Fi. A
	// station attempt may take the full timeout; studying does not wait for it.
	const int btn_gpios[] = {
		BTN_KEY0_PIN, BTN_KEY1_PIN, BTN_KEY2_PIN, BTN_BOOT_PIN,
	};
	prov_config_t cfg = {};
	UserApp_TaskInit(&cfg, btn_gpios,
	                 (int)(sizeof(btn_gpios) / sizeof(btn_gpios[0])));

	bool connected = provisioning_run(&opts, &cfg);

	if (connected) {
		startup_delivery_t startup_delivery = {};
		ESP_LOGI(TAG, "online — study URL '%s'",
		         cfg.study_url[0] ? cfg.study_url : "(none: offline catalog)");
		startup_delivery_record_network(
			&startup_delivery, UserApp_SetNetworkConfig(&cfg));
		net_time_sync(10000);
		startup_delivery_record_overlay_dismiss(
			&startup_delivery, UserApp_SetOverlay(NULL, NULL));

		// Companion-app control server on the home LAN (HTTP + mDNS
		// "obsidianboard.local"), reading and driving the app through the
		// user_app_api bridge.
		if (startup_delivery_api_eligible(&startup_delivery)) {
			device_api_start();
		} else {
			ESP_LOGE(TAG, "startup commands were not accepted; device API disabled");
		}
	} else {
		// A failed saved join emitted a connecting overlay. Returning offline
		// must reveal the catalog again; no-config boots never showed one.
		if (!UserApp_SetOverlay(NULL, NULL)) {
			ESP_LOGE(TAG, "could not queue offline overlay dismissal");
		}
		ESP_LOGI(TAG, "offline — local catalog study remains active");
	}
}
