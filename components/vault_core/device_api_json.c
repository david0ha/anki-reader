#include "device_api_json.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char HEX[] = "0123456789abcdef";

/* A bounded append cursor. `ok` latches false on the first overflow so callers
 * can write the whole document straight through and check once at the end,
 * instead of testing after every field. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   ok;
} sink_t;

static void put(sink_t *s, const char *str)
{
    if (!s->ok) {
        return;
    }
    size_t n = strlen(str);
    if (s->len + n + 1 > s->cap) {
        s->ok = false;
        return;
    }
    memcpy(s->buf + s->len, str, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

static void put_int(sink_t *s, int v)
{
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    put(s, b);
}

/* Append `in` escaped as the body of a JSON string (no surrounding quotes).
 *
 * UTF-8 passes through byte for byte: the fortune messages and 갑자 names are
 * Korean, and JSON strings are defined over Unicode, so escaping them to \u
 * would be legal but pointless. Only the seven mandatory escapes and the C0
 * controls are rewritten. */
static void put_escaped(sink_t *s, const char *in)
{
    if (!s->ok) {
        return;
    }
    if (in == NULL) {
        in = "";
    }
    for (size_t i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        char one[2] = { (char)c, '\0' };
        switch (c) {
        case '"':  put(s, "\\\""); break;
        case '\\': put(s, "\\\\"); break;
        case '\b': put(s, "\\b");  break;
        case '\f': put(s, "\\f");  break;
        case '\n': put(s, "\\n");  break;
        case '\r': put(s, "\\r");  break;
        case '\t': put(s, "\\t");  break;
        default:
            if (c < 0x20) {
                char u[7] = { '\\', 'u', '0', '0', HEX[(c >> 4) & 0xF], HEX[c & 0xF], '\0' };
                put(s, u);
            } else {
                put(s, one);
            }
            break;
        }
        if (!s->ok) {
            return;
        }
    }
}

static void put_str_field(sink_t *s, const char *key, const char *val, bool first)
{
    put(s, first ? "\"" : ",\"");
    put(s, key);
    put(s, "\":\"");
    put_escaped(s, val);
    put(s, "\"");
}

static void put_int_field(sink_t *s, const char *key, int val, bool first)
{
    put(s, first ? "\"" : ",\"");
    put(s, key);
    put(s, "\":");
    put_int(s, val);
}

static void put_bool_field(sink_t *s, const char *key, bool val, bool first)
{
    put(s, first ? "\"" : ",\"");
    put(s, key);
    put(s, "\":");
    put(s, val ? "true" : "false");
}

static int finish(sink_t *s)
{
    if (!s->ok) {
        if (s->cap > 0) {
            s->buf[0] = '\0';
        }
        return -1;
    }
    return (int)s->len;
}

int device_api_json_info(char *out, size_t out_size,
                         const char *device_id, const char *model,
                         const char *fw, const char *ip)
{
    sink_t s = { out, out_size, 0, out_size > 0 };
    if (out_size > 0) {
        out[0] = '\0';
    }
    put(&s, "{");
    put_str_field(&s, "deviceId", device_id, true);
    put_str_field(&s, "model", model, false);
    put_str_field(&s, "fw", fw, false);
    put_str_field(&s, "ip", ip, false);
    put(&s, "}");
    return finish(&s);
}

int device_api_json_state(const device_state_t *st, char *out, size_t out_size)
{
    sink_t s = { out, out_size, 0, out_size > 0 };
    if (out_size > 0) {
        out[0] = '\0';
    }
    if (st == NULL) {
        return -1;
    }

    put(&s, "{");
    put_str_field(&s, "deviceId", st->device_id, true);
    put_str_field(&s, "model", st->model, false);
    put_str_field(&s, "fw", st->fw, false);
    put_str_field(&s, "ip", st->ip, false);
    put_int_field(&s, "page", st->page, false);
    put_int_field(&s, "partialChain", st->partial_chain, false);

    put(&s, ",\"fortune\":{");
    put_bool_field(&s, "valid", st->fortune_valid, true);
    put_int_field(&s, "rank", st->rank, false);
    put_str_field(&s, "hanja", st->rank_hanja, false);
    put_str_field(&s, "hangul", st->rank_hangul, false);
    put_str_field(&s, "message", st->message, false);
    put(&s, "}");

    put(&s, ",\"iljin\":{");
    put_int_field(&s, "index", st->iljin_index, true);
    put_str_field(&s, "hanja", st->iljin_hanja, false);
    put_str_field(&s, "hangul", st->iljin_hangul, false);
    put(&s, "}");

    put(&s, ",\"weather\":{");
    put_bool_field(&s, "valid", st->wx_valid, true);
    put_int_field(&s, "kind", st->wx_kind, false);
    put_int_field(&s, "tempC", st->wx_temp_c, false);
    put_str_field(&s, "city", st->city, false);
    put_str_field(&s, "location", st->location, false);
    put(&s, ",\"forecast\":[");
    {
        int n = st->forecast_count;
        if (n < 0) n = 0;
        if (n > DEV_FORECAST_MAX) n = DEV_FORECAST_MAX;
        for (int i = 0; i < n; i++) {
            put(&s, i ? ",{" : "{");
            put_str_field(&s, "dow", st->forecast[i].dow, true);
            put_int_field(&s, "kind", st->forecast[i].wx, false);
            put_int_field(&s, "lo", st->forecast[i].lo, false);
            put_int_field(&s, "hi", st->forecast[i].hi, false);
            put(&s, "}");
        }
    }
    put(&s, "]}");

    put(&s, ",\"battery\":{");
    put_bool_field(&s, "valid", st->battery_valid, true);
    put_int_field(&s, "percent", st->battery_pct, false);
    put_int_field(&s, "millivolts", st->battery_mv, false);
    put(&s, "}");

    put(&s, "}");
    return finish(&s);
}
