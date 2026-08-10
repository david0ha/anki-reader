/*
 * device_api_model.h — platform-agnostic snapshot of the running device, as
 * exposed to the companion app over GET /api/state.
 *
 * Self-contained on purpose (no ESP-IDF, no LVGL, no provisioning dependency)
 * so the serializer compiles in the host tests and the desktop simulator.
 * user_app fills it under its state lock; device_api_json.c serializes it.
 *
 * This is a SUMMARY, not the vault snapshot. The phone does not need the graph
 * edges or eight note titles — it needs to know the board is alive, what it is
 * showing, and whether the last poll worked. The full snapshot is available
 * from the same source the board polls, which the phone can reach too.
 *
 * Every numeric field is an integer. Nothing here needs a fraction — the panel
 * timings are whole milliseconds and the battery is millivolts — so the class
 * of bug where "%.2f" of a huge magnitude truncates on the decimal point and
 * emits JSON that strict parsers reject is designed out rather than guarded.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define DEV_MODEL_MAXLEN     24
#define DEV_FW_MAXLEN        16
#define DEV_DEVID_MAXLEN     16
#define DEV_IP_MAXLEN        16
#define DEV_PAGE_MAXLEN      32
#define DEV_VAULT_MAXLEN     32
#define DEV_TIME_MAXLEN      24
#define DEV_URL_MAXLEN      129   /* == PROV_URL_MAX_LEN + 1 */
#define DEV_RESULT_MAXLEN    16

typedef struct {
    char model[DEV_MODEL_MAXLEN];
    char fw[DEV_FW_MAXLEN];
    char device_id[DEV_DEVID_MAXLEN];
    char ip[DEV_IP_MAXLEN];

    int  page;                          /* ui_page_t */
    char page_title[DEV_PAGE_MAXLEN];   /* the same name the footer shows */

    /* --- the vault snapshot on the glass --- */
    bool vault_valid;
    bool demo;                          /* rendered from the built-in sample */
    char vault[DEV_VAULT_MAXLEN];
    char generated_at[DEV_TIME_MAXLEN];
    int  notes;
    int  links;
    int  orphans;
    int  tags;
    int  added_today;
    int  added_7d;
    int  agents_total;
    int  agents_running;
    int  recent_count;
    int  inbox_total;

    /* --- how it got there --- */
    char vault_url[DEV_URL_MAXLEN];
    char last_result[DEV_RESULT_MAXLEN];  /* vault_fetch_result_name()        */
    int  poll_seconds;
    int  age_seconds;                     /* since the last SUCCESSFUL fetch  */
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
