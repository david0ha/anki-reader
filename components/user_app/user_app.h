#pragma once

#include "prov_config.h"   /* prov_config_t — the provisioned WiFi + vault URL */
#include "startup_delivery.h"
#include "task_lifecycle.h"

#ifdef __cplusplus
extern "C" {
#endif

void UserApp_AppInit(void);   /* cJSON PSRAM hooks   */
void UserApp_UiInit(void);    /* build the vault UI  */

/* Spawn the UI task and the vault poller. Returns USER_APP_INIT_OK only after
 * every resource/task exists and UiTask has reached its command-consumer loop;
 * every failure removes partial resources and leaves all command APIs closed.
 *
 * `btn_gpios` is the board's button pins in button_id_t order (KEY0, KEY1,
 * KEY2, BOOT); `btn_count` is how many are supplied. They are passed in rather
 * than included because the pinout lives in main/user_config.h, and a component
 * reaching into the application's headers is how a "portable" component stops
 * being one. */
user_app_init_result_t UserApp_TaskInit(const prov_config_t *cfg,
                                        const int *btn_gpios,
                                        int btn_count);

/* Apply post-boot network configuration and provisioning overlays through
 * UiTask's queue. Neither call touches LVGL or the panel from its caller.
 * Calls made after TaskInit block until a saturated live queue accepts the
 * command; before the queue exists they return false immediately. Call only
 * from main/provisioning tasks, never UiTask. */
bool UserApp_SetNetworkConfig(const prov_config_t *cfg);
bool UserApp_SetOverlay(const char *title, const char *body);

#ifdef __cplusplus
}
#endif
