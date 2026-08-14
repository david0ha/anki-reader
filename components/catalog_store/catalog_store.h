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
/* The snapshot lives in PSRAM/heap. Its address remains stable across every
 * failure and is replaced only by a successful grade or reinitialization. */
const kanji_t *catalog_store_current(void);
uint16_t catalog_store_ordinal(void);

/* Decode into the separately allocated pending snapshot, durably append and
 * verify grade, then pointer-swap it into publication. Failure leaves the
 * published card address, bytes, and ordinal unchanged. */
bool catalog_store_grade(kanji_grade_t grade);

#ifdef __cplusplus
}
#endif
