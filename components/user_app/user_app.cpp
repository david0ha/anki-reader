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

static const char *TAG = "app";
static lv_obj_t   *s_screen;

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
    APP_CMD_DATA,            /* KanjiTask published a card that changed       */
    APP_CMD_CARD_ADVANCED,   /* KanjiTask graded, and the NEXT card is up     */
} app_cmd_kind_t;

typedef struct {
    app_cmd_kind_t kind;
    int  ival;
    uint32_t source_generation;
    char text[PROV_URL_MAX_LEN + 1];
} app_cmd_t;

static QueueHandle_t     s_btn_queue;
static QueueHandle_t     s_cmd_queue;
static QueueSetHandle_t  s_queue_set;
static SemaphoreHandle_t s_mtx;
static SemaphoreHandle_t s_poll_wake;

static inline void state_lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static inline void state_unlock(void) { xSemaphoreGive(s_mtx); }

/* --- state (guarded by s_mtx unless noted) -------------------------------- */

static kanji_t  s_data;                 /* what is on (or going to) the glass */
static uint32_t s_hash;
/* Invalidates any synchronous HTTP fetch that started before the source changed. */
static source_guard_t s_source_guard;

/* The interaction state. Owned by UiTask, but read by the companion API, so it
 * lives under the same lock as everything else it reports beside. */
static kanji_nav_t s_nav;

/* The rating KEY1 committed, waiting for KanjiTask to send it.
 * KANJI_GRADE_AGAIN..EASY when one is pending, 0 when none is. A single slot
 * rather than a queue: a learner cannot grade two cards before the first
 * answer comes back, because the next card is what the answer returns. */
static kanji_grade_t s_pending_grade;

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
    st.online = (s_last_result == KANJI_FETCH_OK) ||
                (s_cfg.study_url[0] == '\0');   /* the demo card is never offline */
    if (s_last_ok_us != 0) {
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
    strlcpy(s_cfg.study_url, url, sizeof(s_cfg.study_url));
    source_guard_advance(&s_source_guard);
    /* A rating committed against the old source must not be sent to the new
     * one. The source guard already discards a REPLY that arrives after a
     * change; this is the same rule for a request that has not left yet. */
    s_pending_grade = (kanji_grade_t)0;
    /* Clearing the URL means "go back to the demo card", and it has to happen
     * here rather than by waiting for a poll — with no URL there is no poll, so
     * the board would otherwise sit on the last real card indefinitely and then
     * keep reporting it stale, which is the opposite of what was asked. */
    bool to_demo = (url[0] == '\0');
    if (to_demo) {
        kanji_mock(&s_data);
        s_hash = kanji_hash(&s_data);
        kanji_nav_reset(&s_nav);
        s_last_ok_us = 0;
        s_last_result = KANJI_FETCH_NO_URL;
    }
    state_unlock();

    if (!prov_store_save(&s_cfg)) {
        ESP_LOGW(TAG, "study URL change: NVS save failed (will not survive reboot)");
    }
    ESP_LOGI(TAG, "study URL set to '%s'%s", url, to_demo ? " (demo card)" : "");

    if (to_demo) {
        push_status_to_ui();
        push_data_to_ui();
        present_full();
    }
    if (s_poll_wake) {
        xSemaphoreGive(s_poll_wake);
    }
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
    state_lock();
    r = kanji_nav_press(&s_nav, (kanji_button_t)id, &s_data);
    nav = s_nav;
    if (r.action == KANJI_ACT_SUBMIT && s_pending_grade == (kanji_grade_t)0) {
        /* One slot, and the SECOND press while it is full is dropped rather
         * than replacing it. Both presses land on the same card — the nav does
         * not move until the graded reply arrives — but by the time KanjiTask
         * read the first, the proxy is serving the NEXT card, so a second
         * rating would be recorded against a card the learner has not seen.
         * Dropping it costs one press; keeping it corrupts a review history
         * nothing on the board would ever show. */
        s_pending_grade = nav.grade;
        queued_grade = true;
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
            ESP_LOGI(TAG, "grading %s", kanji_grade_name(nav.grade));
            if (s_poll_wake) xSemaphoreGive(s_poll_wake);
        } else {
            ESP_LOGW(TAG, "a grade is already in flight; %s dropped",
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

    /* The demo card goes up immediately rather than after the first poll: a
     * board that shows a finished screen one second after boot and then quietly
     * replaces it with real data reads as fast, where a board that shows
     * "불러오는 중" for eight seconds reads as broken. */
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
    bool changed = current_source && (h != s_hash || advanced);
    if (current_source) {
        s_data = s_fetched;
        s_hash = h;
        s_last_ok_us = esp_timer_get_time();
        s_last_result = KANJI_FETCH_OK;
        if (advanced) kanji_nav_reset(&s_nav);
    }
    state_unlock();

    if (!current_source) {
        ESP_LOGI(TAG, "study source changed during fetch; stale response discarded");
        return false;
    }
    return changed;
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
        char card_id[KANJI_ID_MAX];
        uint32_t generation;
        kanji_grade_t grade;
        state_lock();
        strlcpy(url, s_cfg.study_url, sizeof(url));
        generation = source_guard_capture(&s_source_guard);
        /* Taken, not peeked: a grade that fails is not retried on the next
         * poll. The proxy would by then be serving a different card, and
         * grading the wrong card is worse than dropping a rating the learner
         * can give again by pressing KEY1. */
        grade = s_pending_grade;
        s_pending_grade = (kanji_grade_t)0;
        /* Copied under the SAME lock as the grade, so the pair can only ever
         * describe one card. Read outside it, a poll landing in between would
         * pair this rating with the next card's id — which is the exact mistake
         * sending the id is meant to catch. */
        strlcpy(card_id, s_data.card.id, sizeof(card_id));
        state_unlock();

        if (url[0]) {
            const bool grading = (grade >= KANJI_GRADE_AGAIN &&
                                  grade <= KANJI_GRADE_EASY);
            kanji_fetch_result_t r =
                grading ? kanji_service_grade(url, grade, card_id, &s_fetched)
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

        /* Woken early by KEY1 (a grade), KEY2, POST /api/refresh, or a URL
         * change. */
        xSemaphoreTake(s_poll_wake, pdMS_TO_TICKS(POLL_SECONDS * 1000));
    }
}

void UserApp_TaskInit(const prov_config_t *cfg, const int *btn_gpios, int btn_count)
{
    s_cfg = *cfg;

    s_mtx       = xSemaphoreCreateMutex();
    s_poll_wake = xSemaphoreCreateBinary();
    s_btn_queue = xQueueCreate(16, sizeof(button_event_t));
    s_cmd_queue = xQueueCreate(8, sizeof(app_cmd_t));
    /* A queue set lets UiTask block on buttons OR app commands in one wait.
     * Both queues must be empty when added, so build the set before
     * buttons_init starts posting. */
    s_queue_set = xQueueCreateSet(16 + 8);
    xQueueAddToSet(s_btn_queue, s_queue_set);
    xQueueAddToSet(s_cmd_queue, s_queue_set);

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
