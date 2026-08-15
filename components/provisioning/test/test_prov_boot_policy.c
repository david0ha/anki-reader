#include "tf.h"
#include "prov_boot_policy.h"

TEST(boot_policy_only_enters_the_portal_when_forced)
{
    CHECK_INT(prov_boot_decide(false, false, false), PROV_BOOT_OFFLINE);
    CHECK_INT(prov_boot_decide(true,  false, false), PROV_BOOT_PORTAL);
    CHECK_INT(prov_boot_decide(false, true,  false), PROV_BOOT_OFFLINE);
    CHECK_INT(prov_boot_decide(false, true,  true),  PROV_BOOT_ONLINE);
}
