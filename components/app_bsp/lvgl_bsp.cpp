#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "lvgl_bsp.h"

static SemaphoreHandle_t lvgl_mux = NULL;
#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))


static const char *TAG = "LvglPort";

static void Increase_lvgl_tick(void *arg)
{
  	lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

bool Lvgl_lock(int timeout_ms)
{
  	const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  	return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;       
}

void Lvgl_unlock(void)
{
  	assert(lvgl_mux && "bsp_display_start must be called first");
  	xSemaphoreGive(lvgl_mux);
}

static void Lvgl_port_task(void *arg)
{
  	uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  	for(;;)
  	{
  	  	if (Lvgl_lock(-1)) 
  	  	{
  	  	  	task_delay_ms = lv_timer_handler();
  	  	  	//Release the mutex
  	  	  	Lvgl_unlock();
  	  	}
  	  	if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS)
  	  	{
  	  	  	task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  	  	} else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS)
  	  	{
  	  	  	task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
  	  	}
  	  	vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  	}
}


void Lvgl_PortInit(int width, int height, DispFlushCb flush_cb) {
    lvgl_mux = xSemaphoreCreateMutex();
    lv_init();
    lv_display_t * disp = lv_display_create(width, height); /* 以水平和垂直分辨率（像素）进行基本初始化 */
    lv_display_set_flush_cb(disp, flush_cb);
	
	// 648x480 RGB565 is 622KB per buffer, so PSRAM is not an optimisation here —
	// it is a requirement, and there is no internal-RAM fallback that could hold
	// even one of them. (The panel is 1bpp; RGB565 is kept because the flush
	// callback binarizes, and because the desktop simulator renders through the
	// identical threshold — see docs/graphics.md.)
	//
	// The failure is spelled out rather than left to a bare assert: "no PSRAM
	// fitted" and "PSRAM not configured" are the two ways a board that looks
	// identical to a working one refuses to boot, and the difference between a
	// named error and `assert(buffer_1)` is an afternoon.
	size_t buffer_size = width * height * BYTES_PER_PIXEL;
	uint8_t *buffer_1 = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
	uint8_t *buffer_2 = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
	if (!buffer_1 || !buffer_2) {
		ESP_LOGE(TAG, "need 2 x %u B of PSRAM for the draw buffers; %u B is free",
		         (unsigned)buffer_size,
		         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
		ESP_LOGE(TAG, "this firmware needs a XIAO ESP32-S3 *Plus* (8MB octal PSRAM) "
		              "with CONFIG_SPIRAM enabled");
	}
	assert(buffer_1);
	assert(buffer_2);

    lv_display_set_buffers(disp, buffer_1, buffer_2, buffer_size, LV_DISPLAY_RENDER_MODE_FULL);

    ESP_LOGI(TAG, "Install LVGL tick timer");
  	esp_timer_create_args_t lvgl_tick_timer_args = {};
  	lvgl_tick_timer_args.callback = &Increase_lvgl_tick;
  	lvgl_tick_timer_args.name = "lvgl_tick";
    esp_timer_handle_t lvgl_tick_timer = NULL;
  	ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  	ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer,LVGL_TICK_PERIOD_MS * 1000));

    xTaskCreatePinnedToCore(Lvgl_port_task, "LVGL", 8 * 1024, NULL, 5, NULL, 0);
}

void Lvgl_RenderNow(void)
{
	// On an LCD you let the LVGL task get to the redraw whenever it does. On
	// e-Paper the caller has to know the framebuffer is complete before it
	// triggers a ~2s panel refresh, so force the redraw synchronously here.
	if (Lvgl_lock(-1)) {
		lv_refr_now(NULL);
		Lvgl_unlock();
	}
}
