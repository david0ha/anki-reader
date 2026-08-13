#include "ui_artwork_layout.h"

const artwork_layout_t *artwork_tarot_layout(void)
{
    /* 272 is deliberately divisible by eight: the I1 source stride is exactly
     * 34 bytes, so every authored card pixel reaches the e-paper unchanged. */
    static const artwork_layout_t LAYOUT = {
        .card = { .x = 6, .y = 8, .w = 272, .h = 464 },
        .deck_spine = { .x = 288, .y = 20, .w = 10, .h = 440 },
        .reading_frame = { .x = 306, .y = 8, .w = 336, .h = 464 },
        .reading_text = { .x = 319, .y = 20, .w = 310, .h = 438 },
        .card_stride = 34,
        .rule_y = { 166, 268, 365 },
    };
    return &LAYOUT;
}
