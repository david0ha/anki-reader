#include "form_parse.h"

#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode a bounded [in, in+in_len) range of x-www-form-urlencoded text into `out`.
static int decode_range(const char *in, size_t in_len, char *out, size_t out_size)
{
    if (out_size == 0) {
        return -1;
    }
    size_t oi = 0;
    for (size_t i = 0; i < in_len;) {
        char ch = in[i];
        if (ch == '+') {
            ch = ' ';
            i++;
        } else if (ch == '%' && i + 2 < in_len &&
                   hex_value(in[i + 1]) >= 0 && hex_value(in[i + 2]) >= 0) {
            ch = (char)((hex_value(in[i + 1]) << 4) | hex_value(in[i + 2]));
            i += 3;
            if (ch == '\0') {
                out[0] = '\0';
                return -1;  // %00: an embedded NUL would truncate the value; reject it
            }
        } else {
            i++;  // literal byte (covers malformed '%' sequences)
        }
        if (oi + 1 >= out_size) {
            out[0] = '\0';
            return -1;  // would overflow (no room for byte + NUL)
        }
        out[oi++] = ch;
    }
    out[oi] = '\0';
    return (int)oi;
}

int prov_url_decode(const char *in, char *out, size_t out_size)
{
    return decode_range(in, strlen(in), out, out_size);
}

bool prov_form_get_field(const char *body, const char *key, char *out, size_t out_size)
{
    if (out_size > 0) {
        out[0] = '\0';
    }
    if (body == NULL || key == NULL) {
        return false;
    }

    size_t key_len = strlen(key);
    for (const char *p = body; p != NULL && *p;) {
        // `p` always sits at the start of a field (string start or just after '&').
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val = p + key_len + 1;
            const char *amp = strchr(val, '&');
            size_t val_len = amp ? (size_t)(amp - val) : strlen(val);
            return decode_range(val, val_len, out, out_size) >= 0;
        }
        const char *amp = strchr(p, '&');
        p = amp ? amp + 1 : NULL;
    }
    return false;
}
