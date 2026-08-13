/* Pure geometry for the native landscape artwork screen. No LVGL or ESP-IDF. */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARTWORK_SCREEN_W 648
#define ARTWORK_SCREEN_H 480
#define ARTWORK_READING_RULES 3

typedef struct {
    int x, y, w, h;
} artwork_rect_t;

typedef struct {
    artwork_rect_t card;
    artwork_rect_t deck_spine;
    artwork_rect_t reading_frame;
    artwork_rect_t reading_text;
    int card_stride;
    int rule_y[ARTWORK_READING_RULES];
} artwork_layout_t;

/* Native-pixel geometry for the 648 x 480 monochrome tarot spread. */
const artwork_layout_t *artwork_tarot_layout(void);

#ifdef __cplusplus
}
#endif
