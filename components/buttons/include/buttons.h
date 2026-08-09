/*
 * buttons.h — physical button input for the Seeed EE05 e-Paper board.
 *
 * Two press-to-GND buttons are exposed to the app as a FreeRTOS event queue:
 *
 *   USER (GPIO2) — the EE05's side button 1, free for the app -> BUTTON_USER
 *   BOOT (GPIO0) — the XIAO module's own button: download-mode pin at reset,
 *                  but a normal input afterwards               -> BUTTON_BOOT
 *
 * Both are active-low (internal pull-up + falling-edge interrupt) and debounced
 * in the ISR. A press posts one button_event_t to the caller-owned queue; the
 * app task decides what each button means (e.g. draw a fortune / next page).
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_USER = 0,   /* GPIO2 */
    BUTTON_BOOT = 1,   /* GPIO0 */
    BUTTON_COUNT
} button_id_t;

typedef struct {
    button_id_t id;
} button_event_t;

/* Configure the USER/BOOT GPIOs and route presses to `out_queue`
 * (queue items must be sizeof(button_event_t)). Call once after the queue
 * exists. Safe to call even if the GPIO ISR service is already installed. */
void buttons_init(QueueHandle_t out_queue);

/* True only while BOTH USER and BOOT are physically held down (active-low).
 * Lets the app detect a deliberate KEY+BOOT chord by sampling real pin state,
 * rather than guessing from the timing of two independent press events. */
bool buttons_both_pressed(void);

#ifdef __cplusplus
}
#endif
