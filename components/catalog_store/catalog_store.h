/* ESP raw-partition adapter for the portable offline catalog and rating state. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "kanji_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize or replace the active store atomically. A failed reinitialization
 * leaves any previously available store and card unchanged. */
bool catalog_store_init(void);

bool catalog_store_available(void);
const kanji_t *catalog_store_current(void);
uint16_t catalog_store_ordinal(void);

/* Decode the next card, durably append and verify grade, then publish it.
 * Failure leaves the published card and ordinal unchanged. */
bool catalog_store_grade(kanji_grade_t grade);

#ifdef __cplusplus
}
#endif
