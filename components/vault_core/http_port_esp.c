/*
 * http_port_esp.c — device HTTP port (esp_http_client + TLS cert bundle).
 *
 * Implements http_get() for the firmware. Mirrors the simulator's libcurl
 * port. The response body is accumulated into PSRAM because a forecast
 * metric=all payload is ~240KB — far too large for internal RAM.
 *
 * Connection reuse: instead of init/perform/cleanup per request (a fresh TLS
 * handshake + cert-bundle validation every time), each worker task keeps ONE
 * persistent esp_http_client handle in thread-local storage and reuses it.
 * Per the ESP-IDF docs, repeated esp_http_client_perform() calls on the same
 * handle ride the open connection when the host is unchanged, so the handshake
 * happens once per host instead of once per GET. On a host change we close the
 * connection (keeping the handle) and let the next perform reconnect. The
 * handle is never shared between tasks (esp_http_client is not thread-safe), so
 * the two fetch workers each own their own connection.
 */
#include "http_port.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define HTTP_MAX_RESP (320 * 1024)   /* hard cap; metric=all is ~240KB */

static const char *TAG = "http";

/* Global TLS-connect gate (NOT __thread): held only across a perform() that will
 * (re)connect, so at most one heavy ECDSA cert-chain verify runs at a time even
 * if the boot stagger drifts. Same-host keep-alive performs skip it, so the hot
 * path stays lock-free. Created once by http_port_init() before any fetch task
 * starts (so http_get never has to lazily create it under a race). */
static SemaphoreHandle_t s_tls_connect_lock;

void http_port_init(void) {
    if (!s_tls_connect_lock) s_tls_connect_lock = xSemaphoreCreateMutex();
}

typedef struct { char *buf; size_t len; size_t cap; bool oom; } acc_t;

static esp_err_t on_evt(esp_http_client_event_t *e) {
    if (e->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    acc_t *a = (acc_t *)e->user_data;
    if (!a || a->oom) return ESP_OK;

    if (a->len + e->data_len + 1 > a->cap) {
        size_t ncap = a->cap ? a->cap : 8192;
        while (ncap < a->len + e->data_len + 1) ncap *= 2;
        if (ncap > HTTP_MAX_RESP) { a->oom = true; ESP_LOGW(TAG, "response > cap"); return ESP_OK; }
        char *p = heap_caps_realloc(a->buf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) { a->oom = true; ESP_LOGE(TAG, "PSRAM OOM"); return ESP_OK; }
        a->buf = p; a->cap = ncap;
    }
    memcpy(a->buf + a->len, e->data, e->data_len);
    a->len += e->data_len;
    a->buf[a->len] = '\0';
    return ESP_OK;
}

/* One persistent client + the host its connection is currently bound to, per
 * worker task (thread-local: each task gets its own copy, zero-initialized). */
static __thread esp_http_client_handle_t t_client;
static __thread char t_host[128];

/* Copy the "host[:port]" of a URL (between "://" and the next '/') into out. */
static void host_of(const char *url, char *out, size_t n) {
    out[0] = '\0';
    const char *p = strstr(url, "://");
    if (!p) return;
    p += 3;
    size_t i = 0;
    while (p[i] && p[i] != '/' && i + 1 < n) { out[i] = p[i]; i++; }
    out[i] = '\0';
}

char *http_get(const char *url, int *out_status) {
    if (out_status) *out_status = 0;

    acc_t a = {0};
    char host[sizeof(t_host)];
    host_of(url, host, sizeof(host));

    /* Will this request open a new TLS connection (no handle, no live connection,
     * or a host change)? If so we serialize the perform below so two tasks don't
     * run their ECDSA cert verify at once. Same-host keep-alive -> false -> no lock. */
    bool will_handshake = (!t_client) || (t_host[0] == '\0') || (strcmp(host, t_host) != 0);

    /* Different host than the connection we're holding open -> drop the
     * connection but keep the handle so we can reconnect cheaply. */
    if (t_client && strcmp(host, t_host) != 0) {
        esp_http_client_close(t_client);
        t_host[0] = '\0';
    }

    if (!t_client) {
        esp_http_client_config_t cfg = {
            .url               = url,
            .event_handler     = on_evt,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms        = 15000,
            .buffer_size       = 4096,
            .keep_alive_enable = true,   /* TCP keepalive: detect dead idle sockets */
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
            /* Save the negotiated TLS session so a later reconnect to the SAME host
             * resumes (abbreviated handshake, skips the ECDSA cert verify). Helps the
             * single-host slow weather task (30 min) whose keep-alive the
             * server drops while idle; the cert bundle still gates a full handshake. */
            .save_client_session = true,
#endif
            .user_agent        = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                                 "AppleWebKit/537.36 (KHTML, like Gecko) "
                                 "Chrome/120 Safari/537.36",
        };
        t_client = esp_http_client_init(&cfg);
        if (!t_client) { free(a.buf); return NULL; }
    }

    /* Point the (reused) handle at this request. user_data carries our
     * accumulator and must be (re)set every call since it lives on our stack. */
    esp_http_client_set_url(t_client, url);
    esp_http_client_set_user_data(t_client, &a);

    /* Serialize only the (re)connect+handshake; same-host reuse runs lock-free. */
    if (will_handshake && s_tls_connect_lock) xSemaphoreTake(s_tls_connect_lock, portMAX_DELAY);
    esp_err_t err = esp_http_client_perform(t_client);
    int code = esp_http_client_get_status_code(t_client);
    if (will_handshake && s_tls_connect_lock) xSemaphoreGive(s_tls_connect_lock);

    if (out_status) *out_status = code;
    if (err != ESP_OK || a.oom) {
        if (err != ESP_OK) ESP_LOGW(TAG, "GET failed: %s", esp_err_to_name(err));
        /* The connection may be poisoned; close it so the next call reconnects
         * cleanly (the handle itself stays valid and reusable). */
        esp_http_client_close(t_client);
        t_host[0] = '\0';
        free(a.buf);
        return NULL;
    }

    /* Success: remember the host so same-host follow-ups skip the handshake. */
    strncpy(t_host, host, sizeof(t_host) - 1);
    t_host[sizeof(t_host) - 1] = '\0';
    return a.buf;   /* caller frees */
}
