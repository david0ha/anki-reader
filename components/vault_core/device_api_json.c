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
 * UTF-8 passes through byte for byte: vault names and note titles are Korean,
 * and JSON strings are defined over Unicode, so escaping them to \u would be
 * legal but pointless. Only the seven mandatory escapes and the C0 controls are
 * rewritten. */
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
    put_str_field(&s, "pageTitle", st->page_title, false);

    put(&s, ",\"vault\":{");
    put_bool_field(&s, "valid", st->vault_valid, true);
    put_bool_field(&s, "demo", st->demo, false);
    put_str_field(&s, "name", st->vault, false);
    put_str_field(&s, "generatedAt", st->generated_at, false);
    put_int_field(&s, "notes", st->notes, false);
    put_int_field(&s, "links", st->links, false);
    put_int_field(&s, "orphans", st->orphans, false);
    put_int_field(&s, "tags", st->tags, false);
    put_int_field(&s, "addedToday", st->added_today, false);
    put_int_field(&s, "added7d", st->added_7d, false);
    put_int_field(&s, "agents", st->agents_total, false);
    put_int_field(&s, "agentsRunning", st->agents_running, false);
    put_int_field(&s, "recent", st->recent_count, false);
    put_int_field(&s, "inbox", st->inbox_total, false);
    put(&s, "}");

    put(&s, ",\"source\":{");
    put_str_field(&s, "url", st->vault_url, true);
    put_str_field(&s, "lastResult", st->last_result, false);
    put_int_field(&s, "pollSeconds", st->poll_seconds, false);
    put_int_field(&s, "ageSeconds", st->age_seconds, false);
    put_bool_field(&s, "stale", st->stale, false);
    put(&s, "}");

    put(&s, ",\"battery\":{");
    put_bool_field(&s, "present", st->battery_present, true);
    put_int_field(&s, "percent", st->battery_pct, false);
    put_int_field(&s, "millivolts", st->battery_mv, false);
    put(&s, "}");

    /* The panel block is not decoration: the refresh policy for this 648x480
     * UC8179 is meant to be chosen from measured timings, and these are them. */
    put(&s, ",\"panel\":{");
    put_int_field(&s, "partialChain", st->partial_chain, true);
    put_int_field(&s, "fullRefreshMs", st->full_refresh_ms, false);
    put_int_field(&s, "partialRefreshMs", st->partial_refresh_ms, false);
    put(&s, "}");

    put(&s, "}");
    return finish(&s);
}
