/*
 * user_app.cpp — app orchestration for the kanji study board.
 *
 *   AppInit:  route cJSON allocations to PSRAM.
 *   UiInit:   build the LVGL study UI on a fresh screen.
 *   TaskInit: spawn the UI task and the card poller.
 *
 * Wi-Fi bring-up, NVS and the post-connect clock sync are owned by the
 * `provisioning` component; the battery by `board_io`; the panel by `port_bsp`.
 * The portable core (the model, the parser, the nav state machine, the whole
 * UI) lives in `vault_core` and is the same code the host tests and the desktop
 * simulator exercise.
 *
 * The structure is dictated by the display. A full refresh of this 648x480
 * panel takes seconds, flashes, and cannot be interleaved with another, so:
 *
 *   - Exactly one task (UiTask) ever touches LVGL or starts a refresh.
 *   - Everything else — buttons, the HTTP API, the card poller — posts a
 *     command and returns.
 *   - Drawing and presenting are separate: widgets are updated, then the frame
 *     is rendered synchronously, then ONE refresh is issued for the whole
 *     change. Never one refresh per widget.
 *
 * Two rules this board adds on top:
 *
 *   - A poll that returns the same content does not touch the panel at all.
 *     Every five minutes, forever, on a device that mostly sits still, that is
 *     the difference between a silent board and one that flashes at nobody.
 *   - Moving the grade cursor refreshes ONLY the dock. Choosing among four
 *     ratings takes up to three presses, and three full refreshes is nine
 *     seconds of strobing before the learner has told the board anything.
 *
 * Grading itself never runs on UiTask. It is an HTTP round trip to a laptop
 * that may be asleep, and UiTask owning the panel means a stalled request would
 * freeze every button on the board — so KEY1 hands the rating to KanjiTask and
 * returns immediately.
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
#include <esp_system.h>   /* esp_restart for the force-AP escape hatch */
#include <esp_timer.h>

#include "sdkconfig.h"
#include "cJSON.h"

#include "user_app.h"
#include "user_app_api.h"
#include "source_guard.h"
#include "study_source.h"
#include "lvgl_bsp.h"          /* Lvgl_lock / Lvgl_unlock / Lvgl_RenderNow */
#include "epd_panel.h"         /* epd_refresh_* / epd_selftest            */
#include "kanji_mock.h"
#include "kanji_model.h"
#include "kanji_nav.h"
#include "kanji_service.h"
#include "ui_kanji.h"
#include "ui_strings.h"
#include "http_port.h"
#include "buttons.h"
#include "board_io.h"
#include "prov_store.h"
#include "prov_config.h"
#include "catalog_store.h"

static const char *TAG = "app";
static lv_obj_t   *s_screen;
static bool        s_overlay_visible;

/* The provisioned config, copied so it outlives app_main's stack. */
static prov_config_t s_cfg;

/* --- cadences ------------------------------------------------------------ */

#ifndef CONFIG_OBSIDIAN_POLL_SECONDS
#define CONFIG_OBSIDIAN_POLL_SECONDS 300
#endif
#define POLL_SECONDS       CONFIG_OBSIDIAN_POLL_SECONDS

/* A minute wake keeps battery telemetry current. Nothing on the glass changes
 * with the clock — the board has no RTC and every span is worded by the proxy —
 * so an idle wake never redraws the panel. */
#define TICK_SECONDS       60

/* The companion API reports a card stale after this many poll intervals. Two
 * rather than one: one missed poll is usually a laptop closing its lid. */
#define STALE_AFTER_POLLS    2

/* KEY2 held this long forces Wi-Fi setup mode — the escape hatch when the board
 * is stuck on a network the user can no longer reach. */
#define HOLD_POLL_MS       25
#define FORCE_AP_HOLD_MS 5000

/* --- commands ------------------------------------------------------------ */

typedef enum {
    APP_CMD_SET_SCREEN,      /* ival = kanji_screen_t, from the companion app */
    APP_CMD_REFRESH_NOW,     /* poll immediately                              */
    APP_CMD_SET_URL,         /* text = the new study URL                      */
    APP_CMD_DISPLAY_TEST,    /* run epd_selftest()                            */
    APP_CMD_SET_NETWORK_CONFIG,
    APP_CMD_SET_OVERLAY,
    APP_CMD_DATA,            /* KanjiTask published a card that changed       */
    APP_CMD_CARD_ADVANCED,   /* KanjiTask graded, and the NEXT card is up     */
} app_cmd_kind_t;

typedef struct {
    app_cmd_kind_t kind;
    int  ival;
    uint32_t source_generation;
    char text[PROV_URL_MAX_LEN + 1];
    char overlay_title[64];
    char overlay_body[192];
    prov_config_t network_config;
} app_cmd_t;

static QueueHandle_t     s_btn_queue;
static QueueHandle_t     s_cmd_queue;
static QueueSetHandle_t  s_queue_set;
static SemaphoreHandle_t s_mtx;
static SemaphoreHandle_t s_catalog_mtx;
static SemaphoreHandle_t s_poll_wake;

static inline void state_lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static inline void state_unlock(void) { xSemaphoreGive(s_mtx); }

/* --- state (guarded by s_mtx unless noted) -------------------------------- */

static kanji_t  s_data;                 /* what is on (or going to) the glass */
static uint32_t s_hash;
static uint16_t s_catalog_ordinal;
/* Invalidates any synchronous HTTP fetch that started before the source changed. */
static source_guard_t s_source_guard;

/* The interaction state. Owned by UiTask, but read by the companion API, so it
 * lives under the same lock as everything else it reports beside. */
static kanji_nav_t s_nav;

/* One captured request remains occupied until its local write or HTTP request
 * completes. Its source, grade, local ordinal, and remote id are one atomic
 * snapshot, so a later source transition cannot reroute the answer. */
static study_grade_request_t s_pending_grade;
static bool s_pending_grade_valid;
static uint32_t s_pending_grade_generation;

static kanji_fetch_result_t s_last_result = KANJI_FETCH_NO_URL;
static int64_t  s_last_ok_us;           /* 0 = never fetched successfully     */

static bool     s_batt_present;
static int      s_batt_pct;
static int      s_batt_mv;

/* cJSON's parse tree for a card is a few KB, but keeping it out of internal RAM
 * leaves that for WiFi/TLS and the panel's DMA framebuffer. */
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
    /* Swap the provisioning status screen for the study UI, freeing the old one
     * (and its widgets) instead of leaking it for the process lifetime. */
    lv_obj_t *prev = lv_screen_active();
    s_screen = lv_obj_create(NULL);
    lv_screen_load(s_screen);
    if (prev && prev != s_screen) {
        lv_obj_delete(prev);
    }
    ui_kanji_create(s_screen);
}

/* --- presenting (UiTask only) --------------------------------------------- */

/* Render whatever the setters changed, then push one refresh for the lot. */
static void present_full(void)
{
    Lvgl_RenderNow();
    epd_refresh_full();
}

/* The grade cursor moved and nothing else did. The window comes from the layout
 * rather than from a constant here, so it cannot drift from what was drawn; the
 * driver promotes the chain back to a full refresh every sixth partial on its
 * own, so ghosting is not this file's problem. */
static void present_dock(void)
{
    int x1, y1, x2, y2;
    ui_kanji_dock_area(&x1, &y1, &x2, &y2);
    Lvgl_RenderNow();
    epd_refresh_partial_area(x1, y1, x2, y2);
}

/* --- content updates (UiTask only) ---------------------------------------- */

/* The snapshot is copied out from under the mutex so LVGL is never touched
 * while holding it. The copy is static rather than automatic because kanji_t is
 * a couple of KB and this frame goes on to call into LVGL, whose render runs on
 * this same task. A static is safe here precisely because of the rule the whole
 * file is built on: UiTask is the only caller. */
static kanji_t s_ui_copy;

static void push_data_to_ui(void)
{
    kanji_nav_t nav;
    state_lock();
    s_ui_copy = s_data;
    nav = s_nav;
    state_unlock();

    if (Lvgl_lock(-1)) {
        ui_kanji_set_data(&s_ui_copy);
        ui_kanji_set_nav(&nav);
        Lvgl_unlock();
    }
}

static void push_status_to_ui(void)
{
    ui_status_t st;
    state_lock();
    st.online = s_data.source == KANJI_SOURCE_DEMO ||
                (s_data.source == KANJI_SOURCE_REMOTE &&
                 s_last_result == KANJI_FETCH_OK);
    if (s_data.source != KANJI_SOURCE_REMOTE) {
        st.stale = false;
    } else if (s_last_ok_us != 0) {
        const int age = (int)((esp_timer_get_time() - s_last_ok_us) / 1000000);
        st.stale = age > POLL_SECONDS * STALE_AFTER_POLLS;
    } else {
        st.stale = s_cfg.study_url[0] != '\0';
    }
    st.battery_present = s_batt_present;
    st.battery_pct     = s_batt_pct;
    state_unlock();

    if (Lvgl_lock(-1)) {
        ui_kanji_set_status(&st);
        Lvgl_unlock();
    }
}

static void read_battery(void)
{
    float v = board_io_battery_voltage();
    state_lock();
    s_batt_present = board_io_battery_present();
    s_batt_mv      = (int)(v * 1000.0f + 0.5f);
    s_batt_pct     = board_io_battery_percent();
    state_unlock();
}

/* Restore the store's published snapshot, or use the built-in card only when
 * no valid catalog exists. The store lock serializes its pointer swap with a
 * local grade; the state lock makes the copied card/hash/nav one publication. */
static bool restore_catalog_or_demo(void)
{
    bool restored = false;
    xSemaphoreTake(s_catalog_mtx, portMAX_DELAY);
    const kanji_t *catalog = catalog_store_available()
                                 ? catalog_store_current()
                                 : NULL;
    const uint16_t ordinal = catalog != NULL ? catalog_store_ordinal() : 0;
    state_lock();
    if (catalog != NULL && catalog->valid) {
        s_data = *catalog;
        s_catalog_ordinal = ordinal;
        restored = true;
    } else {
        kanji_mock(&s_data);
        s_catalog_ordinal = 0;
    }
    s_hash = kanji_hash(&s_data);
    kanji_nav_reset(&s_nav);
    s_last_ok_us = 0;
    s_last_result = KANJI_FETCH_NO_URL;
    state_unlock();
    xSemaphoreGive(s_catalog_mtx);
    return restored;
}

/* --- actions -------------------------------------------------------------- */

/* The companion app can put the board on any screen the buttons can reach — and
 * only those. It goes through kanji_nav_set_screen() rather than setting the
 * fields here, so the phone and the buttons cannot disagree about which screens
 * exist for this card; a request for one that does not is dropped, and the
 * board stays where it was. */
static void action_set_screen(int screen)
{
    state_lock();
    const kanji_nav_t before = s_nav;
    const bool accepted =
        kanji_nav_set_screen(&s_nav, (kanji_screen_t)screen, &s_data);
    const bool changed = accepted &&
                         (before.sheet != s_nav.sheet ||
                          before.revealed != s_nav.revealed ||
                          before.sheet_page != s_nav.sheet_page);
    state_unlock();

    if (!accepted) {
        ESP_LOGW(TAG, "screen %d has nothing to show for this card; ignored",
                 screen);
        return;
    }
    if (changed) {
        push_data_to_ui();
        present_full();
    }
}

static void action_set_url(const char *url)
{
    state_lock();
    const bool changed = strcmp(s_cfg.study_url, url) != 0;
    strlcpy(s_cfg.study_url, url, sizeof(s_cfg.study_url));
    if (changed) {
        source_guard_advance(&s_source_guard);
    }
    state_unlock();

    if (!prov_store_save(&s_cfg)) {
        ESP_LOGW(TAG, "study URL change: NVS save failed (will not survive reboot)");
    }
    const bool to_catalog = url[0] == '\0';
    ESP_LOGI(TAG, "study URL set to '%s'%s", url,
             to_catalog ? " (offline catalog)" : "");

    if (to_catalog) {
        const bool restored = restore_catalog_or_demo();
        ESP_LOGI(TAG, "%s restored after URL clear",
                 restored ? "offline catalog" : "demo fallback");
        push_status_to_ui();
        push_data_to_ui();
        present_full();
    }
    if (s_poll_wake) {
        xSemaphoreGive(s_poll_wake);
    }
}

static void action_set_network_config(const prov_config_t *cfg)
{
    state_lock();
    const bool source_changed = strcmp(s_cfg.study_url, cfg->study_url) != 0;
    s_cfg = *cfg;
    if (source_changed) {
        source_guard_advance(&s_source_guard);
    }
    state_unlock();

    if (source_changed && cfg->study_url[0] == '\0') {
        (void)restore_catalog_or_demo();
        push_status_to_ui();
        push_data_to_ui();
        present_full();
    }
    if (s_poll_wake) {
        xSemaphoreGive(s_poll_wake);
    }
}

static void action_set_overlay(const char *title, const char *body)
{
    const bool visible = title[0] != '\0';
    if (!visible && !s_overlay_visible) {
        return;
    }
    if (Lvgl_lock(-1)) {
        ui_kanji_set_overlay(visible ? title : NULL, body);
        Lvgl_unlock();
    }
    s_overlay_visible = visible;
    present_full();
}

static void action_display_test(void)
{
    ESP_LOGI(TAG, "e-Paper self-test starting");
    epd_selftest();
    /* The self-test drew straight into the framebuffer behind LVGL's back, so
     * force a full redraw rather than leaving the panel white. */
    if (Lvgl_lock(-1)) {
        lv_obj_invalidate(lv_screen_active());
        Lvgl_unlock();
    }
    present_full();
    ESP_LOGI(TAG, "e-Paper self-test done (full %dms, partial %dms)",
             epd_last_full_ms(), epd_last_partial_ms());
}

/* KEY2 held FORCE_AP_HOLD_MS: set the one-shot force-portal flag, show a
 * confirmation, and reboot into Wi-Fi setup. The saved config is kept so the
 * portal pre-fills. Does not return. */
static void force_ap_mode(void)
{
    ESP_LOGW(TAG, "KEY2 long-press -> forcing Wi-Fi setup (AP) mode");
    prov_store_set_force_portal();
    if (Lvgl_lock(-1)) {
        ui_kanji_set_overlay(S_WIFI_TITLE, S_RESTARTING);
        Lvgl_unlock();
    }
    present_full();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void handle_cmd(const app_cmd_t *c)
{
    switch (c->kind) {
    case APP_CMD_SET_SCREEN:   action_set_screen(c->ival); break;
    case APP_CMD_SET_URL:      action_set_url(c->text); break;
    case APP_CMD_DISPLAY_TEST: action_display_test(); break;
    case APP_CMD_SET_NETWORK_CONFIG:
        action_set_network_config(&c->network_config);
        break;
    case APP_CMD_SET_OVERLAY:
        action_set_overlay(c->overlay_title, c->overlay_body);
        break;
    case APP_CMD_REFRESH_NOW:
        if (s_poll_wake) xSemaphoreGive(s_poll_wake);
        break;
    case APP_CMD_DATA:
    case APP_CMD_CARD_ADVANCED: {
        state_lock();
        bool current = source_guard_accepts(&s_source_guard, c->source_generation);
        state_unlock();
        if (!current) {
            ESP_LOGI(TAG, "stale queued card discarded after source change");
            break;
        }
        push_status_to_ui();
        push_data_to_ui();
        present_full();
        break;
    }
    }
}

/* Returns true if the button was still held after `ms`. Releases early. */
static bool held_for(button_id_t id, int ms)
{
    int waited = 0;
    while (waited < ms) {
        if (!buttons_is_pressed(id)) return false;
        vTaskDelay(pdMS_TO_TICKS(HOLD_POLL_MS));
        waited += HOLD_POLL_MS;
    }
    return buttons_is_pressed(id);
}

/* The nav state machine decides what a press means; this decides what to do
 * about the answer. Nothing else in the firmware knows the button mapping. */
static void handle_press(button_id_t id)
{
    /* KEY2's long press is the Wi-Fi escape hatch and is not a navigation
     * event — it reboots the board. Caught before the state machine sees it,
     * because the state machine has no notion of how long a press was. */
    if (id == BUTTON_KEY2 && held_for(BUTTON_KEY2, FORCE_AP_HOLD_MS)) {
        force_ap_mode();               /* reboots — does not return */
    }

    kanji_nav_result_t r;
    kanji_nav_t nav;
    bool queued_grade = false;
    kanji_source_t queued_source = KANJI_SOURCE_NONE;
    state_lock();
    r = kanji_nav_press(&s_nav, (kanji_button_t)id, &s_data);
    nav = s_nav;
    if (r.action == KANJI_ACT_SUBMIT && !s_pending_grade_valid) {
        /* One slot, and the SECOND press while it is full is dropped rather
         * than replacing it. Both presses land on the same card — the nav does
         * not move until the graded reply arrives — but by the time KanjiTask
         * read the first, the proxy is serving the NEXT card, so a second
         * rating would be recorded against a card the learner has not seen.
         * Dropping it costs one press; keeping it corrupts a review history
         * nothing on the board would ever show. */
        const study_grade_route_t route = study_grade_capture(
            &s_pending_grade, s_data.source, nav.grade,
            s_catalog_ordinal, s_data.card.id);
        if (route != STUDY_GRADE_NONE) {
            s_pending_grade_valid = true;
            s_pending_grade_generation = source_guard_capture(&s_source_guard);
            queued_grade = true;
            queued_source = s_pending_grade.source;
        }
    }
    state_unlock();

    switch (r.action) {
    case KANJI_ACT_DRAW_FULL:
        if (Lvgl_lock(-1)) {
            ui_kanji_set_nav(&nav);
            Lvgl_unlock();
        }
        present_full();
        break;

    case KANJI_ACT_DRAW_DOCK:
        if (Lvgl_lock(-1)) {
            ui_kanji_set_nav(&nav);
            Lvgl_unlock();
        }
        present_dock();
        break;

    case KANJI_ACT_SUBMIT:
        /* Hand the rating to KanjiTask and return. The panel is deliberately
         * left showing the answer the learner just graded: the next card is
         * what the grade request returns, so drawing anything now would be
         * drawing a guess. */
        if (queued_grade) {
            ESP_LOGI(TAG, "grading %s from captured %s source",
                     kanji_grade_name(nav.grade),
                     queued_source == KANJI_SOURCE_CATALOG
                         ? "catalog" : "remote");
            if (s_poll_wake) xSemaphoreGive(s_poll_wake);
        } else {
            ESP_LOGW(TAG, "%s cannot be queued: request occupied or source ungradable",
                     kanji_grade_name(nav.grade));
        }
        break;

    case KANJI_ACT_REFRESH:
        if (s_poll_wake) xSemaphoreGive(s_poll_wake);
        break;

    case KANJI_ACT_NONE:
    default:
        break;
    }
}

/*
 * UiTask — the only task that touches LVGL or the panel. Blocks on buttons OR
 * app commands, and wakes every TICK_SECONDS to keep battery telemetry current.
 */
static void UiTask(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "controls: KEY0/KEY1 per the footer, KEY2 = refresh "
                  "(hold 5s = Wi-Fi setup), BOOT = sheets");

    /* TaskInit publishes the restored catalog card before this task starts.
     * The mock remains a last-resort fallback if the catalog was unavailable. */
    state_lock();
    if (!s_data.valid) {
        kanji_mock(&s_data);
        s_hash = kanji_hash(&s_data);
        kanji_nav_reset(&s_nav);
    }
    state_unlock();

    read_battery();
    push_status_to_ui();
    push_data_to_ui();
    present_full();

    for (;;) {
        QueueSetMemberHandle_t member =
            xQueueSelectFromSet(s_queue_set, pdMS_TO_TICKS(TICK_SECONDS * 1000));

        if (member == s_btn_queue) {
            button_event_t ev;
            if (xQueueReceive(s_btn_queue, &ev, 0) == pdTRUE) {
                handle_press(ev.id);
            }
        } else if (member == s_cmd_queue) {
            app_cmd_t cmd;
            if (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
                handle_cmd(&cmd);
            }
        } else {
            /* An idle wake updates battery state only. Nothing on the glass
             * changes with the clock, so unchanged content never refreshes it. */
            read_battery();
        }
    }
}

/* --- the card poller (KanjiTask only) -------------------------------------- */

/* Post a command to UiTask. The only way anything off UiTask causes a repaint. */
static void notify_ui(app_cmd_kind_t kind, uint32_t source_generation)
{
    if (!s_cmd_queue) {
        return;
    }
    app_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.kind = kind;
    c.source_generation = source_generation;
    /* Do not advance the render fingerprint and then silently lose its draw.
     * UiTask is the queue's sole consumer and never waits on KanjiTask, so this
     * bounded producer can safely wait until a slot is available. */
    xQueueSend(s_cmd_queue, &c, portMAX_DELAY);
}

/* Static for the same reason as s_ui_copy: a couple of KB of a 16 KB stack that
 * an https:// URL also has to fit a synchronous TLS handshake into. KanjiTask
 * is the only caller. */
static kanji_t s_fetched;

/* Commit a fetched card. Returns whether the pixels would differ from what is
 * already on the glass; the caller decides whether that is worth a refresh.
 * `advanced` resets the interaction state, because a new card is a new question
 * and leaving the previous card's answer revealed would show it. */
static bool commit(uint32_t generation, bool advanced)
{
    const uint32_t h = kanji_hash(&s_fetched);

    state_lock();
    /* Recheck while holding the same lock as the commit. A URL action may run
     * in the few instructions between hashing and this point. */
    const bool current_source = source_guard_accepts(&s_source_guard, generation);
    const bool transitioned = current_source && s_data.source != s_fetched.source;
    bool changed = current_source && (h != s_hash || advanced || transitioned);
    if (advanced) {
        s_pending_grade_valid = false;
    }
    if (current_source) {
        s_data = s_fetched;
        s_hash = h;
        s_last_ok_us = esp_timer_get_time();
        s_last_result = KANJI_FETCH_OK;
        if (advanced || transitioned) kanji_nav_reset(&s_nav);
    }
    state_unlock();

    if (!current_source) {
        ESP_LOGI(TAG, "study source changed during fetch; stale response discarded");
        return false;
    }
    return changed;
}

static void process_local_grade(const study_grade_request_t *request)
{
    bool saved = false;
    bool published = false;
    uint32_t generation = 0;
    uint16_t next_ordinal = request->catalog_ordinal;

    xSemaphoreTake(s_catalog_mtx, portMAX_DELAY);
    if (catalog_store_available() &&
        catalog_store_ordinal() == request->catalog_ordinal) {
        saved = catalog_store_grade(request->grade);
        if (saved) {
            next_ordinal = catalog_store_ordinal();
        }
    }

    const kanji_t *next = saved ? catalog_store_current() : NULL;
    state_lock();
    if (saved) {
        s_catalog_ordinal = next_ordinal;
    }
    if (next != NULL && next->valid && s_data.source == KANJI_SOURCE_CATALOG) {
        s_data = *next;
        s_hash = kanji_hash(&s_data);
        kanji_nav_reset(&s_nav);
        generation = source_guard_capture(&s_source_guard);
        published = true;
    }
    s_pending_grade_valid = false;
    state_unlock();
    xSemaphoreGive(s_catalog_mtx);

    if (!saved) {
        ESP_LOGE(TAG, "local %s failed at catalog ordinal %u; answer preserved",
                 kanji_grade_name(request->grade), request->catalog_ordinal);
        return;
    }
    if (published) {
        ESP_LOGI(TAG, "local %s persisted; catalog advanced to %u",
                 kanji_grade_name(request->grade), next_ordinal);
        notify_ui(APP_CMD_CARD_ADVANCED, generation);
    } else {
        ESP_LOGI(TAG, "local grade persisted after source takeover; panel unchanged");
    }
}

/*
 * KanjiTask — polls the configured URL, sends whatever rating KEY1 committed,
 * and pokes UiTask only when the content it got back differs from what is
 * already on the glass. Never touches LVGL or the panel itself, so a stalled
 * HTTP request cannot hold up a refresh.
 */
static void KanjiTask(void *arg)
{
    (void)arg;
    /* Let the WiFi/TLS stack settle before the first request. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    for (;;) {
        char url[PROV_URL_MAX_LEN + 1];
        uint32_t generation;
        uint32_t current_generation;
        study_grade_request_t request;
        bool have_request;
        state_lock();
        strlcpy(url, s_cfg.study_url, sizeof(url));
        current_generation = source_guard_capture(&s_source_guard);
        have_request = s_pending_grade_valid;
        if (have_request) {
            request = s_pending_grade;
            generation = s_pending_grade_generation;
        } else {
            memset(&request, 0, sizeof(request));
            generation = current_generation;
        }
        state_unlock();

        const study_grade_route_t route = have_request
                                              ? study_grade_route(&request)
                                              : STUDY_GRADE_NONE;
        if (route == STUDY_GRADE_LOCAL) {
            process_local_grade(&request);
        } else {
            const bool grading = route == STUDY_GRADE_REMOTE;
            if (grading && (generation != current_generation || url[0] == '\0')) {
                state_lock();
                s_pending_grade_valid = false;
                state_unlock();
                ESP_LOGW(TAG, "captured remote grade invalidated by source change; dropped");
            } else if (url[0]) {
                kanji_fetch_result_t r =
                    grading ? kanji_service_grade(url, request.grade,
                                                  request.remote_card_id, &s_fetched)
                            : kanji_service_fetch(url, &s_fetched);

                if (r == KANJI_FETCH_OK) {
                    if (commit(generation, grading)) {
                        ESP_LOGI(TAG, "card %s — refreshing",
                                 grading ? "graded" : "changed");
                        notify_ui(grading ? APP_CMD_CARD_ADVANCED : APP_CMD_DATA,
                                  generation);
                    } else {
                        /* The single most common outcome, and the one that must not
                         * cost a panel refresh. */
                        ESP_LOGD(TAG, "study: unchanged, panel untouched");
                    }
                } else {
                    state_lock();
                    if (grading) {
                        s_pending_grade_valid = false;
                    }
                    bool current_source =
                        source_guard_accepts(&s_source_guard, generation);
                    if (current_source) s_last_result = r;
                    state_unlock();
                    if (!current_source) {
                        ESP_LOGI(TAG, "study source changed during fetch; "
                                      "stale response discarded");
                    } else {
                        ESP_LOGW(TAG, "study %s failed: %s",
                                 grading ? "grade" : "fetch",
                                 kanji_fetch_result_name(r));
                        /* The badge in the header is the only thing that changed,
                         * and it is worth one refresh: a learner pressing KEY1 into
                         * a dead proxy otherwise gets no feedback at all. */
                        notify_ui(APP_CMD_DATA, generation);
                    }
                }
            }
        }

        /* Woken early by KEY1 (a grade), KEY2, POST /api/refresh, or a URL
         * change. */
        xSemaphoreTake(s_poll_wake, pdMS_TO_TICKS(POLL_SECONDS * 1000));
    }
}

void UserApp_TaskInit(const prov_config_t *cfg, const int *btn_gpios, int btn_count)
{
    if (cfg != NULL) {
        s_cfg = *cfg;
    } else {
        memset(&s_cfg, 0, sizeof(s_cfg));
    }

    s_mtx         = xSemaphoreCreateMutex();
    s_catalog_mtx = xSemaphoreCreateMutex();
    s_poll_wake   = xSemaphoreCreateBinary();
    s_btn_queue = xQueueCreate(16, sizeof(button_event_t));
    s_cmd_queue = xQueueCreate(8, sizeof(app_cmd_t));
    /* A queue set lets UiTask block on buttons OR app commands in one wait.
     * Both queues must be empty when added, so build the set before
     * buttons_init starts posting. */
    s_queue_set = xQueueCreateSet(16 + 8);
    xQueueAddToSet(s_btn_queue, s_queue_set);
    xQueueAddToSet(s_cmd_queue, s_queue_set);

    s_source_guard.generation = 0;
    s_overlay_visible = false;
    s_pending_grade_valid = false;
    memset(&s_pending_grade, 0, sizeof(s_pending_grade));

    /* The catalog is the boot source and is initialized before either task can
     * run or provisioning can begin. The built-in snapshot is used only when
     * the catalog partition/state cannot produce a valid current card. */
    const bool catalog_ready = catalog_store_init() &&
                               catalog_store_available() &&
                               catalog_store_current() != NULL;
    if (catalog_ready) {
        s_data = *catalog_store_current();
        s_catalog_ordinal = catalog_store_ordinal();
        ESP_LOGI(TAG, "offline catalog ready at ordinal %u",
                 s_catalog_ordinal);
    } else {
        kanji_mock(&s_data);
        s_catalog_ordinal = 0;
        ESP_LOGW(TAG, "offline catalog unavailable; using demo fallback");
    }
    s_hash = kanji_hash(&s_data);
    s_last_result = KANJI_FETCH_NO_URL;
    s_last_ok_us = 0;
    kanji_nav_reset(&s_nav);

    /* Anything the caller did not supply is disabled rather than left as
     * whatever was on the stack — a stray GPIO number here would attach an
     * interrupt to a pin the panel is using. */
    int gpios[BUTTON_COUNT];
    for (int i = 0; i < BUTTON_COUNT; i++) {
        gpios[i] = (btn_gpios && i < btn_count) ? btn_gpios[i] : -1;
    }
    buttons_init(s_btn_queue, gpios);

    /* Create the global TLS-connect gate before any task can call http_get(). */
    http_port_init();

    /* UiTask does no networking, so it needs only a modest stack — the LVGL
     * render itself runs on the LVGL task. Higher priority so a button press is
     * handled the instant it arrives. */
    xTaskCreatePinnedToCore(UiTask, "ui", 8 * 1024, NULL, 4, NULL, 1);

    /* KanjiTask: the JSON is small, but a TLS handshake and cert-bundle
     * validation would run on THIS task's stack (esp_http_client is
     * synchronous) if the user points it at an https:// URL. 16KB is the size
     * that proved stable for the TLS tasks in the firmware this forked from. */
    xTaskCreatePinnedToCore(KanjiTask, "kanji", 16 * 1024, NULL, 2, NULL, 1);
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

bool UserApp_SetNetworkConfig(const prov_config_t *cfg)
{
    if (cfg == NULL || !s_cmd_queue) {
        return false;
    }
    app_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.kind = APP_CMD_SET_NETWORK_CONFIG;
    c.network_config = *cfg;
    return xQueueSend(s_cmd_queue, &c, 0) == pdTRUE;
}

bool UserApp_SetOverlay(const char *title, const char *body)
{
    if (!s_cmd_queue) {
        return false;
    }
    app_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.kind = APP_CMD_SET_OVERLAY;
    if (title != NULL) {
        strlcpy(c.overlay_title, title, sizeof(c.overlay_title));
    }
    if (body != NULL) {
        strlcpy(c.overlay_body, body, sizeof(c.overlay_body));
    }
    return xQueueSend(s_cmd_queue, &c, 0) == pdTRUE;
}

void user_app_snapshot(device_state_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->model, DEVICE_MODEL, sizeof(out->model));
    strlcpy(out->fw, DEVICE_FW, sizeof(out->fw));
    out->poll_seconds       = POLL_SECONDS;
    out->partial_chain      = epd_partial_chain();
    out->full_refresh_ms    = epd_last_full_ms();
    out->partial_refresh_ms = epd_last_partial_ms();
    if (!s_mtx) {
        return;                     /* TaskInit has not run yet */
    }

    state_lock();
    const kanji_screen_t screen = kanji_nav_screen(&s_nav);
    out->screen = (int)screen;
    strlcpy(out->screen_title, kanji_screen_title(screen), sizeof(out->screen_title));
    out->revealed = s_nav.revealed;
    out->grade    = (int)s_nav.grade;

    out->card_valid = s_data.card.valid;
    out->demo       = s_data.demo;
    strlcpy(out->deck, s_data.session.deck, sizeof(out->deck));
    strlcpy(out->front, s_data.card.front, sizeof(out->front));
    strlcpy(out->reading, s_data.card.reading, sizeof(out->reading));
    strlcpy(out->meaning, s_data.card.sense_count > 0 ? s_data.card.senses[0] : "",
            sizeof(out->meaning));
    strlcpy(out->fsrs_state, s_data.card.fsrs.state, sizeof(out->fsrs_state));
    strlcpy(out->due, s_data.card.fsrs.due, sizeof(out->due));
    out->streak         = s_data.session.streak;
    out->reviewed_today = s_data.session.reviewed_today;
    out->left_new       = s_data.session.left_new;
    out->left_review    = s_data.session.left_review;
    out->track          = s_data.session.track;
    out->track_total    = s_data.session.track_total;
    out->session_complete = s_data.session.complete;
    out->reps           = s_data.card.fsrs.reps;
    out->lapses         = s_data.card.fsrs.lapses;
    out->stability_days = s_data.card.fsrs.stability_days;
    out->difficulty_pct = s_data.card.fsrs.difficulty_pct;

    strlcpy(out->kanji_url, s_cfg.study_url, sizeof(out->kanji_url));
    strlcpy(out->last_result, kanji_fetch_result_name(s_last_result),
            sizeof(out->last_result));
    if (s_last_ok_us != 0) {
        out->age_seconds = (int)((esp_timer_get_time() - s_last_ok_us) / 1000000);
        out->stale = out->age_seconds > POLL_SECONDS * STALE_AFTER_POLLS;
    } else {
        out->age_seconds = -1;      /* never succeeded — not "zero seconds ago" */
        out->stale = s_cfg.study_url[0] != '\0';
    }

    out->battery_present = s_batt_present;
    out->battery_pct     = s_batt_pct;
    out->battery_mv      = s_batt_mv;
    state_unlock();
}

bool user_app_set_screen(int screen)
{
    if (screen < 0 || screen >= KANJI_SCREEN_COUNT) {
        return false;
    }
    return post_cmd(APP_CMD_SET_SCREEN, screen, NULL);
}

bool user_app_refresh_now(void)
{
    return post_cmd(APP_CMD_REFRESH_NOW, 0, NULL);
}

bool user_app_set_study_url(const char *url)
{
    if (!url || !prov_validate_study_url(url)) {
        return false;
    }
    return post_cmd(APP_CMD_SET_URL, 0, url);
}

bool user_app_display_test(void)
{
    return post_cmd(APP_CMD_DISPLAY_TEST, 0, NULL);
}
