/*
 * http_port_curl.c — simulator HTTP port (libcurl).
 *
 * Host-only implementation of http_get(). Mirrors the device's esp_http_client
 * port so the simulator exercises the real fetch+parse+render path on macOS
 * (which, unlike the board, has internet). A browser-ish User-Agent is set
 * because some APIs reject default UAs.
 */
#include "http_port.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_MAX_RESP (320 * 1024)   /* mirror the device cap so the sim surfaces oversize responses */

/* No TLS-connect gate on the host: libcurl opens a fresh easy handle per call and
 * the desktop isn't crypto-starved. Defined so both ports satisfy the seam. */
void http_port_init(void) { }

typedef struct { char *buf; size_t len; } membuf_t;

static size_t on_data(void *ptr, size_t size, size_t nmemb, void *userp) {
    size_t add = size * nmemb;
    membuf_t *m = (membuf_t *)userp;
    if (m->len + add + 1 > HTTP_MAX_RESP) return 0;   /* abort: response too large */
    char *p = realloc(m->buf, m->len + add + 1);
    if (!p) return 0;                 /* abort transfer on OOM */
    m->buf = p;
    memcpy(m->buf + m->len, ptr, add);
    m->len += add;
    m->buf[m->len] = '\0';
    return add;
}

char *http_get(const char *url, int *out_status) {
    if (out_status) *out_status = 0;

    CURL *c = curl_easy_init();
    if (!c) return NULL;

    membuf_t m = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36");

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);

    if (out_status) *out_status = (int)code;
    if (rc != CURLE_OK) { free(m.buf); return NULL; }
    return m.buf;   /* may be NULL for an empty 200; caller treats as failure */
}
