/* Failure-atomic lifecycle for the FreeRTOS objects owned by user_app. */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *user_app_handle_t;

typedef enum {
    USER_APP_RESOURCE_STATE_MUTEX = 0,
    USER_APP_RESOURCE_CATALOG_MUTEX,
    USER_APP_RESOURCE_POLL_WAKE,
    USER_APP_RESOURCE_UI_READY,
    USER_APP_RESOURCE_BUTTON_QUEUE,
    USER_APP_RESOURCE_COMMAND_QUEUE,
    USER_APP_RESOURCE_QUEUE_SET,
} user_app_resource_kind_t;

typedef enum {
    USER_APP_TASK_UI = 0,
    USER_APP_TASK_KANJI,
} user_app_task_kind_t;

typedef struct {
    user_app_handle_t state_mutex;
    user_app_handle_t catalog_mutex;
    user_app_handle_t poll_wake;
    user_app_handle_t ui_ready;
    user_app_handle_t btn_queue;
    user_app_handle_t cmd_queue;
    user_app_handle_t queue_set;
    user_app_handle_t ui_task;
    user_app_handle_t kanji_task;
} user_app_task_resources_t;

typedef enum {
    USER_APP_INIT_OK = 0,
    USER_APP_INIT_ALREADY_STARTED,
    USER_APP_INIT_BAD_ARGUMENT,
    USER_APP_INIT_RESOURCE_FAILED,
    USER_APP_INIT_QUEUE_SET_FAILED,
    USER_APP_INIT_PREPARE_FAILED,
    USER_APP_INIT_UI_TASK_FAILED,
    USER_APP_INIT_UI_READY_FAILED,
    USER_APP_INIT_KANJI_TASK_FAILED,
} user_app_init_result_t;

typedef struct {
    void *context;
    bool (*create_resource)(void *context, user_app_resource_kind_t kind,
                            user_app_handle_t *out);
    bool (*add_member)(void *context, user_app_handle_t member,
                       user_app_handle_t set);
    void (*remove_member)(void *context, user_app_handle_t member,
                          user_app_handle_t set);
    bool (*prepare)(void *context,
                    const user_app_task_resources_t *resources);
    bool (*create_task)(void *context, user_app_task_kind_t kind,
                        user_app_handle_t *out);
    bool (*wait_ui_ready)(void *context, user_app_handle_t ready);
    void (*delete_task)(void *context, user_app_handle_t task);
    void (*delete_resource)(void *context, user_app_resource_kind_t kind,
                            user_app_handle_t resource);
    void (*publish)(void *context,
                    const user_app_task_resources_t *resources);
    void (*cleanup_complete)(void *context);
} user_app_task_ops_t;

user_app_init_result_t user_app_task_lifecycle_start(
    const user_app_task_ops_t *ops,
    user_app_task_resources_t *resources);

#ifdef __cplusplus
}
#endif
