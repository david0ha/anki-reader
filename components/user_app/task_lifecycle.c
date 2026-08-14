#include "task_lifecycle.h"

#include <stddef.h>
#include <string.h>

static void cleanup(const user_app_task_ops_t *ops,
                    user_app_task_resources_t *resources,
                    bool button_added,
                    bool command_added)
{
    if (resources->kanji_task != NULL) {
        ops->delete_task(ops->context, resources->kanji_task);
    }
    if (resources->ui_task != NULL) {
        ops->delete_task(ops->context, resources->ui_task);
    }
    if (command_added) {
        ops->remove_member(ops->context, resources->cmd_queue,
                           resources->queue_set);
    }
    if (button_added) {
        ops->remove_member(ops->context, resources->btn_queue,
                           resources->queue_set);
    }

#define DELETE_RESOURCE(field, kind) do { \
    if (resources->field != NULL) { \
        ops->delete_resource(ops->context, kind, resources->field); \
    } \
} while (0)
    DELETE_RESOURCE(queue_set, USER_APP_RESOURCE_QUEUE_SET);
    DELETE_RESOURCE(cmd_queue, USER_APP_RESOURCE_COMMAND_QUEUE);
    DELETE_RESOURCE(btn_queue, USER_APP_RESOURCE_BUTTON_QUEUE);
    DELETE_RESOURCE(ui_ready, USER_APP_RESOURCE_UI_READY);
    DELETE_RESOURCE(poll_wake, USER_APP_RESOURCE_POLL_WAKE);
    DELETE_RESOURCE(catalog_mutex, USER_APP_RESOURCE_CATALOG_MUTEX);
    DELETE_RESOURCE(state_mutex, USER_APP_RESOURCE_STATE_MUTEX);
#undef DELETE_RESOURCE

    memset(resources, 0, sizeof(*resources));
    if (ops->cleanup_complete != NULL) {
        ops->cleanup_complete(ops->context);
    }
}

static bool create_one(const user_app_task_ops_t *ops,
                       user_app_resource_kind_t kind,
                       user_app_handle_t *out)
{
    *out = NULL;
    return ops->create_resource(ops->context, kind, out) && *out != NULL;
}

user_app_init_result_t user_app_task_lifecycle_start(
    const user_app_task_ops_t *ops,
    user_app_task_resources_t *resources)
{
    if (ops == NULL || resources == NULL || ops->create_resource == NULL ||
        ops->add_member == NULL || ops->remove_member == NULL ||
        ops->prepare == NULL || ops->create_task == NULL ||
        ops->wait_ui_ready == NULL || ops->delete_task == NULL ||
        ops->delete_resource == NULL || ops->publish == NULL) {
        return USER_APP_INIT_BAD_ARGUMENT;
    }

    memset(resources, 0, sizeof(*resources));
    bool button_added = false;
    bool command_added = false;
    user_app_init_result_t result = USER_APP_INIT_RESOURCE_FAILED;

#define CREATE_RESOURCE(field, kind) \
    if (!create_one(ops, kind, &resources->field)) goto fail
    CREATE_RESOURCE(state_mutex, USER_APP_RESOURCE_STATE_MUTEX);
    CREATE_RESOURCE(catalog_mutex, USER_APP_RESOURCE_CATALOG_MUTEX);
    CREATE_RESOURCE(poll_wake, USER_APP_RESOURCE_POLL_WAKE);
    CREATE_RESOURCE(ui_ready, USER_APP_RESOURCE_UI_READY);
    CREATE_RESOURCE(btn_queue, USER_APP_RESOURCE_BUTTON_QUEUE);
    CREATE_RESOURCE(cmd_queue, USER_APP_RESOURCE_COMMAND_QUEUE);
    CREATE_RESOURCE(queue_set, USER_APP_RESOURCE_QUEUE_SET);
#undef CREATE_RESOURCE

    result = USER_APP_INIT_QUEUE_SET_FAILED;
    if (!ops->add_member(ops->context, resources->btn_queue,
                         resources->queue_set)) {
        goto fail;
    }
    button_added = true;
    if (!ops->add_member(ops->context, resources->cmd_queue,
                         resources->queue_set)) {
        goto fail;
    }
    command_added = true;

    result = USER_APP_INIT_PREPARE_FAILED;
    if (!ops->prepare(ops->context, resources)) {
        goto fail;
    }

    result = USER_APP_INIT_UI_TASK_FAILED;
    if (!ops->create_task(ops->context, USER_APP_TASK_UI,
                          &resources->ui_task) || resources->ui_task == NULL) {
        goto fail;
    }

    result = USER_APP_INIT_UI_READY_FAILED;
    if (!ops->wait_ui_ready(ops->context, resources->ui_ready)) {
        goto fail;
    }

    result = USER_APP_INIT_KANJI_TASK_FAILED;
    if (!ops->create_task(ops->context, USER_APP_TASK_KANJI,
                          &resources->kanji_task) ||
        resources->kanji_task == NULL) {
        goto fail;
    }

    ops->delete_resource(ops->context, USER_APP_RESOURCE_UI_READY,
                         resources->ui_ready);
    resources->ui_ready = NULL;
    ops->publish(ops->context, resources);
    return USER_APP_INIT_OK;

fail:
    cleanup(ops, resources, button_added, command_added);
    return result;
}
