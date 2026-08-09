#include "prov_json.h"

#include <stdio.h>
#include <string.h>

static const char HEX[] = "0123456789abcdef";

// Reset to an empty string and report failure. Centralizes the overflow contract.
static int fail(char *out, size_t out_size)
{
    if (out_size > 0) {
        out[0] = '\0';
    }
    return -1;
}

int prov_json_escape(const char *in, char *out, size_t out_size)
{
    if (out_size == 0) {
        return -1;
    }
    if (in == NULL) {
        in = "";
    }
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        const char *rep = NULL;
        char ubuf[7];
        size_t rlen = 1;
        switch (c) {
            case '"':  rep = "\\\""; rlen = 2; break;
            case '\\': rep = "\\\\"; rlen = 2; break;
            case '\b': rep = "\\b";  rlen = 2; break;
            case '\f': rep = "\\f";  rlen = 2; break;
            case '\n': rep = "\\n";  rlen = 2; break;
            case '\r': rep = "\\r";  rlen = 2; break;
            case '\t': rep = "\\t";  rlen = 2; break;
            default:
                if (c < 0x20) {
                    ubuf[0] = '\\'; ubuf[1] = 'u'; ubuf[2] = '0'; ubuf[3] = '0';
                    ubuf[4] = HEX[(c >> 4) & 0xF];
                    ubuf[5] = HEX[c & 0xF];
                    ubuf[6] = '\0';
                    rep = ubuf;
                    rlen = 6;
                }
                break;
        }
        if (rep != NULL) {
            if (o + rlen + 1 > out_size) {
                return fail(out, out_size);
            }
            memcpy(out + o, rep, rlen);
            o += rlen;
        } else {
            if (o + 1 + 1 > out_size) {
                return fail(out, out_size);
            }
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return (int)o;
}

int prov_json_info(char *out, size_t out_size,
                   const char *device_id, const char *model, const char *ap_ssid)
{
    char e_id[400];
    char e_model[160];
    char e_ap[256];
    if (prov_json_escape(device_id, e_id, sizeof(e_id)) < 0 ||
        prov_json_escape(model, e_model, sizeof(e_model)) < 0 ||
        prov_json_escape(ap_ssid, e_ap, sizeof(e_ap)) < 0) {
        return fail(out, out_size);
    }
    int n = snprintf(out, out_size,
                     "{\"deviceId\":\"%s\",\"model\":\"%s\",\"apSsid\":\"%s\"}",
                     e_id, e_model, e_ap);
    if (n < 0 || (size_t)n >= out_size) {
        return fail(out, out_size);
    }
    return n;
}

// Append the formatted fragment at out[o]; returns the new offset or -1 on overflow.
static int append(char *out, size_t out_size, size_t o, const char *fmt, const char *value)
{
    int n = snprintf(out + o, out_size - o, fmt, value);
    if (n < 0 || (size_t)n >= out_size - o) {
        return -1;
    }
    return (int)(o + (size_t)n);
}

int prov_json_status(char *out, size_t out_size,
                     const char *state, const char *ssid, const char *reason)
{
    char e_state[64];
    char e_ssid[400];
    char e_reason[256];
    if (out_size == 0 || prov_json_escape(state, e_state, sizeof(e_state)) < 0) {
        return fail(out, out_size);
    }

    int o = snprintf(out, out_size, "{\"state\":\"%s\"", e_state);
    if (o < 0 || (size_t)o >= out_size) {
        return fail(out, out_size);
    }

    if (ssid != NULL && ssid[0] != '\0') {
        if (prov_json_escape(ssid, e_ssid, sizeof(e_ssid)) < 0) {
            return fail(out, out_size);
        }
        o = append(out, out_size, (size_t)o, ",\"ssid\":\"%s\"", e_ssid);
        if (o < 0) {
            return fail(out, out_size);
        }
    }
    if (reason != NULL && reason[0] != '\0') {
        if (prov_json_escape(reason, e_reason, sizeof(e_reason)) < 0) {
            return fail(out, out_size);
        }
        o = append(out, out_size, (size_t)o, ",\"reason\":\"%s\"", e_reason);
        if (o < 0) {
            return fail(out, out_size);
        }
    }

    if ((size_t)o + 1 + 1 > out_size) {
        return fail(out, out_size);
    }
    out[o++] = '}';
    out[o] = '\0';
    return o;
}

int prov_json_networks(const prov_ap_t *aps, size_t count, char *out, size_t out_size)
{
    static const char OPEN[] = "{\"networks\":[";
    static const char CLOSE[] = "]}";
    const size_t open_len = sizeof(OPEN) - 1;
    const size_t close_len = sizeof(CLOSE) - 1;

    if (out_size < open_len + close_len + 1) {
        return fail(out, out_size);
    }

    size_t o = 0;
    memcpy(out + o, OPEN, open_len);
    o += open_len;

    bool first = true;
    for (size_t i = 0; i < count; i++) {
        char esc[400];
        if (prov_json_escape(aps[i].ssid, esc, sizeof(esc)) < 0) {
            continue;  // pathological SSID; skip rather than abort the whole list
        }
        char entry[480];
        int m = snprintf(entry, sizeof(entry),
                         "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                         first ? "" : ",", esc, (int)aps[i].rssi,
                         aps[i].secure ? "true" : "false");
        if (m < 0 || (size_t)m >= sizeof(entry)) {
            continue;
        }
        // Reserve room for this entry plus the closing "]}" and NUL.
        if (o + (size_t)m + close_len + 1 > out_size) {
            break;  // no room for this entry — stop; the array stays valid JSON
        }
        memcpy(out + o, entry, (size_t)m);
        o += (size_t)m;
        first = false;
    }

    memcpy(out + o, CLOSE, close_len);
    o += close_len;
    out[o] = '\0';
    return (int)o;
}
