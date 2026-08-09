#include "prov_portal.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "prov_wifi.h"
#include "form_parse.h"
#include "prov_json.h"

static const char *TAG = "prov_portal";

// Embedded setup page (see portal.html). EMBED_TXTFILES NUL-terminates it.
extern const char portal_html_start[] asm("_binary_portal_html_start");

static prov_config_t              s_current;
static bool                       s_have_current;
static prov_portal_save_cb_t      s_on_save;
static prov_portal_provision_cb_t s_on_provision;
static void                      *s_user;

// Identity served by GET /api/info (copied from the caller's prov_portal_info_t at start).
static char s_device_id[128];
static char s_model[32];
static char s_ap_ssid[40];

// Status reported by GET /api/status, written by the connect-test task via
// prov_portal_set_status and read by the HTTP task. Guarded by s_status_mtx.
static SemaphoreHandle_t   s_status_mtx;
static prov_portal_state_t s_state;
static char                s_status_ssid[PROV_SSID_MAX_LEN + 1];
static char                s_status_reason[32];

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------

// Force single-use connections. The iOS Captive Network Assistant (and Android's
// CaptivePortalLogin) runs the portal inside a sandboxed WebView that is unreliable about
// reusing an HTTP/1.1 keep-alive socket for its follow-up XHRs. Combined with the server's
// lru_purge reclaiming idle sockets, the browser's fetch('/scan')/fetch('/state') ended up
// written onto a connection the server had already moved on from, so those requests never
// reached a handler (the dropdown hung on "Scanning…"). Sending "Connection: close" on every
// response makes the client open a fresh connection per request — the same mitigation
// xiaozhi's esp-wifi-connect applies to all of its handlers. Must be set before the first
// send/chunk; the literals have static storage so the queued header stays valid.
static void no_keepalive(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
}

// Read the full request body into `buf` (NUL-terminated). Returns the byte count, -1 if the
// body exceeds `bufsz`-1 (caller should reply 413), or -2 on a socket read error (reply 400).
// Rejecting oversize bodies rather than truncating avoids leaving unread bytes in the stream.
static int read_body(httpd_req_t *req, char *buf, size_t bufsz)
{
    if (req->content_len > bufsz - 1) {
        return -1;
    }
    int total = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf + total, remaining);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;   // transient recv timeout/interrupt — retry rather than abort the body
        }
        if (r <= 0) {
            return -2;
        }
        total += r;
        remaining -= r;
    }
    buf[total] = '\0';
    return total;
}

// Escape text for an HTML attribute/text context (&, <, >, ", '). Always NUL-terminates;
// silently stops if `out` fills (callers size `out` for the worst case of 6 bytes/char).
static void html_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        const char *rep = NULL;
        switch (in[i]) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default: break;
        }
        if (rep != NULL) {
            size_t l = strlen(rep);
            if (o + l >= outsz) break;
            memcpy(out + o, rep, l);
            o += l;
        } else {
            if (o + 1 >= outsz) break;
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

// Send the embedded template up to `marker` as a chunk; return the pointer just past it.
// If the marker is absent, flush the remainder and return NULL so the caller stops injecting.
static const char *emit_until(httpd_req_t *req, const char *from, const char *marker)
{
    if (from == NULL) {
        return NULL;
    }
    const char *m = strstr(from, marker);
    if (m == NULL) {
        httpd_resp_sendstr_chunk(req, from);
        return NULL;
    }
    httpd_resp_send_chunk(req, from, m - from);
    return m + strlen(marker);
}

// Server-render the <option> list from the background-scan cache. The captive WebView does
// not reliably run fetch()/XHR, so the network list must be baked into the page rather than
// loaded by a second request. Marks the saved network selected and always appends "Other…".
static void emit_options(httpd_req_t *req)
{
    static prov_ap_t aps[24];
    size_t count = prov_wifi_scan_cached(aps, sizeof(aps) / sizeof(aps[0]));
    ESP_LOGI(TAG, "rendering %u cached network(s) into portal page", (unsigned)count);

    const char *saved = s_have_current ? s_current.ssid : "";
    bool saved_seen = false;

    for (size_t i = 0; i < count; i++) {
        char esc[6 * sizeof(aps[0].ssid) + 1];
        html_escape(aps[i].ssid, esc, sizeof(esc));
        bool sel = (saved[0] != '\0' && strcmp(aps[i].ssid, saved) == 0);
        if (sel) saved_seen = true;
        const char *bars = aps[i].rssi >= -55 ? "●●●" : aps[i].rssi >= -70 ? "●●" : "●";
        char opt[600];
        snprintf(opt, sizeof(opt), "<option value=\"%s\"%s>%s%s   %s</option>",
                 esc, sel ? " selected" : "", esc, aps[i].secure ? " 🔒" : "", bars);
        httpd_resp_sendstr_chunk(req, opt);
    }
    // A previously-saved network that is out of range right now should still appear (selected),
    // so reconfiguring shows the current choice rather than silently dropping it.
    if (saved[0] != '\0' && !saved_seen) {
        char esc[6 * sizeof(s_current.ssid) + 1];
        html_escape(saved, esc, sizeof(esc));
        char opt[600];
        snprintf(opt, sizeof(opt), "<option value=\"%s\" selected>%s   (saved)</option>", esc, esc);
        httpd_resp_sendstr_chunk(req, opt);
    }
    if (count == 0 && saved[0] == '\0') {
        httpd_resp_sendstr_chunk(req, "<option value=\"\">No networks found — tap ⟳ to reload</option>");
    }
    httpd_resp_sendstr_chunk(req, "<option value=\"__manual__\">Other network…</option>");
}

// Fill the {{LOCATION}} slot with the saved weather location (escaped), so
// reconfiguring pre-populates the field. Empty for a fresh setup.
static void emit_saved_location(httpd_req_t *req)
{
    if (!s_have_current || s_current.location[0] == '\0') {
        return;
    }
    char esc[PROV_LOCATION_MAX_LEN * 6 + 1];
    html_escape(s_current.location, esc, sizeof(esc));
    httpd_resp_sendstr_chunk(req, esc);
}

static esp_err_t index_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET %s (serving portal page)", req->uri);
    no_keepalive(req);
    httpd_resp_set_type(req, "text/html");

    // Stream the template, substituting the server-rendered network list and the
    // saved location at their markers. No client-side fetch is required to populate the form.
    const char *p = portal_html_start;
    p = emit_until(req, p, "{{OPTIONS}}");
    emit_options(req);
    p = emit_until(req, p, "{{LOCATION}}");
    emit_saved_location(req);
    if (p != NULL) {
        httpd_resp_sendstr_chunk(req, p);
    }
    httpd_resp_sendstr_chunk(req, NULL);  // end response
    return ESP_OK;
}

// Minimal styled result page. Because the form now does a native POST (a full-page
// navigation, not an XHR), /save must answer with HTML the browser can display.
static esp_err_t send_result_page(httpd_req_t *req, const char *title, const char *msg, bool ok)
{
    no_keepalive(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Ticker Board</title><style>"
        "body{margin:0;min-height:100vh;display:grid;place-items:center;padding:24px;"
        "background:#0b0e14;color:#e8edf4;font-family:system-ui,-apple-system,sans-serif}"
        ".box{max-width:380px;text-align:center}.box h1{font-size:18px;margin:0 0 10px}"
        ".box p{color:#8a93a6;font-size:14px;margin:0 0 18px;line-height:1.5}"
        ".box a{display:inline-block;padding:11px 16px;border-radius:10px;text-decoration:none;"
        "font-weight:600;background:#1fcf8f;color:#06231a}</style></head><body><div class=\"box\">");
    char buf[320];
    snprintf(buf, sizeof(buf), "<h1 style=\"color:%s\">%s</h1><p>%s</p>",
             ok ? "#1fcf8f" : "#ff5c6c", title, msg);
    httpd_resp_sendstr_chunk(req, buf);
    if (!ok) {
        httpd_resp_sendstr_chunk(req, "<a href=\"/\">← Back to setup</a>");
    }
    httpd_resp_sendstr_chunk(req, "</div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[1024];
    if (req->content_len > sizeof(body) - 1) {
        // The legitimate form is a few hundred bytes; reject anything larger rather than
        // silently truncating (which would leave unread bytes in the stream).
        httpd_resp_set_status(req, "413 Payload Too Large");
        return send_result_page(req, "Request too large", "Please go back and try again.", false);
    }
    int total = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, body + total, remaining);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error");
        }
        total += r;
        remaining -= r;
    }
    body[total] = '\0';

    char ssid[64] = {0};
    char ssid_manual[64] = {0};
    char password[96] = {0};
    char location[128] = {0};
    prov_form_get_field(body, "ssid", ssid, sizeof(ssid));
    prov_form_get_field(body, "ssid_manual", ssid_manual, sizeof(ssid_manual));
    prov_form_get_field(body, "password", password, sizeof(password));
    prov_form_get_field(body, "location", location, sizeof(location));

    // "Other network…" selected → the real SSID is in the manual field, not the sentinel.
    if (strcmp(ssid, "__manual__") == 0) {
        strlcpy(ssid, ssid_manual, sizeof(ssid));
    }

    if (ssid[0] == '\0') {
        return send_result_page(req, "Network name is required",
                                "Please go back and choose or enter a Wi-Fi network.", false);
    }
    // Reject over-length credentials rather than silently truncating them into cfg
    // (a truncated SSID/passphrase would just fail to connect, confusingly).
    if (strlen(ssid) > PROV_SSID_MAX_LEN) {
        return send_result_page(req, "Network name is too long",
                                "The network name exceeds the 32-character limit.", false);
    }
    if (strlen(password) > PROV_PASS_MAX_LEN) {
        return send_result_page(req, "Password is too long",
                                "The password exceeds the 64-character limit.", false);
    }
    if (strlen(location) > PROV_LOCATION_MAX_LEN) {
        return send_result_page(req, "Location is too long",
                                "The weather location exceeds the 48-character limit.", false);
    }

    prov_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.ssid, ssid, sizeof(cfg.ssid));
    strlcpy(cfg.password, password, sizeof(cfg.password));
    strlcpy(cfg.location, location, sizeof(cfg.location));

    ESP_LOGI(TAG, "config submitted: ssid='%s', loc='%s'", cfg.ssid, cfg.location);

    char esc[PROV_SSID_MAX_LEN * 6 + 1];
    html_escape(cfg.ssid, esc, sizeof(esc));
    char msg[320];
    snprintf(msg, sizeof(msg),
             "The display is reconnecting to “%s”. You can close this page.", esc);
    // Respond before triggering the save/reboot so the browser receives the page first.
    esp_err_t rc = send_result_page(req, "Saved ✓", msg, true);

    if (s_on_save) {
        s_on_save(&cfg, s_user);
    }
    return rc;
}

// ---------------------------------------------------------------------------
// JSON API (driven by the companion mobile app over the SoftAP)
// ---------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, const char *status_line, const char *body)
{
    no_keepalive(req);
    if (status_line != NULL) {
        httpd_resp_set_status(req, status_line);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static const char *state_str(prov_portal_state_t s)
{
    switch (s) {
        case PROV_PORTAL_CONNECTING: return "connecting";
        case PROV_PORTAL_CONNECTED:  return "connected";
        case PROV_PORTAL_FAILED:     return "failed";
        case PROV_PORTAL_IDLE:
        default:                     return "idle";
    }
}

// GET /api/info — device identity so the app can register the device to the user.
static esp_err_t api_info_get(httpd_req_t *req)
{
    char body[512];
    if (prov_json_info(body, sizeof(body), s_device_id, s_model, s_ap_ssid) < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "info");
    }
    return send_json(req, NULL, body);
}

// GET /api/scan — the cached network list as JSON (real rssi/secure, unlike the HTML glyphs).
static esp_err_t api_scan_get(httpd_req_t *req)
{
    prov_ap_t aps[24];
    size_t count = prov_wifi_scan_cached(aps, sizeof(aps) / sizeof(aps[0]));
    char body[2048];
    if (prov_json_networks(aps, count, body, sizeof(body)) < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan");
    }
    ESP_LOGI(TAG, "GET /api/scan -> %u network(s)", (unsigned)count);
    return send_json(req, NULL, body);
}

// GET /api/status — the current connect-test state for the app to poll.
static esp_err_t api_status_get(httpd_req_t *req)
{
    prov_portal_state_t st = PROV_PORTAL_IDLE;
    char ssid[PROV_SSID_MAX_LEN + 1] = {0};
    char reason[32] = {0};
    if (s_status_mtx != NULL && xSemaphoreTake(s_status_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        st = s_state;
        strlcpy(ssid, s_status_ssid, sizeof(ssid));
        strlcpy(reason, s_status_reason, sizeof(reason));
        xSemaphoreGive(s_status_mtx);
    }
    char body[256];
    if (prov_json_status(body, sizeof(body), state_str(st),
                         ssid[0] ? ssid : NULL, reason[0] ? reason : NULL) < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status");
    }
    return send_json(req, NULL, body);
}

// POST /api/provision (application/x-www-form-urlencoded: ssid, ssid_manual, password, location)
// — validate, then kick off an asynchronous connect test via the on_provision callback. Replies
// 202 immediately; the app polls GET /api/status for connected/failed.
//
// The shipped companion app still POSTs the stock build's `tickers`, `finnhub_key`, `fmp_key`
// and `econ_url` fields. Unknown fields are simply not read, so those are discarded and
// onboarding keeps working unchanged against an un-updated app. The body allowance below stays
// generous for the same reason.
static esp_err_t api_provision_post(httpd_req_t *req)
{
    char body[2048];
    int blen = read_body(req, body, sizeof(body));
    if (blen == -1) {
        return send_json(req, "413 Payload Too Large", "{\"ok\":false,\"error\":\"too_large\"}");
    }
    if (blen < 0) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"read_error\"}");
    }

    char ssid[64] = {0};
    char ssid_manual[64] = {0};
    char password[96] = {0};
    char location[128] = {0};
    prov_form_get_field(body, "ssid", ssid, sizeof(ssid));
    prov_form_get_field(body, "ssid_manual", ssid_manual, sizeof(ssid_manual));
    prov_form_get_field(body, "password", password, sizeof(password));
    prov_form_get_field(body, "location", location, sizeof(location));
    // "Other network…" selected → the real SSID is in the manual field, not the sentinel.
    if (strcmp(ssid, "__manual__") == 0) {
        strlcpy(ssid, ssid_manual, sizeof(ssid));
    }

    const char *err = NULL;
    switch (prov_validate_credentials(ssid, password)) {
        case PROV_CRED_SSID_EMPTY:    err = "ssid_empty";    break;
        case PROV_CRED_SSID_TOO_LONG: err = "ssid_too_long"; break;
        case PROV_CRED_PASS_TOO_LONG: err = "pass_too_long"; break;
        case PROV_CRED_OK:                                   break;
    }
    if (err != NULL) {
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", err);
        return send_json(req, "400 Bad Request", resp);
    }

    prov_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.ssid, ssid, sizeof(cfg.ssid));
    strlcpy(cfg.password, password, sizeof(cfg.password));
    strlcpy(cfg.location, location, sizeof(cfg.location));

    ESP_LOGI(TAG, "POST /api/provision: ssid='%s' — starting connect test", cfg.ssid);
    // NOTE: status is moved to CONNECTING inside on_app_provision, only after its duplicate guard
    // accepts — setting it here would let a duplicate/retry submit clobber a terminal status.

    // Reply 202 BEFORE the (blocking, channel-hopping) connect test starts, so the phone gets
    // the response while still associated; it then polls GET /api/status for the outcome.
    esp_err_t rc = send_json(req, "202 Accepted", "{\"ok\":true,\"state\":\"connecting\"}");
    if (s_on_provision != NULL) {
        s_on_provision(&cfg, s_user);
    }
    return rc;
}

// NOTE: the captive-portal auto-popup (DNS hijack + OS-probe 302 redirects) was intentionally
// removed — the companion app drives provisioning over the JSON API at a fixed 192.168.4.1, so
// joining the SoftAP no longer triggers the "Sign in to Wi-Fi network" sheet. The browser setup
// page is still reachable directly at http://192.168.4.1/ as a fallback.

static void start_http(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;  // "/" + "/save" + 4 "/api/*"
    config.lru_purge_enable = true;
    // The captive WebView issues requests slowly and in bursts; the 5s default tears half-read
    // sockets down with recv errno 104. xiaozhi's esp-wifi-connect uses 15s for the same reason.
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/",     .method = HTTP_GET,  .handler = index_get},
        {.uri = "/save", .method = HTTP_POST, .handler = save_post},
        // JSON API the companion app drives over the SoftAP (no OS captive popup of their own).
        {.uri = "/api/info",      .method = HTTP_GET,  .handler = api_info_get},
        {.uri = "/api/scan",      .method = HTTP_GET,  .handler = api_scan_get},
        {.uri = "/api/status",    .method = HTTP_GET,  .handler = api_status_get},
        {.uri = "/api/provision", .method = HTTP_POST, .handler = api_provision_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}

// ---------------------------------------------------------------------------

void prov_portal_set_status(prov_portal_state_t state, const char *ssid, const char *reason)
{
    if (s_status_mtx == NULL) {
        return;
    }
    if (xSemaphoreTake(s_status_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_state = state;
        strlcpy(s_status_ssid, ssid ? ssid : "", sizeof(s_status_ssid));
        strlcpy(s_status_reason, reason ? reason : "", sizeof(s_status_reason));
        xSemaphoreGive(s_status_mtx);
    }
}

void prov_portal_start(const prov_config_t *current,
                       prov_portal_save_cb_t on_save,
                       prov_portal_provision_cb_t on_provision,
                       const prov_portal_info_t *info,
                       void *user)
{
    s_have_current = (current != NULL);
    if (current != NULL) {
        s_current = *current;
    }
    s_on_save = on_save;
    s_on_provision = on_provision;
    s_user = user;

    if (info != NULL) {
        strlcpy(s_device_id, info->device_id ? info->device_id : "", sizeof(s_device_id));
        strlcpy(s_model, info->model ? info->model : "", sizeof(s_model));
        strlcpy(s_ap_ssid, info->ap_ssid ? info->ap_ssid : "", sizeof(s_ap_ssid));
    }

    if (s_status_mtx == NULL) {
        s_status_mtx = xSemaphoreCreateMutex();
    }
    s_state = PROV_PORTAL_IDLE;
    s_status_ssid[0] = '\0';
    s_status_reason[0] = '\0';

    start_http();
}
