/* Pure critical-queue and startup-eligibility policy. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USER_APP_CRITICAL_QUEUE_WAIT UINT32_MAX

typedef bool (*startup_queue_send_fn)(void *context, const void *item,
                                      uint32_t wait_policy);

typedef struct {
    bool network_config_accepted;
    bool overlay_dismiss_accepted;
} startup_delivery_t;

bool startup_queue_send_critical(startup_queue_send_fn send,
                                 void *context,
                                 const void *item,
                                 uint32_t wait_policy);
void startup_delivery_record_network(startup_delivery_t *delivery,
                                     bool accepted);
void startup_delivery_record_overlay_dismiss(startup_delivery_t *delivery,
                                             bool accepted);
bool startup_delivery_api_eligible(const startup_delivery_t *delivery);

#ifdef __cplusplus
}
#endif
