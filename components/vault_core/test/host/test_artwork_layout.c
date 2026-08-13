#include "th.h"

#include "ui_artwork_layout.h"

static bool inside_screen(artwork_rect_t r)
{
    return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 &&
           r.x + r.w <= ARTWORK_SCREEN_W && r.y + r.h <= ARTWORK_SCREEN_H;
}

static bool overlaps(artwork_rect_t a, artwork_rect_t b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

static void test_card_is_the_hero_and_preserves_each_source_pixel(void)
{
    const artwork_layout_t *l = artwork_tarot_layout();
    CHECK(l != NULL);
    CHECK(inside_screen(l->card));

    /* The card consumes virtually the entire panel height.  Its width is
     * byte-aligned so an LVGL I1 source can be copied without scaling or
     * fractional-pixel resampling. */
    CHECK_INT(l->card.w, 272);
    CHECK_INT(l->card.h, 464);
    CHECK(l->card.h * 100 >= ARTWORK_SCREEN_H * 96);
    CHECK_INT(l->card.w % 8, 0);
    CHECK_INT(l->card_stride, 34);
}

static void test_reading_frame_uses_the_remaining_space_without_collision(void)
{
    const artwork_layout_t *l = artwork_tarot_layout();
    CHECK(inside_screen(l->reading_frame));
    CHECK(inside_screen(l->reading_text));
    CHECK(inside_screen(l->deck_spine));
    CHECK(!overlaps(l->card, l->deck_spine));
    CHECK(!overlaps(l->card, l->reading_frame));
    CHECK(!overlaps(l->deck_spine, l->reading_frame));

    CHECK(l->reading_text.x > l->reading_frame.x);
    CHECK(l->reading_text.y > l->reading_frame.y);
    CHECK(l->reading_text.x + l->reading_text.w <
          l->reading_frame.x + l->reading_frame.w);
    CHECK(l->reading_text.y + l->reading_text.h <
          l->reading_frame.y + l->reading_frame.h);

    /* Keep the reading side useful rather than leaving a decorative gutter. */
    CHECK(l->reading_frame.w >= 330);
    CHECK(l->reading_text.w >= 300);
}

static void test_section_rules_are_ordered_and_inside_the_reading_frame(void)
{
    const artwork_layout_t *l = artwork_tarot_layout();
    CHECK_INT(ARTWORK_READING_RULES, 3);
    for (int i = 0; i < ARTWORK_READING_RULES; i++) {
        const int y = l->rule_y[i];
        CHECK(y > l->reading_text.y);
        CHECK(y < l->reading_text.y + l->reading_text.h);
        if (i > 0) CHECK(y > l->rule_y[i - 1]);
    }
}

int main(void)
{
    test_card_is_the_hero_and_preserves_each_source_pixel();
    test_reading_frame_uses_the_remaining_space_without_collision();
    test_section_rules_are_ordered_and_inside_the_reading_frame();
    TH_REPORT("artwork_layout");
}
