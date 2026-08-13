/* Native 648 x 480 daily tarot UI for the 5.83-inch e-paper panel. */
#pragma once

#include "lvgl.h"
#include "vault_model.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_artwork_create(lv_obj_t *parent);
void ui_artwork_set_data(const vault_t *v);
void ui_artwork_set_overlay(const char *title, const char *body);

#ifdef __cplusplus
}
#endif
