/*
 * user_app.cpp — app orchestration for the saju/omikuji board.
 *
 *   AppInit:  route cJSON allocations to PSRAM.
 *   UiInit:   build the LVGL fortune UI on a fresh screen.
 *   TaskInit: spawn the UI task and the weather worker.
 *
 * WiFi bring-up, NVS and the post-connect clock sync are owned by the
 * `provisioning` component; the RTC and battery by `board_io`; the panel by
 * `port_bsp`. The portable core (omikuji, saju, weather parsing, the whole UI)
 * lives in `fortune_core` and is the same code the host tests and the desktop
 * simulator exercise.
 *
 * The structure is dictated by the display. An e-Paper refresh takes up to two
 * seconds, flashes the panel, and cannot be interleaved with another, so:
 *
 *   - Exactly one task (UiTask) ever touches LVGL or starts a refresh.
 *   - Everything else — buttons, the HTTP API, the weather worker — posts a
 *     command and returns.
 *   - Drawing and presenting are separate: widgets are updated, then the frame
 *     is rendered synchronously, then one refresh is issued for the whole
 *     change. Never one refresh per widget.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_system.h>   /* esp_restart for the force-AP escape hatch */
#include <esp_timer.h>

#include "sdkconfig.h"
#include "cJSON.h"

#include "user_app.h"
#include "user_app_api.h"
#include "lvgl_bsp.h"          /* Lvgl_lock / Lvgl_unlock / Lvgl_RenderNow */
#include "epd_panel.h"         /* epd_refresh_* / epd_selftest            */
#include "ui_fortune.h"
#include "omikuji.h"
#include "saju.h"
#include "http_port.h"         /* http_port_init: TLS-connect gate setup  */
#include "weather_service.h"   /* Open-Meteo geocoding + forecast         */
#include "buttons.h"
#include "board_io.h"          /* RTC + battery                           */
#include "prov_store.h"        /* persist the location to NVS             */

static const char *TAG = "app";
static lv_obj_t   *s_screen;

/* The provisioned config, copied so it outlives app_main's stack. */
static prov_config_t s_cfg;

/* --- cadences ------------------------------------------------------------ */

/* How often the UI task wakes to refresh the clock and battery. On the old
 * reflective LCD this was 15s. On e-Paper each tick costs a partial refresh
 * (visible, and ghosting-accumulating), and the clock only shows HH:MM, so a
 * minute is both the useful resolution and the panel-friendly one. */
#define HOME_TICK_SECONDS  60

/* Floor on how often a partial refresh may run, whatever asks for it. Weather,
 * battery and the clock all land on the same tick, and without this a burst of
 * updates would strobe the panel. */
#define MIN_PARTIAL_MS     (55 * 1000)

#define WEATHER_REFRESH_SECONDS 1800   /* 30 min — outdoor weather drifts slowly */

/* USER+BOOT together: a tap is ignored, a 5s hold forces Wi-Fi setup mode — the
 * escape hatch when the board is stuck on a network the user can't reach. */
#define CHORD_POLL_MS     15
#define CHORD_WINDOW_MS  120
#define FORCE_AP_HOLD_MS 5000

/* --- commands ------------------------------------------------------------ */

typedef enum {
    APP_CMD_DRAW,            /* new fortune                          */
    APP_CMD_SET_PAGE,        /* ival = ui_page_t                     */
    APP_CMD_SET_LOCATION,    /* text = free-text place               */
    APP_CMD_DISPLAY_TEST,    /* run epd_selftest()                   */
    APP_CMD_WEATHER,         /* WeatherTask published a new forecast */
} app_cmd_kind_t;

typedef struct {
    app_cmd_kind_t kind;
    int  ival;
    char text[PROV_LOCATION_MAX_LEN + 1];
} app_cmd_t;

static QueueHandle_t     s_btn_queue;
static QueueHandle_t     s_cmd_queue;
static QueueSetHandle_t  s_queue_set;
static SemaphoreHandle_t s_mtx;
static SemaphoreHandle_t s_weather_wake;

static inline void state_lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static inline void state_unlock(void) { xSemaphoreGive(s_mtx); }

/* --- state (guarded by s_mtx unless noted) -------------------------------- */

static bool             s_fortune_valid;
static omikuji_result_t s_fortune;
static saju_iljin_t     s_iljin;
static bool             s_day_valid;
static manse_day_t      s_day;              /* date + pillars for the 만세력 page */
static long             s_iljin_jdn;        /* which day s_iljin/s_fortune are for */
static int              s_page;

static bool     s_wx_valid;
static wx_kind_t s_wx_kind;
static int      s_wx_temp_c;
static char     s_wx_city[64];
static wx_day_t s_wx_days[WX_FORECAST_MAX];
static int      s_wx_day_count;

static bool  s_batt_valid;
static int   s_batt_pct;
static int   s_batt_mv;

/* Only ever touched by UiTask. */
static int64_t s_last_partial_us;

/* cJSON's parse tree for the Open-Meteo forecast is modest, but keeping it out
 * of internal RAM leaves that for WiFi/TLS. */
static void *psram_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);      /* boards without PSRAM still work */
}

void UserApp_AppInit(void)
{
    cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);
}

void UserApp_UiInit(void)
{
    /* Swap the provisioning status screen for the fortune UI, freeing the old
     * one (and its widgets) instead of leaking it for the process lifetime. */
    lv_obj_t *prev = lv_screen_active();
    s_screen = lv_obj_create(NULL);
    lv_screen_load(s_screen);
    if (prev && prev != s_screen) {
        lv_obj_delete(prev);
    }
    ui_fortune_create(s_screen);
}

/* --- presenting (UiTask only) --------------------------------------------- */

/* Render whatever the setters changed, then push one refresh for the lot.
 *
 * `full` is not a hint: it is the difference between a 2s flashing update that
 * clears ghosting and a 0.3s silent one that adds to it. Use full whenever the
 * whole screen changed (a new fortune, a page switch) and partial for the
 * clock/battery/weather nudges. epd_refresh_partial() promotes itself to a full
 * refresh every EPD_PARTIAL_CHAIN_MAX calls, so ghosting still gets cleared
 * without anyone tracking it here. */
static void present(bool full)
{
    Lvgl_RenderNow();
    if (full) {
        epd_refresh_full();
    } else {
        epd_refresh_partial();
    }
    s_last_partial_us = esp_timer_get_time();
}

/* A partial refresh that respects the floor. Returns whether it ran. */
static bool present_partial_throttled(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_partial_us != 0 && now - s_last_partial_us < (int64_t)MIN_PARTIAL_MS * 1000) {
        return false;
    }
    present(false);
    return true;
}

/* --- content updates (UiTask only) ---------------------------------------- */

static void push_fortune_to_ui(void)
{
    state_lock();
    bool have = s_fortune_valid;
    omikuji_result_t f = s_fortune;
    bool have_day = s_day_valid;
    manse_day_t day = s_day;
    saju_iljin_t ij = s_iljin;
    state_unlock();

    if (Lvgl_lock(-1)) {
        ui_fortune_set_omikuji(have ? &f : NULL);
        ui_fortune_set_day(have_day ? &day : NULL);
        ui_fortune_set_iljin(&ij);
        Lvgl_unlock();
    }
}

static void push_weather_to_ui(void)
{
    state_lock();
    bool valid = s_wx_valid;
    wx_kind_t kind = s_wx_kind;
    int temp = s_wx_temp_c;
    char city[sizeof(s_wx_city)];
    strlcpy(city, s_wx_city, sizeof(city));
    wx_day_t days[WX_FORECAST_MAX];
    memcpy(days, s_wx_days, sizeof(days));
    int n = s_wx_day_count;
    state_unlock();

    if (Lvgl_lock(-1)) {
        ui_fortune_set_weather(valid, kind, temp, city);
        ui_fortune_set_forecast(days, n);
        Lvgl_unlock();
    }
}

static void read_battery(void)
{
    float v = board_io_battery_voltage();
    int pct = board_io_battery_percent();
    bool ok = v > 0.1f;

    state_lock();
    s_batt_valid = ok;
    s_batt_mv    = (int)(v * 1000.0f + 0.5f);
    s_batt_pct   = pct;
    state_unlock();

    if (Lvgl_lock(-1)) {
        ui_fortune_set_battery(ok, pct);
        Lvgl_unlock();
    }
}

/* Recompute the day's 일진 and draw a fresh fortune. Called at boot and again
 * whenever the local date rolls over, so the board is a new slip each morning
 * rather than a stale one from whenever it was last powered on. Returns true if
 * anything changed. */
static bool roll_day(bool force)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    long jdn = saju_jdn(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);

    state_lock();
    bool changed = force || (jdn != s_iljin_jdn);
    state_unlock();
    if (!changed) {
        return false;
    }

    /* Everything on the 만세력 page that follows from the date, computed once
     * here so the UI stays a pure view. */
    manse_day_t day;
    day.year  = lt.tm_year + 1900;
    day.month = lt.tm_mon + 1;
    day.day   = lt.tm_mday;
    day.wday  = lt.tm_wday;
    saju_iljin_for_date(day.year, day.month, day.day, &day.iljin);
    saju_yearju_for_date(day.year, day.month, day.day, &day.yearju);

    omikuji_result_t f;
    omikuji_draw(esp_random, &f);

    state_lock();
    s_iljin = day.iljin;
    s_day = day;
    s_day_valid = true;
    s_iljin_jdn = jdn;
    s_fortune = f;
    s_fortune_valid = true;
    state_unlock();

    ESP_LOGI(TAG, "%04d-%02d-%02d  일진 %s (%s)  운세 %s (%s)",
             day.year, day.month, day.day,
             day.iljin.hanja, day.iljin.hangul, f.hanja, f.hangul);
    return true;
}

/* --- actions -------------------------------------------------------------- */

static void action_draw(void)
{
    omikuji_result_t f;
    omikuji_draw(esp_random, &f);

    state_lock();
    s_fortune = f;
    s_fortune_valid = true;
    state_unlock();

    ESP_LOGI(TAG, "draw -> %s (%s)", f.hanja, f.hangul);
    push_fortune_to_ui();

    if (Lvgl_lock(-1)) {
        ui_fortune_show_page(UI_PAGE_OMIKUJI);
        Lvgl_unlock();
    }
    state_lock();
    s_page = UI_PAGE_OMIKUJI;
    state_unlock();

    present(true);   /* the whole screen changed */
}

static void action_set_page(int page)
{
    if (page < 0 || page >= UI_PAGE_COUNT) {
        return;
    }
    state_lock();
    s_page = page;
    state_unlock();

    if (Lvgl_lock(-1)) {
        ui_fortune_show_page((ui_page_t)page);
        Lvgl_unlock();
    }
    present(true);   /* a page swap replaces every pixel */
}

static void action_set_location(const char *place)
{
    state_lock();
    strlcpy(s_cfg.location, place, sizeof(s_cfg.location));
    state_unlock();

    if (!prov_store_save(&s_cfg)) {
        ESP_LOGW(TAG, "location change: NVS save failed (will not survive reboot)");
    }
    if (s_weather_wake) {
        xSemaphoreGive(s_weather_wake);
    }
    ESP_LOGI(TAG, "weather location set to '%s'", place);
}

static void action_display_test(void)
{
    ESP_LOGI(TAG, "e-Paper self-test starting (~10s)");
    epd_selftest();
    /* The self-test drew straight into the framebuffer behind LVGL's back, so
     * force a full redraw rather than leaving the panel white. */
    if (Lvgl_lock(-1)) {
        lv_obj_invalidate(lv_screen_active());
        Lvgl_unlock();
    }
    present(true);
    ESP_LOGI(TAG, "e-Paper self-test done");
}

/* USER+BOOT held FORCE_AP_HOLD_MS: set the one-shot force-portal flag, show a
 * confirmation, and reboot into Wi-Fi setup. The saved config is kept so the
 * portal pre-fills. Does not return. */
static void force_ap_mode(void)
{
    ESP_LOGW(TAG, "USER+BOOT long-press -> forcing Wi-Fi setup (AP) mode");
    prov_store_set_force_portal();
    if (Lvgl_lock(-1)) {
        ui_fortune_set_overlay(FORTUNE_WIFI_LABEL, "restarting...");
        Lvgl_unlock();
    }
    present(true);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void handle_cmd(const app_cmd_t *c)
{
    switch (c->kind) {
    case APP_CMD_DRAW:         action_draw(); break;
    case APP_CMD_SET_PAGE:     action_set_page(c->ival); break;
    case APP_CMD_SET_LOCATION: action_set_location(c->text); break;
    case APP_CMD_DISPLAY_TEST: action_display_test(); break;
    case APP_CMD_WEATHER:
        push_weather_to_ui();
        present_partial_throttled();
        break;
    }
}

static void handle_press(button_id_t id)
{
    if (id == BUTTON_USER) {
        ESP_LOGI(TAG, "USER -> new fortune");
        action_draw();
    } else {
        state_lock();
        int page = (s_page + 1) % UI_PAGE_COUNT;
        state_unlock();
        ESP_LOGI(TAG, "BOOT -> page %d", page);
        action_set_page(page);
    }
}

/*
 * UiTask — the only task that touches LVGL or the panel. Blocks on buttons OR
 * app commands, and wakes every HOME_TICK_SECONDS to keep the clock, battery
 * and date current.
 */
static void UiTask(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "controls: USER = 새로 뽑기, BOOT = 페이지 전환, both 5s = Wi-Fi setup");

    roll_day(true);                 /* first fortune of this boot */
    read_battery();
    push_fortune_to_ui();
    push_weather_to_ui();
    if (Lvgl_lock(-1)) {
        ui_fortune_tick();
        ui_fortune_show_page(UI_PAGE_OMIKUJI);
        Lvgl_unlock();
    }
    present(true);

    for (;;) {
        QueueSetMemberHandle_t member =
            xQueueSelectFromSet(s_queue_set, pdMS_TO_TICKS(HOME_TICK_SECONDS * 1000));

        if (member == s_btn_queue) {
            button_event_t ev;
            if (xQueueReceive(s_btn_queue, &ev, 0) != pdTRUE) {
                continue;           /* spurious wake (see the chord drain below) */
            }
            /* Poll the pins for a chord, stopping early once seen; tolerates a
             * slightly-late second finger without delaying a true single press
             * by more than CHORD_WINDOW_MS. */
            bool chord = buttons_both_pressed();
            for (int w = 0; !chord && w < CHORD_WINDOW_MS; w += CHORD_POLL_MS) {
                vTaskDelay(pdMS_TO_TICKS(CHORD_POLL_MS));
                chord = buttons_both_pressed();
            }
            if (chord) {
                button_event_t drop;
                while (xQueueReceive(s_btn_queue, &drop, 0) == pdTRUE) { }
                int held_ms = 0;
                while (buttons_both_pressed() && held_ms < FORCE_AP_HOLD_MS) {
                    vTaskDelay(pdMS_TO_TICKS(CHORD_POLL_MS));
                    held_ms += CHORD_POLL_MS;
                }
                if (held_ms >= FORCE_AP_HOLD_MS) {
                    force_ap_mode();   /* reboots — does not return */
                }
                /* A short chord does nothing: on this device both single
                 * presses are already meaningful and there is no third view. */
            } else {
                handle_press(ev.id);
            }
        } else if (member == s_cmd_queue) {
            app_cmd_t cmd;
            if (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
                handle_cmd(&cmd);
            }
        } else {
            /* Idle tick. */
            if (roll_day(false)) {          /* crossed local midnight */
                push_fortune_to_ui();
                read_battery();
                if (Lvgl_lock(-1)) { ui_fortune_tick(); Lvgl_unlock(); }
                present(true);              /* new day, new slip */
                continue;
            }
            read_battery();
            if (Lvgl_lock(-1)) { ui_fortune_tick(); Lvgl_unlock(); }
            present_partial_throttled();
        }
    }
}

/*
 * WeatherTask — geocodes the provisioned location once (Open-Meteo, keyless),
 * then refreshes current conditions + the forecast on a slow cadence. Publishes
 * into the shared state and pokes UiTask; it never touches LVGL or the panel
 * itself, so a TLS stall can't hold up a refresh. Parks when no location is set.
 */
#ifndef CONFIG_FORTUNE_LOCATION
#define CONFIG_FORTUNE_LOCATION ""
#endif

static geo_loc_t s_geo;   /* resolved location (only WeatherTask touches it) */

static void notify_ui(app_cmd_kind_t kind)
{
    if (!s_cmd_queue) {
        return;
    }
    app_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.kind = kind;
    xQueueSend(s_cmd_queue, &c, 0);
}

static void WeatherTask(void *arg)
{
    (void)arg;
    /* Let the WiFi/TLS stack settle before the first handshake. */
    vTaskDelay(pdMS_TO_TICKS(4000));

    /* Outer loop = one "configured location" epoch. Re-entered whenever the app
     * changes the location, so the widget tracks live edits without a reboot. */
    for (;;) {
        char place[PROV_LOCATION_MAX_LEN + 1];
        state_lock();
        strlcpy(place, s_cfg.location[0] ? s_cfg.location : CONFIG_FORTUNE_LOCATION, sizeof(place));
        state_unlock();

        if (!place[0]) {
            ESP_LOGI(TAG, "weather: no location set -> block hidden");
            state_lock();
            s_wx_valid = false;
            s_wx_city[0] = '\0';
            s_wx_day_count = 0;
            state_unlock();
            notify_ui(APP_CMD_WEATHER);
            xSemaphoreTake(s_weather_wake, portMAX_DELAY);
            continue;
        }

        /* Geocode, retrying for the network to come up. A location change during
         * the wait breaks out early to re-snapshot the new place. */
        bool geo_ok = false;
        for (int tries = 0; tries < 6 && !geo_ok; tries++) {
            geo_ok = weather_service_geocode(place, &s_geo);
            if (!geo_ok && xSemaphoreTake(s_weather_wake, pdMS_TO_TICKS(10000)) == pdTRUE) break;
        }
        if (!geo_ok) {
            ESP_LOGW(TAG, "weather: geocode failed for '%s'", place);
            continue;
        }

        char city[64];
        if (s_geo.country[0]) snprintf(city, sizeof city, "%s, %s", s_geo.name, s_geo.country);
        else                  snprintf(city, sizeof city, "%s", s_geo.name);
        ESP_LOGI(TAG, "weather: '%s' -> %s (%.3f, %.3f)", place, city, s_geo.lat, s_geo.lon);

        bool reconfig = false;
        while (!reconfig) {
            weather_t w;
            if (weather_service_fetch(s_geo.lat, s_geo.lon, &w) && w.valid) {
                int n = w.day_count;
                if (n > WX_FORECAST_MAX) n = WX_FORECAST_MAX;

                state_lock();
                s_wx_valid  = w.now_valid;
                s_wx_kind   = w.now_wx;
                s_wx_temp_c = w.now_temp_c;
                strlcpy(s_wx_city, city, sizeof(s_wx_city));
                memcpy(s_wx_days, w.days, sizeof(s_wx_days));
                s_wx_day_count = n;
                state_unlock();

                notify_ui(APP_CMD_WEATHER);
                ESP_LOGI(TAG, "weather: %d C now, %d-day forecast", w.now_temp_c, n);
            } else {
                ESP_LOGW(TAG, "weather: fetch failed");
            }
            if (xSemaphoreTake(s_weather_wake, pdMS_TO_TICKS(WEATHER_REFRESH_SECONDS * 1000)) == pdTRUE) {
                reconfig = true;
            }
        }
    }
}

void UserApp_TaskInit(const prov_config_t *cfg)
{
    s_cfg = *cfg;

    s_mtx          = xSemaphoreCreateMutex();
    s_weather_wake = xSemaphoreCreateBinary();
    s_btn_queue    = xQueueCreate(16, sizeof(button_event_t));
    s_cmd_queue    = xQueueCreate(8, sizeof(app_cmd_t));
    /* A queue set lets UiTask block on buttons OR app commands in one wait.
     * Both queues must be empty when added, so build the set before
     * buttons_init starts posting. */
    s_queue_set = xQueueCreateSet(16 + 8);
    xQueueAddToSet(s_btn_queue, s_queue_set);
    xQueueAddToSet(s_cmd_queue, s_queue_set);
    buttons_init(s_btn_queue);

    /* Create the global TLS-connect gate before any task can call http_get(). */
    http_port_init();

    /* UiTask does no networking, so it needs only a modest stack — the LVGL
     * render itself runs on the LVGL task. Higher priority so a button press is
     * handled the instant it arrives. */
    xTaskCreatePinnedToCore(UiTask, "ui", 8 * 1024, NULL, 4, NULL, 1);

    /* WeatherTask: the JSON is tiny, but the mbedTLS handshake and cert-bundle
     * validation run on THIS task's stack (esp_http_client is synchronous) and
     * are heavy — 16KB is the size that proved stable for the TLS tasks in the
     * previous firmware. Low priority; the UI never waits on it. */
    xTaskCreatePinnedToCore(WeatherTask, "weather", 16 * 1024, NULL, 2, NULL, 1);
}

/* ===========================================================================
 * Companion-app control bridge (declared in user_app_api.h). These run on the
 * HTTP server task: the read copies state under s_mtx; the writes post a
 * command for UiTask to apply via the same paths as a button press. All are
 * safe no-ops until UserApp_TaskInit has created the queues.
 * =========================================================================== */

static bool post_cmd(app_cmd_kind_t kind, int ival, const char *text)
{
    if (!s_cmd_queue) {
        return false;
    }
    app_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.kind = kind;
    c.ival = ival;
    if (text) {
        strlcpy(c.text, text, sizeof(c.text));
    }
    return xQueueSend(s_cmd_queue, &c, 0) == pdTRUE;
}

void user_app_snapshot(device_state_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->model, DEVICE_MODEL, sizeof(out->model));
    strlcpy(out->fw, DEVICE_FW, sizeof(out->fw));
    out->partial_chain = epd_partial_chain();
    if (!s_mtx) {
        return;                     /* TaskInit has not run yet */
    }

    state_lock();
    out->page = s_page;

    out->fortune_valid = s_fortune_valid;
    if (s_fortune_valid) {
        out->rank = (int)s_fortune.rank;
        strlcpy(out->rank_hanja, s_fortune.hanja, sizeof(out->rank_hanja));
        strlcpy(out->rank_hangul, s_fortune.hangul, sizeof(out->rank_hangul));
        strlcpy(out->message, s_fortune.message, sizeof(out->message));
    }

    out->iljin_index = s_iljin.sexagenary;
    strlcpy(out->iljin_hanja, s_iljin.hanja, sizeof(out->iljin_hanja));
    strlcpy(out->iljin_hangul, s_iljin.hangul, sizeof(out->iljin_hangul));

    strlcpy(out->location, s_cfg.location, sizeof(out->location));
    out->wx_valid  = s_wx_valid;
    out->wx_kind   = (int)s_wx_kind;
    out->wx_temp_c = s_wx_temp_c;
    strlcpy(out->city, s_wx_city, sizeof(out->city));
    int n = s_wx_day_count;
    if (n > DEV_FORECAST_MAX) n = DEV_FORECAST_MAX;
    out->forecast_count = n;
    for (int i = 0; i < n; i++) {
        strlcpy(out->forecast[i].dow, s_wx_days[i].dow, sizeof(out->forecast[i].dow));
        out->forecast[i].wx = (int)s_wx_days[i].wx;
        out->forecast[i].lo = s_wx_days[i].lo;
        out->forecast[i].hi = s_wx_days[i].hi;
    }

    out->battery_valid = s_batt_valid;
    out->battery_pct   = s_batt_pct;
    out->battery_mv    = s_batt_mv;
    state_unlock();
}

bool user_app_draw_fortune(void)
{
    return post_cmd(APP_CMD_DRAW, 0, NULL);
}

bool user_app_set_page(int page)
{
    if (page < 0 || page >= UI_PAGE_COUNT) {
        return false;
    }
    return post_cmd(APP_CMD_SET_PAGE, page, NULL);
}

bool user_app_set_location(const char *place)
{
    if (!place || strlen(place) > PROV_LOCATION_MAX_LEN) {
        return false;
    }
    return post_cmd(APP_CMD_SET_LOCATION, 0, place);
}

bool user_app_display_test(void)
{
    return post_cmd(APP_CMD_DISPLAY_TEST, 0, NULL);
}
