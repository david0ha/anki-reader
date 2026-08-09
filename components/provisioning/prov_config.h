// Pure (host-testable) configuration model for Wi-Fi + weather-location provisioning.
// This header MUST NOT depend on ESP-IDF so it can be unit-tested on the host.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define PROV_SSID_MAX_LEN     32   // 802.11 SSID limit
#define PROV_PASS_MAX_LEN     64   // WPA2 passphrase limit
#define PROV_LOCATION_MAX_LEN 48   // free-text weather location ("Seoul", "Paris, FR")

typedef struct {
    char ssid[PROV_SSID_MAX_LEN + 1];
    char password[PROV_PASS_MAX_LEN + 1];
    // Free-text place the user typed for weather. The device geocodes it to a
    // coordinate (Open-Meteo) once online — the portal/AP has no internet to do
    // so itself — and shows the resolved "City, CC" as confirmation. Empty -> no
    // weather widget.
    char location[PROV_LOCATION_MAX_LEN + 1];
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

#ifdef __cplusplus
}
#endif
