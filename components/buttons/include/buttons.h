/*
 * buttons.h — physical button input for the Seeed EE04 e-Paper board.
 *
 * Four press-to-GND buttons are exposed to the app as a FreeRTOS event queue:
 *
 *   KEY0 — EE04 side button 1
 *   KEY1 — EE04 side button 2
 *   KEY2 — EE04 side button 3
 *   BOOT — the XIAO module's own button: download-mode pin at reset, a normal
 *          input afterwards
 *
 * What each one MEANS is deliberately not written here. On this board a button
 * means something different on every screen — KEY0 reveals the answer, then
 * walks the grade cursor, then pages a sheet — and the mapping lives in exactly
 * one place: components/vault_core/kanji_nav.c, which is pure logic and is
 * driven from every state by test_kanji_nav.c. A second copy of it in a driver
 * header is a copy that goes stale, which is what happened to the one this
 * paragraph replaced.
 *
 * The single exception the driver has to know about is KEY2's five-second hold,
 * because it is timed here rather than decided there — see buttons_is_pressed.
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
