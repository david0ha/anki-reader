/*
 * buttons.h — physical button input for the Seeed EE04 e-Paper board.
 *
 * Four press-to-GND buttons are exposed to the app as a FreeRTOS event queue:
 *
 *   KEY0 — EE04 side button 1  -> next page
 *   KEY1 — EE04 side button 2  -> refresh now
 *   KEY2 — EE04 side button 3  -> tap: back to page 0; 5s hold: Wi-Fi setup
 *   BOOT — the XIAO module's own button: download-mode pin at reset, a normal
 *          input afterwards
 *
 * All are active-low (internal pull-up + falling-edge interrupt) and debounced
 * in the ISR. A press posts one button_event_t to the caller-owned queue; the
 * app task decides what each button means.
 *
 * The GPIO numbers are NOT hardcoded here — they come from main/user_config.h
 * via buttons_init(), so the board's pinout lives in exactly one file.
 */
#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_KEY0 = 0,
    BUTTON_KEY1,
    BUTTON_KEY2,
    BUTTON_BOOT,
    BUTTON_COUNT
} button_id_t;

typedef struct {
    button_id_t id;
} button_event_t;

/* Configure the button GPIOs and route presses to `out_queue` (queue items must
 * be sizeof(button_event_t)). `gpios` is indexed by button_id_t; an entry < 0
 * disables that button. Call once, after the queue exists. Safe to call even if
 * the GPIO ISR service is already installed. */
void buttons_init(QueueHandle_t out_queue, const int gpios[BUTTON_COUNT]);

/* True only while the button is physically held down (active-low).
 *
 * Long presses are detected by sampling this, not by timing two edges: a
 * release generates no interrupt here, so the press event tells you when a hold
 * started but never when it ended. */
bool buttons_is_pressed(button_id_t id);

#ifdef __cplusplus
}
#endif
