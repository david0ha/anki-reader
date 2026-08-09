/*
 * device_api_model.h — platform-agnostic snapshot of the running device, as
 * exposed to the companion app over GET /api/state.
 *
 * Self-contained on purpose (no ESP-IDF, no LVGL, no provisioning dependency)
 * so the serializer compiles in the host tests and the desktop simulator.
 * user_app fills it under its state lock; device_api_json.c serializes it.
 *
 * Every numeric field is an integer. The previous revision of this API carried
 * doubles for prices and had to defend against NaN and against "%.2f" of a huge
 * magnitude truncating on the decimal point and producing JSON that strict
 * parsers reject. Nothing here needs a fraction — temperatures are whole
 * degrees and the battery is reported in millivolts — so that whole class of
 * bug is designed out rather than guarded against.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define DEV_MODEL_MAXLEN     24
#define DEV_FW_MAXLEN        16
#define DEV_DEVID_MAXLEN     16
#define DEV_IP_MAXLEN        16
#define DEV_CITY_MAXLEN      64   /* resolved "City, CC"         */
#define DEV_LOCATION_MAXLEN  48   /* == PROV_LOCATION_MAX_LEN    */
#define DEV_HANJA_MAXLEN      8
#define DEV_HANGUL_MAXLEN    12
#define DEV_MESSAGE_MAXLEN   64
#define DEV_FORECAST_MAX      7   /* == WX_FORECAST_MAX          */

typedef struct {
    char dow[4];    /* "FRI" */
    int  wx;        /* wx_kind_t */
    int  lo;
    int  hi;
} dev_forecast_t;

typedef struct {
    char model[DEV_MODEL_MAXLEN];
    char fw[DEV_FW_MAXLEN];
    char device_id[DEV_DEVID_MAXLEN];
    char ip[DEV_IP_MAXLEN];

    int  page;              /* ui_page_t: 0 = omikuji, 1 = home  */

    /* The drawn fortune. valid=false before the first draw. */
    bool fortune_valid;
    int  rank;              /* omikuji_rank_t, 0 = 大凶 .. 6 = 大吉 */
    char rank_hanja[DEV_HANJA_MAXLEN];
    char rank_hangul[DEV_HANGUL_MAXLEN];
    char message[DEV_MESSAGE_MAXLEN];

    /* Today's 일진. */
    int  iljin_index;       /* 0..59, 0 = 甲子 */
    char iljin_hanja[DEV_HANJA_MAXLEN];
    char iljin_hangul[DEV_HANGUL_MAXLEN];

    /* Weather (Open-Meteo). */
    char location[DEV_LOCATION_MAXLEN + 1];   /* what the user typed  */
    bool wx_valid;
    int  wx_kind;                             /* wx_kind_t            */
    int  wx_temp_c;
    char city[DEV_CITY_MAXLEN];               /* geocoded confirmation */
    int  forecast_count;
    dev_forecast_t forecast[DEV_FORECAST_MAX];

    /* Power. */
    bool battery_valid;
    int  battery_pct;
    int  battery_mv;

    /* e-Paper: partial refreshes since the last full one. Exposed so the
     * refresh policy can be observed without a serial cable. */
    int  partial_chain;
} device_state_t;
