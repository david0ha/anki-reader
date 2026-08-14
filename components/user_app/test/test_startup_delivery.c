#include "../startup_delivery.h"

#include <stdint.h>
#include <stdio.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        failures++; \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
    } \
} while (0)

enum {
    CMD_FILLER = 7,
    CMD_NETWORK = 11,
    CMD_OVERLAY_DISMISS = 12,
};

typedef struct {
    bool occupied;
    int slot;
    int delivered[4];
    int delivered_count;
    uint32_t observed_wait_policy;
} fake_queue_t;

static bool blocking_send(void *context, const void *item, uint32_t wait_policy)
{
    fake_queue_t *queue = context;
    queue->observed_wait_policy = wait_policy;
    if (queue->occupied) {
        if (wait_policy == 0) {
            return false;
        }
        queue->delivered[queue->delivered_count++] = queue->slot;
        queue->occupied = false;
    }
    queue->slot = *(const int *)item;
    queue->occupied = true;
    return true;
}

static bool rejecting_send(void *context, const void *item, uint32_t wait_policy)
{
    (void)context;
    (void)item;
    (void)wait_policy;
    return false;
}

static void flush(fake_queue_t *queue)
{
    if (queue->occupied) {
        queue->delivered[queue->delivered_count++] = queue->slot;
        queue->occupied = false;
    }
}

static void saturated_queue_preserves_critical_startup_order(void)
{
    fake_queue_t queue = {
        .occupied = true,
        .slot = CMD_FILLER,
    };
    startup_delivery_t delivery = {0};
    const int network = CMD_NETWORK;
    const int overlay = CMD_OVERLAY_DISMISS;

    const bool network_accepted = startup_queue_send_critical(
        blocking_send, &queue, &network, USER_APP_CRITICAL_QUEUE_WAIT);
    startup_delivery_record_network(&delivery, network_accepted);
    CHECK(network_accepted);
    CHECK(queue.observed_wait_policy == USER_APP_CRITICAL_QUEUE_WAIT);
    CHECK(!startup_delivery_api_eligible(&delivery));

    const bool overlay_accepted = startup_queue_send_critical(
        blocking_send, &queue, &overlay, USER_APP_CRITICAL_QUEUE_WAIT);
    startup_delivery_record_overlay_dismiss(&delivery, overlay_accepted);
    CHECK(overlay_accepted);
    CHECK(startup_delivery_api_eligible(&delivery));

    flush(&queue);
    CHECK(queue.delivered_count == 3);
    CHECK(queue.delivered[0] == CMD_FILLER);
    CHECK(queue.delivered[1] == CMD_NETWORK);
    CHECK(queue.delivered[2] == CMD_OVERLAY_DISMISS);
}

static void rejected_or_unavailable_queue_never_enables_the_api(void)
{
    startup_delivery_t delivery = {0};
    const int network = CMD_NETWORK;
    const int overlay = CMD_OVERLAY_DISMISS;

    bool accepted = startup_queue_send_critical(
        rejecting_send, NULL, &network, USER_APP_CRITICAL_QUEUE_WAIT);
    startup_delivery_record_network(&delivery, accepted);
    CHECK(!accepted);
    CHECK(!startup_delivery_api_eligible(&delivery));

    accepted = startup_queue_send_critical(
        NULL, NULL, &overlay, USER_APP_CRITICAL_QUEUE_WAIT);
    startup_delivery_record_overlay_dismiss(&delivery, accepted);
    CHECK(!accepted);
    CHECK(!startup_delivery_api_eligible(&delivery));
}

int main(void)
{
    saturated_queue_preserves_critical_startup_order();
    rejected_or_unavailable_queue_never_enables_the_api();

    printf("startup delivery: %d failures\n", failures);
    return failures ? 1 : 0;
}
