/*
 * kanji_mock.h — the built-in demo card.
 *
 * Used when no kanji_url has been provisioned, so the board is a finished
 * object with no PC involved, and by the simulator as its default content.
 * Sets `demo` so the header can say so.
 */
#pragma once

#include "kanji_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Overwrite *k with the demo card. Never fails. */
void kanji_mock(kanji_t *k);

#ifdef __cplusplus
}
#endif
