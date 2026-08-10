#pragma once

#include "prov_config.h"   /* prov_config_t — the provisioned WiFi + vault URL */

#ifdef __cplusplus
extern "C" {
#endif

void UserApp_AppInit(void);   /* cJSON PSRAM hooks   */
void UserApp_UiInit(void);    /* build the vault UI  */

/* Spawn the UI task and the vault poller.
 *
 * `btn_gpios` is the board's button pins in button_id_t order (KEY0, KEY1,
 * KEY2, BOOT); `btn_count` is how many are supplied. They are passed in rather
 * than included because the pinout lives in main/user_config.h, and a component
 * reaching into the application's headers is how a "portable" component stops
 * being one. */
void UserApp_TaskInit(const prov_config_t *cfg, const int *btn_gpios, int btn_count);

#ifdef __cplusplus
}
#endif
