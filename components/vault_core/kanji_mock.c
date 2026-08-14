/*
 * kanji_mock.c — the built-in demo card.
 *
 * The board is a finished object with no PC running: when no kanji_url has been
 * provisioned, this card is what reaches the glass. It is a real catalog row
 * (会う, JLPT N5) rather than lorem ipsum, because the demo screen is also the
 * screenshot in the docs and the thing a reviewer judges the layout by — and a
 * layout judged against invented data is judged against the wrong lengths.
 *
 * It must stay byte-equivalent to tools/mock_kanji_server.py's payload:
 * test_kanji_mock.c parses that server's committed output and asserts the two
 * hash identically. That is what keeps the demo screen and the wire contract
 * from drifting apart.
 */
#include "kanji_mock.h"

#include <string.h>

/* Local shorthand: every string in this file is a literal that fits, but going
 * through the same UTF-8-safe copy as the parser means the mock cannot become
 * the one path that produces a half-truncated character. */
#define CP(dst, src) kanji_str_copy((dst), sizeof(dst), (src))

static void add_sense(kanji_card_t *c, const char *text)
{
    if (c->sense_count >= KANJI_SENSES_MAX) return;
    CP(c->senses[c->sense_count], text);
    c->sense_count++;
}

static void add_example(kanji_card_t *c, const char *text, const char *reading,
                        const char *gloss)
{
    if (c->example_count >= KANJI_EXAMPLES_MAX) return;
    kanji_example_t *e = &c->examples[c->example_count++];
    CP(e->text, text);
    CP(e->reading, reading);
    CP(e->gloss, gloss);
}

static void add_part(kanji_card_t *c, const char *glyph, const char *meaning,
                     const char *reading)
{
    if (c->part_count >= KANJI_PARTS_MAX) return;
    kanji_part_t *p = &c->parts[c->part_count++];
    CP(p->glyph, glyph);
    CP(p->meaning, meaning);
    CP(p->reading, reading);
}

static void add_comment(kanji_card_t *c, const char *author, int likes,
                        const char *body)
{
    if (c->comment_count >= KANJI_COMMENTS_MAX) return;
    kanji_comment_t *m = &c->comments[c->comment_count++];
    CP(m->author, author);
    CP(m->body, body);
    m->likes = likes;
}

void kanji_mock(kanji_t *k)
{
    if (!k) return;
    memset(k, 0, sizeof *k);
    k->valid = true;
    k->demo  = true;

    CP(k->session.deck, "JLPT N5 Vocabulary");
    CP(k->session.level, "N5");
    k->session.streak = 12;
    k->session.reviewed_today = 34;
    k->session.left_new = 7;
    k->session.left_review = 18;
    k->session.retry = 2;
    k->session.track = 35;
    k->session.track_total = 60;
    k->session.complete = false;

    kanji_card_t *card = &k->card;
    card->valid = true;
    CP(card->id, "f00c539e-23f9-4294-bee1-c642189b105f");
    CP(card->front, "会う");
    CP(card->reading, "あう");
    CP(card->level, "N5");

    add_sense(card, "만나다");
    add_sense(card, "대면하다");
    add_sense(card, "우연히 만나다");

    add_example(card, "出会う", "であう",
                "우연히 만나다");
    add_example(card, "出会い", "であい",
                "만남");

    CP(card->description,
       "会는 사람들이 모여 서로 말하고 교류하는 "
       "모습을 바탕으로 한 글자입니다. 위쪽은 "
       "모임을 나타내는 형태이고, 아래쪽은 "
       "입(말함)을 연상시키며 '사람들이 모여 "
       "말하는(만나는) 모습'에서 '만나다'라는 "
       "뜻이 생겼습니다.");
    CP(card->hook_title, "기억 힌트");
    CP(card->hook_body,
       "会는 사람들이 모여 입으로 말을 주고받는 "
       "모습을 형상화한 글자입니다. 위의 구성은 "
       "'모임'을, 아래의 모양은 '말함/입'을 "
       "나타내어 '사람들이 모인다/만난다'는 "
       "의미가 됩니다.");

    add_part(card, "会", "모이다, 만나다", "あう (훈독)");

    add_comment(card, "카나 선생", 12,
                "「会う」는 사람을 만날 때 씁니다. 우연히 "
                "마주친 경우에는 「出会う」를 더 자주 "
                "씁니다.");
    add_comment(card, "유키", 5,
                "「友達に会う」처럼 조사는 に를 씁니다. "
                "を를 쓰면 어색하게 들려요.");
    card->comment_total = 14;

    CP(card->fsrs.state, "review");
    CP(card->fsrs.state_label, "복습");
    CP(card->fsrs.due, "9일 뒤");
    card->fsrs.reps = 5;
    card->fsrs.lapses = 1;
    card->fsrs.stability_days = 9;
    card->fsrs.difficulty_pct = 47;

    CP(card->preview.span[0], "10분 뒤");
    CP(card->preview.span[1], "4일 뒤");
    CP(card->preview.span[2], "9일 뒤");
    CP(card->preview.span[3], "21일 뒤");
}
