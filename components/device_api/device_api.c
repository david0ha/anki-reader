#include "device_api.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "mdns.h"

#include "cJSON.h"
#include "device_api_json.h"
#include "user_app_api.h"

static const char *TAG = "device_api";

/* Sized in device_api_json.h, where the host tests can assert the worst case
 * actually fits. */
#define STATE_BUF_SZ DEVICE_API_STATE_BUF_SZ
/* Control bodies are tiny ({"screen":1}); the study URL is the largest. */
#define POST_BUF_SZ  320

// ---------------------------------------------------------------------------
// Small response helpers
// ---------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    // Friendly to a browser-based dev build (react-native-web); native RN does not need it.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    return send_json(req, NULL, "{\"ok\":true}");
}

// 400 with {"ok":false,"error":"<code>"} — the app maps `error` to a typed Esp32Error.
static esp_err_t send_err(httpd_req_t *req, const char *code)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", code);
    return send_json(req, "400 Bad Request", buf);
}

// Read the full request body into `buf` (NUL-terminated). Returns the byte count, or <0 on an
// oversize body / socket error (caller replies too_large/read_error).
static int read_body(httpd_req_t *req, char *buf, size_t bufsz)
{
    if (req->content_len > (int)bufsz - 1) {
        return -1;
    }
    int total = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf + total, remaining);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;   // transient recv timeout — retry rather than abort the body
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

// Read + parse the JSON body. On success returns the cJSON root (caller must cJSON_Delete);
// on failure returns NULL having already sent the error response.
static cJSON *parse_body(httpd_req_t *req, esp_err_t *sent)
{
    char body[POST_BUF_SZ];
    int blen = read_body(req, body, sizeof(body));
    if (blen == -1) { *sent = send_err(req, "too_large"); return NULL; }
    if (blen < 0)   { *sent = send_err(req, "read_error"); return NULL; }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) { *sent = send_err(req, "bad_json"); return NULL; }
    return root;
}

// ---------------------------------------------------------------------------
// Device identity
// ---------------------------------------------------------------------------

static void device_id(char *out /* >= 5 bytes */)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, 5, "%02X%02X", mac[4], mac[5]);
}

static void sta_ip(char *out, size_t n)
{
    out[0] = '\0';
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif == NULL) {
        return;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(nif, &ip) == ESP_OK) {
        snprintf(out, n, IPSTR, IP2STR(&ip.ip));
    }
}

// ---------------------------------------------------------------------------
// GET handlers
// ---------------------------------------------------------------------------

static esp_err_t api_info_get(httpd_req_t *req)
{
    char id[DEV_DEVID_MAXLEN], ip[DEV_IP_MAXLEN];
    device_id(id);
    sta_ip(ip, sizeof(ip));
    char body[DEVICE_API_INFO_BUF_SZ];
    if (device_api_json_info(body, sizeof(body), id, DEVICE_MODEL, DEVICE_FW, ip) < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "info");
    }
    return send_json(req, NULL, body);
}

static esp_err_t api_state_get(httpd_req_t *req)
{
    static device_state_t st;      // ~600B — too big for the httpd task's stack
    user_app_snapshot(&st);
    device_id(st.device_id);       // network identity is owned here, not by user_app
    sta_ip(st.ip, sizeof(st.ip));

    static char buf[STATE_BUF_SZ]; // the single httpd task serializes one response at a time
    if (device_api_json_state(&st, buf, sizeof(buf)) < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "state");
    }
    return send_json(req, NULL, buf);
}

// ---------------------------------------------------------------------------
// POST handlers — drive the app via the user_app_api bridge
// ---------------------------------------------------------------------------

// POST /api/refresh — no body. Poll the study source now instead of waiting out
// the interval. The panel is only refreshed if what comes back differs from what
// is already on the glass, so this is safe to call repeatedly.
static esp_err_t api_refresh_post(httpd_req_t *req)
{
    return user_app_refresh_now() ? send_ok(req) : send_err(req, "busy");
}

// POST /api/screen { screen: 0..4 } — put the board on any screen the buttons can reach
// (question, answer, 설명, 댓글, FSRS). It moves the same nav state a KEY press moves, so a
// phone cannot park the board somewhere the buttons cannot get it out of.
static esp_err_t api_screen_post(httpd_req_t *req)
{
    esp_err_t sent;
    cJSON *root = parse_body(req, &sent);
    if (root == NULL) return sent;

    esp_err_t rc;
    cJSON *sc = cJSON_GetObjectItem(root, "screen");
    if (!cJSON_IsNumber(sc)) {
        rc = send_err(req, "bad_json");
    } else {
        // The range check lives in user_app, beside the enum it checks against; a full queue
        // reports the same code, for the same reason as the URL route below.
        rc = user_app_set_screen((int)sc->valuedouble) ? send_ok(req)
                                                       : send_err(req, "screen_range");
    }
    cJSON_Delete(root);
    return rc;
}

// POST /api/study { url } — point the device at a different study proxy. Persisted to NVS and
// applied live; an empty string switches to the built-in demo card.
static esp_err_t api_study_post(httpd_req_t *req)
{
    esp_err_t sent;
    cJSON *root = parse_body(req, &sent);
    if (root == NULL) return sent;

    cJSON *url = cJSON_GetObjectItem(root, "url");
    esp_err_t rc;
    if (!cJSON_IsString(url) || url->valuestring == NULL) {
        rc = send_err(req, "bad_json");
    } else if (!user_app_set_study_url(url->valuestring)) {
        // One code for both "not a usable URL" and "queue full": the first is by
        // far the likelier, and the app cannot do anything different about the
        // second anyway.
        rc = send_err(req, "study_url_invalid");
    } else {
        rc = send_ok(req);
    }
    cJSON_Delete(root);
    return rc;
}

// POST /api/display/test — no body. Runs the panel self-test sweep (tens of seconds
// of full refreshes) on the UI task; replies as soon as it is queued, not when it finishes.
static esp_err_t api_display_test_post(httpd_req_t *req)
{
    return user_app_display_test() ? send_ok(req) : send_err(req, "busy");
}

// ---------------------------------------------------------------------------
// Server + mDNS bring-up
// ---------------------------------------------------------------------------

static void start_http(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/api/info",         .method = HTTP_GET,  .handler = api_info_get},
        {.uri = "/api/state",        .method = HTTP_GET,  .handler = api_state_get},
        {.uri = "/api/refresh",      .method = HTTP_POST, .handler = api_refresh_post},
        {.uri = "/api/screen",       .method = HTTP_POST, .handler = api_screen_post},
        {.uri = "/api/study",        .method = HTTP_POST, .handler = api_study_post},
        {.uri = "/api/display/test", .method = HTTP_POST, .handler = api_display_test_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    ESP_LOGI(TAG, "control server up on port 80");
}

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    // DEVICE_MODEL lowercased, spelled out rather than computed so grepping the
    // hostname finds this line: the AP the learner joins, the model /api/info
    // reports and the host the app resolves are deliberately one word. NOT
    // "tickerboard": that name belongs to the fortune board this project forked
    // from, whose shipped app resolves tickerboard.local, and two devices
    // answering the same discovery probe on one LAN is a support ticket nobody
    // can debug from the outside. Boards flashed before the rename still
    // advertise "obsidianboard" and need a reflash to be found.
    mdns_hostname_set("ankireader");
    mdns_instance_name_set(DEVICE_MODEL);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS advertising http://ankireader.local");
}

void device_api_start(void)
{
    start_http();
    start_mdns();
}
