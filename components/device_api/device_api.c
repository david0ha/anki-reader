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

/* The state document is ~700 bytes with a full forecast; size with headroom. */
#define STATE_BUF_SZ 1600
/* Control bodies are tiny ({"page":1}); the location string is the largest. */
#define POST_BUF_SZ  256

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
    char body[256];
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

// POST /api/fortune/draw — no body. Draws a new omikuji (full panel refresh).
static esp_err_t api_draw_post(httpd_req_t *req)
{
    (void)req;
    return user_app_draw_fortune() ? send_ok(req) : send_err(req, "busy");
}

// POST /api/page { page: 0 = omikuji, 1 = home }
static esp_err_t api_page_post(httpd_req_t *req)
{
    esp_err_t sent;
    cJSON *root = parse_body(req, &sent);
    if (root == NULL) return sent;

    esp_err_t rc;
    cJSON *pg = cJSON_GetObjectItem(root, "page");
    if (!cJSON_IsNumber(pg)) {
        rc = send_err(req, "bad_json");
    } else {
        rc = user_app_set_page((int)pg->valuedouble) ? send_ok(req) : send_err(req, "page_range");
    }
    cJSON_Delete(root);
    return rc;
}

// POST /api/location { location } — set the weather location live (persisted to NVS; the
// device re-geocodes it via Open-Meteo). An empty string turns the weather block off.
static esp_err_t api_location_post(httpd_req_t *req)
{
    esp_err_t sent;
    cJSON *root = parse_body(req, &sent);
    if (root == NULL) return sent;

    cJSON *loc = cJSON_GetObjectItem(root, "location");
    esp_err_t rc;
    if (!cJSON_IsString(loc) || loc->valuestring == NULL) {
        rc = send_err(req, "bad_json");
    } else if (strlen(loc->valuestring) > DEV_LOCATION_MAXLEN) {
        rc = send_err(req, "location_too_long");
    } else {
        rc = user_app_set_location(loc->valuestring) ? send_ok(req) : send_err(req, "busy");
    }
    cJSON_Delete(root);
    return rc;
}

// POST /api/display/test — no body. Runs the panel self-test sweep (~10s of full
// refreshes) on the UI task; replies as soon as it is queued, not when it finishes.
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
        {.uri = "/api/fortune/draw", .method = HTTP_POST, .handler = api_draw_post},
        {.uri = "/api/page",         .method = HTTP_POST, .handler = api_page_post},
        {.uri = "/api/location",     .method = HTTP_POST, .handler = api_location_post},
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
    // Kept as "tickerboard" deliberately: app/src/lib/discovery.ts hardcodes
    // tickerboard.local as its fallback host. Renaming this without an app
    // release would make the device undiscoverable off the AP.
    mdns_hostname_set("tickerboard");
    mdns_instance_name_set(DEVICE_MODEL);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS advertising http://tickerboard.local");
}

void device_api_start(void)
{
    start_http();
    start_mdns();
}
