/* Pure boot decision for offline-first provisioning. */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROV_BOOT_OFFLINE = 0,
    PROV_BOOT_PORTAL,
    PROV_BOOT_ONLINE,
} prov_boot_decision_t;

prov_boot_decision_t prov_boot_decide(bool forced_portal,
                                      bool have_saved_config,
                                      bool joined_saved_network);

#ifdef __cplusplus
}
#endif
