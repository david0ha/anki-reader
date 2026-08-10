// Pure (host-testable) configuration model for Wi-Fi + vault-source provisioning.
// This header MUST NOT depend on ESP-IDF so it can be unit-tested on the host.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define PROV_SSID_MAX_LEN     32   // 802.11 SSID limit
#define PROV_PASS_MAX_LEN     64   // WPA2 passphrase limit
#define PROV_URL_MAX_LEN     128   // where the vault snapshot is fetched from

typedef struct {
    char ssid[PROV_SSID_MAX_LEN + 1];
    char password[PROV_PASS_MAX_LEN + 1];
    // The URL the device polls for its vault snapshot, e.g.
    // "http://macbook.local:8123/vault.json". Plain HTTP is the normal case:
    // this is a link between two machines on the user's own LAN, and requiring
    // a certificate for it would mean requiring a certificate authority.
    //
    // Empty is a supported, complete configuration — the board then renders the
    // built-in demo snapshot with a DEMO badge, which is what makes it a
    // finished object with no PC running.
    char vault_url[PROV_URL_MAX_LEN + 1];
} prov_config_t;

#ifdef __cplusplus
extern "C" {
#endif

// Result of prov_validate_credentials — mirrors the error codes the JSON API reports to the
// companion app (POST /api/provision). Kept here (pure) so the identical validation runs in
// the host tests and the firmware handler.
typedef enum {
    PROV_CRED_OK = 0,
    PROV_CRED_SSID_EMPTY,
    PROV_CRED_SSID_TOO_LONG,   // strlen(ssid) > PROV_SSID_MAX_LEN
    PROV_CRED_PASS_TOO_LONG,   // strlen(password) > PROV_PASS_MAX_LEN
} prov_cred_result_t;

// Validate submitted Wi-Fi credentials without storing them. NULL ssid/password is treated as
// empty. An empty password is allowed (open networks); only an empty SSID is rejected.
prov_cred_result_t prov_validate_credentials(const char *ssid, const char *password);

// True if `url` is something the device can actually fetch: empty (meaning "use the demo
// snapshot"), or an http:// or https:// URL with a host, within PROV_URL_MAX_LEN.
//
// Deliberately permissive about the rest — a hostname, an IP, a port, a path, a query string
// are all fine. The point is to catch the two mistakes people actually make, which are pasting
// a bare hostname with no scheme and pasting a whole file:// path.
bool prov_validate_vault_url(const char *url);

#ifdef __cplusplus
}
#endif
