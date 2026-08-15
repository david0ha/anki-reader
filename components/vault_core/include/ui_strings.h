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
#define S_RAIL_COMPLETE    "완료"
#define S_RAIL_EMPTY       "없음"
#define S_NO_DATA          "데이터 없음"
#define S_WAITING          "불러오는 중..."

/* The two header stat chips, matching the web app's 연속 / 오늘 복습. */
#define S_STREAK           "연속"
#define S_REVIEWED_TODAY   "오늘"
#define S_TRACK            "TRK"
#define S_BATTERY          "배터리"

/* --- the key legend --------------------------------------------------------
 * There is no footer legend strip any more: the answer face's dock IS the
 * legend, because each of its four cells prints the button that commits it. The
 * key caps name the physical controls as 1 / 2 / 3 / i. */

#define S_KEY0             "1"
#define S_KEY1             "2"
#define S_KEY2             "3"
#define S_BOOT             "i"
#define S_KEY_REFRESH      "새로고침"
#define S_KEY_WIFI         "길게 Wi-Fi"

/* kanji_nav.c's per-face legend. The four ratings a press commits are not here:
 * the dock reads them through kanji_button_grade() from kanji_model.c's own
 * table, so the glass and the state machine cannot drift apart. */
#define S_HINT_REVEAL      "정답 보기"

/* What the legend says between a grade being pressed and the next card arriving. Grading is an
 * HTTP round trip to a laptop that may be asleep, so this state is measured in seconds and the
 * board has to say something honest during it rather than appear to have ignored the press. */
#define S_HINT_WAIT        "채점 중"

#define S_SCREEN_QUESTION  "문제"
#define S_SCREEN_ANSWER    "정답"

/* --- the question screen -------------------------------------------------- */

#define S_TAP_TO_REVEAL    "정답 보기"
#define S_NO_DATA_SUB      "새로고침으로 다시 확인해 주세요."
#define S_NEW_CARD         "새 카드"
#define S_LEFT_NEW         "새"
#define S_LEFT_REVIEW      "복습"
#define S_RETRY            "다시"
#define S_UNIT_CARDS       "장"

/* --- the eyebrows ----------------------------------------------------------
 * Every block on the answer face opens with one of these over a hairline, and it is the single
 * device that does most of the work of making a dense page read as designed rather than dumped.
 *
 * They are bilingual on purpose, and Korean·Japanese rather than Korean·English: the reader is
 * a Korean speaker studying Japanese, so the second half of each label is itself a word worth
 * having seen. 成り立ち is the word a Japanese dictionary uses for exactly this section, which
 * is the kind of thing this board exists to teach incidentally.
 *
 * Set in the 16 px face with 2 px of tracking — see ui_eyebrow(). */
#define S_EB_MEANING       "뜻 · いみ"
#define S_EB_BUILD         "성립 · 成り立ち"
#define S_EB_EXAMPLE       "예문 · れいぶん"
#define S_EB_READING       "읽기 · よみ"
#define S_EB_PARTS         "구성 · つくり"
#define S_EB_MEMORY        "기억 · きおく"

/* --- the answer screen ---------------------------------------------------- */

#define S_MEANING          "뜻"
#define S_READING          "읽기"
#define S_EXAMPLE          "예문"
#define S_GRADE_PROMPT     "이 카드, 얼마나 기억났나요?"

#define S_ON_READING       "음독"
#define S_KUN_READING      "훈독"

/* The right rail's three figures. 안정 is deliberately not among them: at the backend's 0.9
 * desired retention an FSRS interval is within a percent of the stability it came from, so the
 * masthead's due span already prints that number and a rail row would print it twice. */
#define S_STAT_REPS        "반복"
#define S_STAT_LAPSES      "실패"
#define S_STAT_DIFFICULTY  "난이도"

/* The front's plate — the learner's own record with this card, which is the only thing besides
 * Japanese that the question face is allowed to show. */
#define S_PLATE_STATE      "단계"
#define S_PLATE_REPS       "반복"
#define S_PLATE_STABILITY  "안정"
#define S_PLATE_LAPSES     "실패"

/* The four FSRS ratings. kanji_model.c is the single table these belong to;
 * they are repeated here only so gen_fonts.py sees them. */
#define S_GRADE_AGAIN      "다시"
#define S_GRADE_HARD       "어려움"
#define S_GRADE_GOOD       "보통"
#define S_GRADE_EASY       "쉬움"

/* --- the units the figures are composed with -------------------------------
 * The rail's 반복 5회 and the plate's 안정 9일 are built with snprintf, so the
 * unit is a string here rather than a literal at the call site — and the em dash
 * is the whole of what an unknown figure prints. A row that vanished when the
 * proxy sent no number would leave the plate a different height on every card;
 * one that printed 0 would be a lie. */
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
#define S_WIFI_CONNECTING  "Wi-Fi 연결 중\n%s"
#define S_WIFI_CONNECTED   "Wi-Fi 연결됨\n%s"
#define S_WIFI_PORTAL      "1. Wi-Fi에 연결하세요:\n%s\n\n2. 연결을 유지한 뒤 안내된 페이지를 여세요."
#define S_WIFI_SAVED       "\"%s\" 저장됨\n%s"

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
