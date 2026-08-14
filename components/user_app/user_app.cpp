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
#include "startup_delivery.h"
#include "task_lifecycle.h"
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
    study_draw_token_t draw_token;
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
static SemaphoreHandle_t s_ui_ready;
static user_app_task_resources_t s_task_resources;
static bool s_initialized;
static bool s_http_port_ready;
static bool s_catalog_store_prepared;

static inline void state_lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static inline void state_unlock(void) { xSemaphoreGive(s_mtx); }

/* --- state (guarded by s_mtx unless noted) -------------------------------- */

/* The production arbitration module owns the card, navigation, source guard,
 * and captured grade as one host-tested state machine. */
static study_runtime_t s_study;
#define s_data                       s_study.data
#define s_hash                       s_study.hash
#define s_catalog_ordinal            s_study.catalog_ordinal
#define s_source_guard               s_study.source_guard
#define s_nav                        s_study.nav
#define s_pending_grade              s_study.pending_grade
#define s_pending_grade_valid        s_study.pending_grade_valid
#define s_pending_grade_generation   s_study.pending_grade_generation

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

static bool catalog_available_adapter(void *context)
{
    (void)context;
    return catalog_store_available();
}

static const kanji_t *catalog_current_adapter(void *context)
{
    (void)context;
    return catalog_store_current();
}

static uint16_t catalog_ordinal_adapter(void *context)
{
    (void)context;
    return catalog_store_ordinal();
}

static bool catalog_grade_adapter(void *context, kanji_grade_t grade)
{
    (void)context;
    return catalog_store_grade(grade);
}

static study_catalog_ops_t catalog_ops(void)
{
    return (study_catalog_ops_t){
        .context = NULL,
        .available = catalog_available_adapter,
        .current = catalog_current_adapter,
        .ordinal = catalog_ordinal_adapter,
        .grade = catalog_grade_adapter,
    };
}

static void study_state_lock(void *context)
{
    (void)context;
    state_lock();
}

static void study_state_unlock(void *context)
{
    (void)context;
    state_unlock();
}

static study_state_lock_t study_lock_ops(void)
{
    return (study_state_lock_t){
        .context = NULL,
        .lock = study_state_lock,
        .unlock = study_state_unlock,
    };
}

/* Restore the store's published snapshot, or use the built-in card only when
 * no valid catalog exists. The store lock serializes its pointer swap with a
 * local grade; study_runtime_restore makes card/hash/nav one publication. */
static bool restore_catalog_or_demo(void)
{
    xSemaphoreTake(s_catalog_mtx, portMAX_DELAY);
    const study_restore_result_t result =
        study_runtime_restore(&s_study, catalog_ops(), study_lock_ops());
    state_lock();
    s_last_ok_us = 0;
    s_last_result = KANJI_FETCH_NO_URL;
    state_unlock();
    xSemaphoreGive(s_catalog_mtx);
    return result == STUDY_RESTORE_CATALOG;
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
        study_runtime_advance_source(&s_study);
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
        study_runtime_advance_source(&s_study);
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
        const bool current =
            study_runtime_accepts_draw(&s_study, &c->draw_token);
        state_unlock();
        if (!current) {
            ESP_LOGI(TAG, "stale queued draw discarded after "
                          "publication/source change");
            break;
        }
        /* Local catalog draws are publication-only, so a URL change cannot
         * strand a card already queued behind SET_URL. Remote data/status
         * draws additionally carry the HTTP source generation and are dropped
         * if they came from the URL that SET_URL just replaced. */
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
        const study_grade_route_t route =
            study_runtime_capture_grade(&s_study, nav.grade);
        if (route != STUDY_GRADE_NONE) {
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

    /* TaskInit does not expose the command API until this sole consumer has
     * completed its first frame and is about to enter the queue loop. */
    if (s_ui_ready != NULL) {
        xSemaphoreGive(s_ui_ready);
    }

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
static void notify_ui(app_cmd_kind_t kind, study_draw_token_t draw_token)
{
    if (!s_cmd_queue) {
        return;
    }
    app_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.kind = kind;
    c.draw_token = draw_token;
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
static bool commit(uint32_t generation, bool advanced,
                   study_draw_token_t *draw_token)
{
    state_lock();
    const study_remote_result_t result = study_runtime_commit_remote(
        &s_study, &s_fetched, generation, advanced);
    if (result == STUDY_REMOTE_PUBLISHED && draw_token != NULL) {
        *draw_token = study_runtime_capture_draw(
            &s_study, STUDY_DRAW_PUBLICATION_AND_SOURCE);
    }
    if (result != STUDY_REMOTE_STALE) {
        s_last_ok_us = esp_timer_get_time();
        s_last_result = KANJI_FETCH_OK;
    }
    state_unlock();

    if (result == STUDY_REMOTE_STALE) {
        ESP_LOGI(TAG, "study source changed during fetch; stale response discarded");
        return false;
    }
    return result == STUDY_REMOTE_PUBLISHED;
}

static void process_local_grade(const study_grade_request_t *request)
{
    study_draw_token_t draw_token = {};
    uint16_t next_ordinal = request->catalog_ordinal;

    xSemaphoreTake(s_catalog_mtx, portMAX_DELAY);
    const study_local_result_t result = study_runtime_process_local_grade(
        &s_study, request, catalog_ops(), study_lock_ops());
    if (result == STUDY_LOCAL_PUBLISHED) {
        state_lock();
        draw_token = study_runtime_capture_draw(
            &s_study, STUDY_DRAW_PUBLICATION_ONLY);
        next_ordinal = s_catalog_ordinal;
        state_unlock();
    }
    xSemaphoreGive(s_catalog_mtx);

    if (result == STUDY_LOCAL_FAILED) {
        ESP_LOGE(TAG, "local %s failed at catalog ordinal %u; answer preserved",
                 kanji_grade_name(request->grade), request->catalog_ordinal);
        return;
    }
    if (result == STUDY_LOCAL_PUBLISHED) {
        ESP_LOGI(TAG, "local %s persisted; catalog advanced to %u",
                 kanji_grade_name(request->grade), next_ordinal);
        notify_ui(APP_CMD_CARD_ADVANCED, draw_token);
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
            bool remote_grade_ready = true;
            if (grading) {
                state_lock();
                remote_grade_ready = study_runtime_remote_grade_ready(
                    &s_study, generation, url);
                state_unlock();
            }
            if (grading && !remote_grade_ready) {
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
                    study_draw_token_t draw_token = {};
                    if (commit(generation, grading, &draw_token)) {
                        ESP_LOGI(TAG, "card %s — refreshing",
                                 grading ? "graded" : "changed");
                        notify_ui(grading ? APP_CMD_CARD_ADVANCED : APP_CMD_DATA,
                                  draw_token);
                    } else {
                        /* The single most common outcome, and the one that must not
                         * cost a panel refresh. */
                        ESP_LOGD(TAG, "study: unchanged, panel untouched");
                    }
                } else {
                    study_draw_token_t draw_token = {};
                    state_lock();
                    if (grading) {
                        s_pending_grade_valid = false;
                    }
                    bool current_source =
                        source_guard_accepts(&s_source_guard, generation);
                    if (current_source) {
                        s_last_result = r;
                        draw_token = study_runtime_capture_draw(
                            &s_study, STUDY_DRAW_PUBLICATION_AND_SOURCE);
                    }
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
                        notify_ui(APP_CMD_DATA, draw_token);
                    }
                }
            }
        }

        /* Woken early by KEY1 (a grade), KEY2, POST /api/refresh, or a URL
         * change. */
        xSemaphoreTake(s_poll_wake, pdMS_TO_TICKS(POLL_SECONDS * 1000));
    }
}

static bool task_create_resource(void *context, user_app_resource_kind_t kind,
                                 user_app_handle_t *out)
{
    (void)context;
    if (out == NULL) {
        return false;
    }
    *out = NULL;
    switch (kind) {
    case USER_APP_RESOURCE_STATE_MUTEX:
    case USER_APP_RESOURCE_CATALOG_MUTEX:
        *out = xSemaphoreCreateMutex();
        break;
    case USER_APP_RESOURCE_POLL_WAKE:
    case USER_APP_RESOURCE_UI_READY:
        *out = xSemaphoreCreateBinary();
        break;
    case USER_APP_RESOURCE_BUTTON_QUEUE:
        *out = xQueueCreate(16, sizeof(button_event_t));
        break;
    case USER_APP_RESOURCE_COMMAND_QUEUE:
        *out = xQueueCreate(8, sizeof(app_cmd_t));
        break;
    case USER_APP_RESOURCE_QUEUE_SET:
        *out = xQueueCreateSet(16 + 8);
        break;
    }
    return *out != NULL;
}

static bool task_add_member(void *context, user_app_handle_t member,
                            user_app_handle_t set)
{
    (void)context;
    return xQueueAddToSet((QueueSetMemberHandle_t)member,
                          (QueueSetHandle_t)set) == pdPASS;
}

static void task_remove_member(void *context, user_app_handle_t member,
                               user_app_handle_t set)
{
    (void)context;
    (void)xQueueRemoveFromSet((QueueSetMemberHandle_t)member,
                              (QueueSetHandle_t)set);
}

static bool task_prepare(void *context,
                         const user_app_task_resources_t *resources)
{
    (void)context;
    s_mtx = (SemaphoreHandle_t)resources->state_mutex;
    s_catalog_mtx = (SemaphoreHandle_t)resources->catalog_mutex;
    s_poll_wake = (SemaphoreHandle_t)resources->poll_wake;
    s_ui_ready = (SemaphoreHandle_t)resources->ui_ready;
    s_btn_queue = (QueueHandle_t)resources->btn_queue;
    s_cmd_queue = (QueueHandle_t)resources->cmd_queue;
    s_queue_set = (QueueSetHandle_t)resources->queue_set;

    study_runtime_init(&s_study);
    s_overlay_visible = false;

    /* The catalog is the boot source and is initialized before either task can
     * run or provisioning can begin. The built-in snapshot is used only when
     * the catalog partition/state cannot produce a valid current card. */
    s_catalog_store_prepared = true;
    (void)catalog_store_init();
    const bool catalog_ready = restore_catalog_or_demo();
    if (catalog_ready) {
        ESP_LOGI(TAG, "offline catalog ready at ordinal %u",
                 s_catalog_ordinal);
    } else {
        ESP_LOGW(TAG, "offline catalog unavailable; using demo fallback");
    }

    /* Create the global TLS-connect gate before KanjiTask can call http_get().
     * A later lifecycle failure deletes both tasks before cleanup releases it. */
    s_http_port_ready = http_port_init();
    return s_http_port_ready;
}

static bool task_create(void *context, user_app_task_kind_t kind,
                        user_app_handle_t *out)
{
    (void)context;
    TaskHandle_t task = NULL;
    BaseType_t result;
    if (kind == USER_APP_TASK_UI) {
        result = xTaskCreatePinnedToCore(UiTask, "ui", 8 * 1024, NULL, 4,
                                         &task, 1);
    } else {
        result = xTaskCreatePinnedToCore(KanjiTask, "kanji", 16 * 1024, NULL,
                                         2, &task, 1);
    }
    *out = (user_app_handle_t)task;
    return result == pdPASS && task != NULL;
}

static bool task_wait_ui_ready(void *context, user_app_handle_t ready)
{
    (void)context;
    return xSemaphoreTake((SemaphoreHandle_t)ready,
                          pdMS_TO_TICKS(30000)) == pdTRUE;
}

static void task_delete(void *context, user_app_handle_t task)
{
    (void)context;
    vTaskDelete((TaskHandle_t)task);
}

static void task_delete_resource(void *context,
                                 user_app_resource_kind_t kind,
                                 user_app_handle_t resource)
{
    (void)context;
    (void)kind;
    vQueueDelete((QueueHandle_t)resource);
}

static void task_publish(void *context,
                         const user_app_task_resources_t *resources)
{
    (void)context;
    (void)resources;
    s_ui_ready = NULL;
    s_initialized = true;
}

static void task_cleanup_complete(void *context)
{
    (void)context;
    if (s_http_port_ready) {
        http_port_deinit();
        s_http_port_ready = false;
    }
    if (s_catalog_store_prepared) {
        catalog_store_release();
        s_catalog_store_prepared = false;
    }
    s_initialized = false;
    s_mtx = NULL;
    s_catalog_mtx = NULL;
    s_poll_wake = NULL;
    s_ui_ready = NULL;
    s_btn_queue = NULL;
    s_cmd_queue = NULL;
    s_queue_set = NULL;
}

user_app_init_result_t UserApp_TaskInit(const prov_config_t *cfg,
                                        const int *btn_gpios,
                                        int btn_count)
{
    if (s_initialized) {
        return USER_APP_INIT_ALREADY_STARTED;
    }
    if (cfg != NULL) {
        s_cfg = *cfg;
    } else {
        memset(&s_cfg, 0, sizeof(s_cfg));
    }

    const user_app_task_ops_t ops = {
        .context = NULL,
        .create_resource = task_create_resource,
        .add_member = task_add_member,
        .remove_member = task_remove_member,
        .prepare = task_prepare,
        .create_task = task_create,
        .wait_ui_ready = task_wait_ui_ready,
        .delete_task = task_delete,
        .delete_resource = task_delete_resource,
        .publish = task_publish,
        .cleanup_complete = task_cleanup_complete,
    };
    const user_app_init_result_t result =
        user_app_task_lifecycle_start(&ops, &s_task_resources);
    if (result != USER_APP_INIT_OK) {
        ESP_LOGE(TAG, "task initialization failed at stage %d", (int)result);
        return result;
    }

    /* Anything the caller did not supply is disabled rather than left as
     * whatever was on the stack — a stray GPIO number here would attach an
     * interrupt to a pin the panel is using. */
    int gpios[BUTTON_COUNT];
    for (int i = 0; i < BUTTON_COUNT; i++) {
        gpios[i] = (btn_gpios && i < btn_count) ? btn_gpios[i] : -1;
    }
    buttons_init(s_btn_queue, gpios);
    return USER_APP_INIT_OK;
}

/* ===========================================================================
 * Companion-app control bridge (declared in user_app_api.h). These run on the
 * HTTP server task: the read copies state under s_mtx; the writes post a
 * command for UiTask to apply via the same paths as a button press. All are
 * safe no-ops until UserApp_TaskInit has completed successfully.
 * =========================================================================== */

static bool post_cmd(app_cmd_kind_t kind, int ival, const char *text)
{
    if (!s_initialized || !s_cmd_queue) {
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

static bool send_critical_command(void *context, const void *item,
                                  uint32_t wait_policy)
{
    QueueHandle_t queue = (QueueHandle_t)context;
    const TickType_t wait = wait_policy == USER_APP_CRITICAL_QUEUE_WAIT
                                ? portMAX_DELAY
                                : pdMS_TO_TICKS(wait_policy);
    return queue != NULL &&
           xQueueSend(queue, item, wait) == pdTRUE;
}

bool UserApp_SetNetworkConfig(const prov_config_t *cfg)
{
    if (cfg == NULL || !s_initialized || !s_cmd_queue) {
        return false;
    }
    app_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.kind = APP_CMD_SET_NETWORK_CONFIG;
    c.network_config = *cfg;
    return startup_queue_send_critical(send_critical_command, s_cmd_queue, &c,
                                       USER_APP_CRITICAL_QUEUE_WAIT);
}

bool UserApp_SetOverlay(const char *title, const char *body)
{
    if (!s_initialized || !s_cmd_queue) {
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
    return startup_queue_send_critical(send_critical_command, s_cmd_queue, &c,
                                       USER_APP_CRITICAL_QUEUE_WAIT);
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
    if (!s_initialized || !s_mtx) {
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
