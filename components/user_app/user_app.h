#pragma once

#include "prov_config.h"   /* prov_config_t — the provisioned WiFi + location */

#ifdef __cplusplus
extern "C" {
#endif

void UserApp_AppInit(void);                       /* cJSON PSRAM hooks         */
void UserApp_UiInit(void);                        /* build the fortune UI      */
void UserApp_TaskInit(const prov_config_t *cfg);  /* spawn the UI/weather tasks */

#ifdef __cplusplus
}
#endif
