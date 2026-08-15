/*
 * device_api_model.h — platform-agnostic snapshot of the running device, as
 * exposed to the companion app over GET /api/state.
 *
 * Self-contained on purpose (no ESP-IDF, no LVGL, no kanji_model.h, no
 * provisioning dependency) so the serializer compiles in the host tests and the
 * desktop simulator. user_app fills it under its state lock; device_api_json.c
 * serializes it.
 *
 * This is a SUMMARY, not the study snapshot. The phone does not need three
 * senses, three examples, three shape parts and three comments — it needs to
 * know the board is alive, which card it is on, whether the answer is showing,
 * and whether the last poll worked. Anything richer is one request away from the
 * same proxy the board polls, which the phone can reach too.
 *
 * The string capacities are restated here rather than included from
 * kanji_model.h, and each is pinned to the cap it must not fall short of.
 * user_app strlcpy()s straight into these buffers, so a field one byte under its
 * source cap would cut a かな reading short on the wire only — the panel would
 * show the whole thing and the phone would show a lie, which is the hardest kind
 * of truncation bug to notice.
 *
 * Every numeric field is an integer. Nothing here needs a fraction — FSRS
 * stability is whole days, difficulty is whole percent, the panel timings are
 * whole milliseconds and the battery is millivolts — so the class of bug where
 * "%.2f" of a huge magnitude truncates on the decimal point and emits JSON that
 * strict parsers reject is designed out rather than guarded. Two of those
 * integers carry a sentinel rather than a value; both are marked below, and both
 * mean "the scheduler has not decided yet", which is not zero.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define DEV_MODEL_MAXLEN     24
#define DEV_FW_MAXLEN        16
#define DEV_DEVID_MAXLEN     16
#define DEV_IP_MAXLEN        16
#define DEV_SCREEN_MAXLEN    24   /* kanji_screen_title(): "문제" or "정답"  */
#define DEV_DECK_MAXLEN      40   /* >= KANJI_DECK_MAX                       */
#define DEV_FRONT_MAXLEN     40   /* >= KANJI_FRONT_MAX                      */
#define DEV_READING_MAXLEN   64   /* >= KANJI_READING_MAX                    */
#define DEV_MEANING_MAXLEN   48   /* >= KANJI_SENSE_MAX                      */
#define DEV_LABEL_MAXLEN     24   /* >= KANJI_LABEL_MAX: fsrs state, due     */
#define DEV_URL_MAXLEN      129   /* == PROV_URL_MAX_LEN + 1                 */
#define DEV_RESULT_MAXLEN    16   /* kanji_fetch_result_name(): "bad_payload" */

typedef struct {
    char model[DEV_MODEL_MAXLEN];
    char fw[DEV_FW_MAXLEN];
    char device_id[DEV_DEVID_MAXLEN];
    char ip[DEV_IP_MAXLEN];

    /* --- what is on the glass ---
     * The board has four buttons and no touch panel, so the phone drives the
     * same nav state a press does. Reporting it back is what stops the two
     * disagreeing about which screen is up.
     *
     * There are TWO screens now, not five: 유래, 구성요소 and the FSRS figures
     * all sit on the answer face, so a phone that used to page a sheet has
     * nothing left to page. */
    int  screen;                            /* kanji_screen_t, 0..1           */
    char screen_title[DEV_SCREEN_MAXLEN];   /* the word the footer shows      */
    bool revealed;                          /* the answer side is up          */
    int  grade;                             /* kanji_grade_t, once committed  */

    /* --- the card --- */
    bool card_valid;                    /* false = the session served no card */
    bool demo;                          /* the built-in card, no URL set      */
    char front[DEV_FRONT_MAXLEN];
    char reading[DEV_READING_MAXLEN];
    char meaning[DEV_MEANING_MAXLEN];   /* the first sense; the panel shows 3  */
    char fsrs_state[DEV_LABEL_MAXLEN];  /* wire word: new/learning/review/…    */
    char due[DEV_LABEL_MAXLEN];         /* "9일 뒤" — worded by the proxy      */
    int  reps;
    int  lapses;
    int  stability_days;                /* -1 = not scheduled yet             */
    int  difficulty_pct;                /* -1 = not scheduled yet             */

    /* --- the session --- */
    char deck[DEV_DECK_MAXLEN];
    int  streak;
    int  reviewed_today;
    int  left_new;
    int  left_review;
    int  track;                         /* 1-based place in today's queue     */
    int  track_total;
    bool session_complete;

    /* --- how it got there --- */
    char kanji_url[DEV_URL_MAXLEN];
    char last_result[DEV_RESULT_MAXLEN];  /* kanji_fetch_result_name()        */
    int  poll_seconds;
    int  age_seconds;                     /* since the last SUCCESSFUL fetch;
                                           * -1 = never, which is not zero    */
    bool stale;

    /* --- power --- */
    bool battery_present;
    int  battery_pct;
    int  battery_mv;

    /* --- e-Paper ---
     * The refresh timings are here because the panel's refresh policy is meant
     * to be set from measurement on real hardware, and reading them off a phone
     * beats holding a serial cable to a board on a shelf. */
    int  partial_chain;
    int  full_refresh_ms;
    int  partial_refresh_ms;
} device_state_t;
