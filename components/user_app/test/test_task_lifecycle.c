#include "../task_lifecycle.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        failures++; \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
    } \
} while (0)

typedef struct {
    int fail_at;
    int operation;
    int next_handle;
    int live_resources;
    int live_tasks;
    int removed_members;
    bool prepared;
    bool prepare_gate_live;
    bool prepare_gate_released;
    bool published;
    bool cleaned;
} fake_init_t;

static bool fails(fake_init_t *fake)
{
    fake->operation++;
    return fake->operation == fake->fail_at;
}

static bool create_resource(void *context, user_app_resource_kind_t kind,
                            user_app_handle_t *out)
{
    fake_init_t *fake = context;
    (void)kind;
    if (fails(fake)) {
        *out = NULL;
        return false;
    }
    fake->next_handle++;
    *out = (void *)(uintptr_t)fake->next_handle;
    fake->live_resources++;
    return true;
}

static bool add_member(void *context, user_app_handle_t member,
                       user_app_handle_t set)
{
    fake_init_t *fake = context;
    CHECK(member != NULL);
    CHECK(set != NULL);
    return !fails(fake);
}

static void remove_member(void *context, user_app_handle_t member,
                          user_app_handle_t set)
{
    fake_init_t *fake = context;
    CHECK(member != NULL);
    CHECK(set != NULL);
    fake->removed_members++;
}

static bool prepare(void *context, const user_app_task_resources_t *resources)
{
    fake_init_t *fake = context;
    CHECK(resources->cmd_queue != NULL);
    fake->prepared = true;
    if (fails(fake)) {
        return false;
    }
    fake->prepare_gate_live = true;
    return true;
}

static bool create_task(void *context, user_app_task_kind_t kind,
                        user_app_handle_t *out)
{
    fake_init_t *fake = context;
    (void)kind;
    if (fails(fake)) {
        *out = NULL;
        return false;
    }
    fake->next_handle++;
    *out = (void *)(uintptr_t)fake->next_handle;
    fake->live_tasks++;
    return true;
}

static bool wait_ui_ready(void *context, user_app_handle_t ready)
{
    fake_init_t *fake = context;
    CHECK(ready != NULL);
    return !fails(fake);
}

static void delete_task(void *context, user_app_handle_t task)
{
    fake_init_t *fake = context;
    CHECK(task != NULL);
    fake->live_tasks--;
}

static void delete_resource(void *context, user_app_resource_kind_t kind,
                            user_app_handle_t resource)
{
    fake_init_t *fake = context;
    (void)kind;
    CHECK(resource != NULL);
    fake->live_resources--;
}

static void publish(void *context, const user_app_task_resources_t *resources)
{
    fake_init_t *fake = context;
    CHECK(resources->cmd_queue != NULL);
    CHECK(fake->live_tasks == 2);
    fake->published = true;
}

static void cleanup_complete(void *context)
{
    fake_init_t *fake = context;
    if (fake->prepare_gate_live) {
        fake->prepare_gate_live = false;
        fake->prepare_gate_released = true;
    }
    fake->cleaned = true;
}

static user_app_task_ops_t fake_ops(fake_init_t *fake)
{
    user_app_task_ops_t value = {
        .context = fake,
        .create_resource = create_resource,
        .add_member = add_member,
        .remove_member = remove_member,
        .prepare = prepare,
        .create_task = create_task,
        .wait_ui_ready = wait_ui_ready,
        .delete_task = delete_task,
        .delete_resource = delete_resource,
        .publish = publish,
        .cleanup_complete = cleanup_complete,
    };
    return value;
}

static void every_init_failure_is_atomic_and_unpublished(void)
{
    enum { INIT_OPERATION_COUNT = 13 };
    for (int fail_at = 1; fail_at <= INIT_OPERATION_COUNT; fail_at++) {
        fake_init_t fake = { .fail_at = fail_at };
        user_app_task_resources_t resources;
        memset(&resources, 0xA5, sizeof(resources));
        user_app_task_ops_t init_ops = fake_ops(&fake);
        user_app_init_result_t result =
            user_app_task_lifecycle_start(&init_ops, &resources);
        CHECK(result != USER_APP_INIT_OK);
        CHECK(!fake.published);
        CHECK(fake.cleaned);
        CHECK(fake.live_tasks == 0);
        CHECK(fake.live_resources == 0);
        CHECK(!fake.prepare_gate_live);
        CHECK(fake.prepare_gate_released == (fail_at >= 11));
        CHECK(fake.removed_members == (fail_at >= 10 ? 2 :
                                       fail_at == 9 ? 1 : 0));
        CHECK(resources.state_mutex == NULL);
        CHECK(resources.cmd_queue == NULL);
        CHECK(resources.ui_task == NULL);
        CHECK(resources.kanji_task == NULL);
    }
}

static void success_publishes_only_after_both_tasks_and_ui_readiness(void)
{
    fake_init_t fake = {0};
    user_app_task_resources_t resources;
    user_app_task_ops_t init_ops = fake_ops(&fake);
    user_app_init_result_t result =
        user_app_task_lifecycle_start(&init_ops, &resources);
    CHECK(result == USER_APP_INIT_OK);
    CHECK(fake.operation == 13);
    CHECK(fake.prepared);
    CHECK(fake.published);
    CHECK(!fake.cleaned);
    CHECK(fake.live_tasks == 2);
    CHECK(fake.live_resources == 6);
    CHECK(fake.prepare_gate_live);
    CHECK(!fake.prepare_gate_released);
    CHECK(resources.ui_ready == NULL);
    CHECK(resources.cmd_queue != NULL);
}

int main(void)
{
    every_init_failure_is_atomic_and_unpublished();
    success_publishes_only_after_both_tasks_and_ui_readiness();
    printf("task lifecycle: %d failures\n", failures);
    return failures ? 1 : 0;
}
