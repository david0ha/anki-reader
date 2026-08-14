#include "prov_boot_policy.h"

prov_boot_decision_t prov_boot_decide(bool forced_portal,
                                      bool have_saved_config,
                                      bool joined_saved_network)
{
    if (forced_portal) {
        return PROV_BOOT_PORTAL;
    }
    if (have_saved_config && joined_saved_network) {
        return PROV_BOOT_ONLINE;
    }
    return PROV_BOOT_OFFLINE;
}
