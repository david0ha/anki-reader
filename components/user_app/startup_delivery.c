#include "startup_delivery.h"

#include <stddef.h>

bool startup_queue_send_critical(startup_queue_send_fn send,
                                 void *context,
                                 const void *item,
                                 uint32_t wait_policy)
{
    return send != NULL && item != NULL && wait_policy != 0 &&
           send(context, item, wait_policy);
}

void startup_delivery_record_network(startup_delivery_t *delivery,
                                     bool accepted)
{
    if (delivery != NULL) {
        delivery->network_config_accepted = accepted;
    }
}

void startup_delivery_record_overlay_dismiss(startup_delivery_t *delivery,
                                             bool accepted)
{
    if (delivery != NULL) {
        delivery->overlay_dismiss_accepted = accepted;
    }
}

bool startup_delivery_api_eligible(const startup_delivery_t *delivery)
{
    return delivery != NULL && delivery->network_config_accepted &&
           delivery->overlay_dismiss_accepted;
}
