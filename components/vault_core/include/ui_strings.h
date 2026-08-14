/*
 * ui_strings.h — every fixed string that reaches the glass, in one file.
 *
 * Two reasons this exists rather than string literals scattered through the
 * screen files:
 *
 *   1. tools/gen_fonts.py derives the font's glyph list from this file. A label
 *      added here and forgotten in the font is impossible; a label added
 *      straight into a screen file would be a tofu box on the glass.
 *   2. It is the whole of the board's copy, so changing the voice of the UI is
 *      one diff.
 *
 * Dynamic strings — the headword, its reading, the Korean senses, the shape
 * explanation, the comments — come from the network and cannot be subset. They
 * are drawn from the FULL faces (Hangul 완성형 + JIS X 0208 kanji + kana); see
 * ui_fonts.h.
 */
#pragma once

/* --- chrome --------------------------------------------------------------- */

#define S_BRAND            "KANJIS"
#define S_BADGE_DEMO       "DEMO"
#define S_BADGE_STALE      "오래됨"
#define S_BADGE_OFFLINE    "오프라인"
#define S_NO_DATA          "데이터 없음"
#define S_WAITING          "불러오는 중..."

/* The two header stat chips, matching the web app's 연속 / 오늘 복습. */
#define S_STREAK           "연속"
#define S_REVIEWED_TODAY   "오늘"
#define S_TRACK            "TRK"

/* --- the footer key legend ------------------------------------------------
 * The three labels are supplied by kanji_nav.c per screen — a fixed legend on
 * a board whose KEY0 means 정답 on one screen and 등급 on the next is a lie
 * printed in 16 px. These are the fixed parts around them. */

#define S_KEY0             "KEY0"
#define S_KEY1             "KEY1"
#define S_KEY2             "KEY2"
#define S_BOOT             "BOOT"
#define S_KEY_REFRESH      "새로고침"
#define S_KEY_WIFI         "길게 Wi-Fi"

/* kanji_nav.c's per-screen legend, and the screen names beside them. */
#define S_HINT_REVEAL      "정답 보기"
#define S_HINT_HINT        "힌트"
#define S_HINT_DESC        "설명"
#define S_HINT_FSRS        "학습 정보"
#define S_HINT_GRADE       "등급 바꾸기"
#define S_HINT_COMMIT      "확정"
#define S_HINT_PAGE        "다음 쪽"
#define S_HINT_CLOSE       "닫기"
#define S_HINT_TAB         "다음 탭"

#define S_SCREEN_QUESTION  "문제"
#define S_SCREEN_ANSWER    "정답"
#define S_SCREEN_DESC      "설명"
#define S_SCREEN_COMMENTS  "댓글"
#define S_SCREEN_FSRS      "FSRS"

/* --- the question screen -------------------------------------------------- */

#define S_TAP_TO_REVEAL    "KEY0 을 눌러 정답 보기"
#define S_NEW_CARD         "새 카드"
#define S_LEFT_NEW         "새로 배울"
#define S_LEFT_REVIEW      "복습할"
#define S_RETRY            "다시 볼"
#define S_UNIT_CARDS       "장"

/* --- the answer screen ---------------------------------------------------- */

#define S_MEANING          "뜻"
#define S_READING          "읽기"
#define S_EXAMPLE          "예문"
#define S_GRADE_PROMPT     "이 카드, 얼마나 기억났나요?"

/* The four FSRS ratings. kanji_model.c is the single table these belong to;
 * they are repeated here only so gen_fonts.py sees them. */
#define S_GRADE_AGAIN      "다시"
#define S_GRADE_HARD       "어려움"
#define S_GRADE_GOOD       "보통"
#define S_GRADE_EASY       "쉬움"

/* --- the 설명 sheet -------------------------------------------------------- */

#define S_SHEET_DESC       "설명"
#define S_SHAPE            "글자의 유래"
#define S_HOOK_DEFAULT     "기억 힌트"
#define S_PARTS            "구성 요소"
#define S_NO_DESC          "이 카드에는 설명이 없습니다."

/* --- the 댓글 sheet -------------------------------------------------------- */

#define S_SHEET_COMMENTS   "댓글"
#define S_LIKES            "좋아요"
#define S_NO_COMMENTS      "아직 댓글이 없습니다."
#define S_COMMENT_MORE     "외"

/* --- the FSRS sheet -------------------------------------------------------
 * Three pages the learner can actually read on a 648x480 panel. The board is
 * the only place this scheduler is ever explained — the web app shows the
 * numbers but never says what they mean — so the copy is deliberately plain and
 * deliberately about THIS card, not about the algorithm in the abstract. */

#define S_SHEET_FSRS       "FSRS 복습 일정"

#define S_FSRS_P1_TITLE    "FSRS 가 뭔가요?"
#define S_FSRS_P1_BODY \
    "카드를 언제 다시 보여줄지 정하는 알고리즘입니다. 지금까지의 복습 " \
    "기록에서 이 카드의 기억 강도와 어려움을 추정하고, 기억할 확률이 " \
    "목표치(기본 90%)까지 떨어지는 날에 다시 꺼내 줍니다.\n\n" \
    "그래서 잘 외운 카드는 점점 뜸하게, 자꾸 틀리는 카드는 자주 " \
    "나옵니다. 매일 전부 보는 것보다 훨씬 적게 보고도 더 오래 남습니다."

#define S_FSRS_P2_TITLE    "세 가지 숫자"
#define S_FSRS_P2_BODY \
    "안정성 - 기억이 목표 확률까지 떨어지는 데 걸리는 날수. 맞힐수록 " \
    "길어집니다.\n\n" \
    "난이도 - 이 카드가 나에게 얼마나 까다로운지. 높을수록 간격이 " \
    "천천히 늘어납니다.\n\n" \
    "상태 - 새 카드에서 학습 중을 거쳐 복습으로 올라갑니다. 복습 중에 " \
    "다시를 누르면 다시 학습으로 내려갑니다."

#define S_FSRS_P3_TITLE    "어떤 평가를 고를까"
#define S_FSRS_P3_BODY \
    "다시 - 못 떠올렸다. 오늘 안에 또 나옵니다.\n" \
    "어려움 - 겨우 떠올렸다. 간격이 조금만 늘어납니다.\n" \
    "보통 - 떠올렸다. 기본값입니다.\n" \
    "쉬움 - 바로 떠올렸다. 간격이 크게 늘어납니다.\n\n" \
    "버튼 옆의 시간이, 그 평가를 골랐을 때 이 카드를 다시 보게 될 " \
    "시점입니다. 고민하지 말고 방금의 느낌대로 고르세요."

#define S_FSRS_THIS_CARD   "이 카드"
#define S_FSRS_STATE       "상태"
#define S_FSRS_STABILITY   "안정성"
#define S_FSRS_DIFFICULTY  "난이도"
#define S_FSRS_REPS        "복습"
#define S_FSRS_LAPSES      "실수"
#define S_FSRS_DUE         "다음"
#define S_UNIT_DAYS        "일"
#define S_UNIT_TIMES       "회"
#define S_VALUE_UNKNOWN    "—"

/* The four scheduler states, worded as kanjis-front words them. The proxy
 * sends the Korean label so the board never maps an enum it cannot see, but
 * these are here so the font carries them even if a payload is missing. */
#define S_STATE_NEW        "새 카드"
#define S_STATE_LEARNING   "학습 중"
#define S_STATE_REVIEW     "복습"
#define S_STATE_RELEARNING "다시 학습"

/* --- session states ------------------------------------------------------- */

#define S_SESSION_DONE     "오늘 학습 완료"
#define S_SESSION_DONE_SUB "다음 복습 시간에 다시 꺼내 드릴게요."
#define S_NO_CARD          "가져올 카드가 없습니다"

/* --- provisioning overlay ------------------------------------------------- */

#define S_WIFI_TITLE       "Wi-Fi 설정"
#define S_RESTARTING       "재시작 중..."

/* Every character that only ever appears in a runtime-composed string —
 * snprintf'd digits, separators, units. gen_fonts.py folds this into the face
 * verbatim.
 *
 * This constant exists because of a real bug class in the project this board
 * forked from: a Korean label rendered fine but the space in "%s %s" came out
 * as a tofu box, because a space is drawn from the label's own font and no
 * source literal happened to contain one. */
#define S_COMPOSED_CHARS   "0123456789 .,:;/%()[]-+·×↔"

/* Punctuation that arrives in DATA rather than in this file — Korean senses,
 * Japanese example sentences, comment bodies. The 완성형 set covers every
 * Hangul syllable and JIS X 0208 covers the kanji, but not the typography
 * around them, and 「」 around a Japanese quotation is not exotic: it is what
 * the catalog is written in.
 *
 * This list is curated, not derived, because there is nothing to derive it
 * from. It is the accepted limit of the font: a symbol outside 완성형, kana,
 * JIS X 0208, ASCII and this line will render as a tofu box. Both the
 * simulator and tools/kanji_server.py check every string in the payload
 * against the font, so if that ever happens it fails on a laptop with the
 * offending codepoint printed, not silently on the glass. */
#define S_DATA_PUNCT       "—–‐…“”‘’「」『』《》〈〉・·•※°→←↑↓↔×÷±≈≠≤≥§¶©®™€£¥№～〜、。〆"

/* The component forms and the notation the 설명 sheet's BODY TEXT is written in.
 *
 * A shape explanation is a decomposition: 別 = 另(다른) + 刂(칼). Neither 另 nor
 * 刂 is a JIS X 0208 kanji — a radical's combining form is its own codepoint,
 * outside the level 1/level 2 tables — so before this line existed, 3,217 of the
 * catalog's 9,956 cards had at least one character no face could draw, and the
 * proxy rewrote each to ·. That is not an edge case; it deletes the subject of
 * the sentence the sheet exists to print. 亻 alone is in 602 cards.
 *
 * Curated, not derived, for the same reason S_DATA_PUNCT is: there is no table
 * of "component forms a Korean explanation of a Japanese character cites". This
 * is the set the catalog actually uses, measured over all 9,956 cards.
 *
 * The cutoff is meaning, not frequency. These 158 glyphs measured 32 KiB of
 * .rodata across the three body faces, so rarity is not an argument anything
 * has to answer: ♥ occurs in exactly one card, and that card IS ハート, where
 * the · was the whole miss. What is left out is what a glyph could not fix —
 *
 *   - a Russian word an LLM leaked into two Korean sentences (источник),
 *   - four Hangul syllables outside 완성형 and one conjoining jamo used
 *     standalone (쎔 샾 칢 궇 ᅳ) — the board's Hangul is 완성형, and these are
 *     four members of an open set of 11,172,
 *   - two combining marks left stranded by their base character (U+0304,
 *     U+3099), which is encoding damage,
 *   - two CJK compatibility ideographs (U+F90A, U+FA66) whose NFC form is a
 *     character the faces already carry — normalization, not coverage.
 *
 * The IPA the catalog transcribes readings with ([toːi], [d͡ʑi]) is not a
 * judgement call: no shipped face has ː, ʑ or the tie bar, so it stays
 * substituted whole rather than half-drawn.
 *
 * Two limits remain, and both fail loudly rather than silently. 33 characters
 * are in NEITHER Noto Sans KR nor Noto Sans JP as this pipeline downloads them
 * (釒 in 39 cards, ∙ in 36, 兑 in 14 ...); listing one here would stop
 * gen_fonts.py in verify_face() rather than ship a hole. And 34 more are astral
 * (U+20000+, 𥫗 𠆢 𠂉 ...): LVGL's sparse cmap addresses uint16 offsets from
 * range_start, so a codepoint above 0xFFFF cannot be stored at all. Both keep
 * the proxy's substitution. */
#define S_DATA_RADICALS \
    "⺀⺅⺈⺌⺍⺕⺗⺤⺧⺮⺶⺹⺼⻂⻊⻌⻏⻖⻗⻝⼁⽇㐅㓁㔾㕣㠯䒑丂丅丨丩" \
    "丬丰丷乇乑乚乛亻亼仿俞內关兴兹冎刂匀卬叚另吳咅啇喿嚙圣坴埶壴夆夋" \
    "夌复夹娄宁尃尙尞屰巠帀开强彔忄戠戶扌敫斿昷曆步歷歺毌每氐氵氶氺灬" \
    "爫牜犭狀產畐疒礻絕緣繫罒翟耂舄艹衤覀訁說賴辵辶錄阝靑飠黃黑" \
    "«»àãèéêöāīōū‑‧⇒①②③─│♥♯〝〟ㄴㄹㆍ－３５｢｣" \
    "ｯ"
