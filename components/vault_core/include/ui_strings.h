/*
 * ui_strings.h — every fixed string that reaches the glass, in one file.
 *
 * Two reasons this exists rather than string literals scattered through the
 * page files:
 *
 *   1. tools/gen_fonts.py derives the subset face's glyph list from this file.
 *      A label added here and forgotten in the font is impossible; a label
 *      added straight into a page file would be a tofu box on the glass.
 *   2. It is the whole of the board's copy, so changing the voice of the UI is
 *      one diff.
 *
 * Dynamic strings — note titles, tag names, agent names, the vault name — come
 * from the network and cannot be subset. They are drawn from the FULL 완성형
 * faces (ui_font_kr_16 / ui_font_kr_20); see ui_fonts.h.
 */
#pragma once

/* --- chrome --------------------------------------------------------------- */

#define S_BRAND            "OBSIDIAN"
#define S_BADGE_DEMO       "DEMO"
#define S_BADGE_STALE      "오래됨"
#define S_BADGE_OFFLINE    "오프라인"
#define S_NO_DATA          "데이터 없음"
#define S_WAITING          "불러오는 중..."

/* Footer key legend. KEY2's long press is the Wi-Fi escape hatch. */
#define S_KEY_PAGE         "KEY0 페이지"
#define S_KEY_REFRESH      "KEY1 새로고침"
#define S_KEY_WIFI         "KEY2 길게 Wi-Fi"

/* --- page 0: 통계 --------------------------------------------------------- */

#define S_PAGE_STATS       "볼트 통계"
#define S_STAT_NOTES       "노트"
#define S_STAT_LINKS       "링크"
#define S_STAT_ORPHANS     "고아"
#define S_STAT_TAGS        "태그"
#define S_ACTIVITY         "최근 7일 활동"
#define S_ADDED_TODAY      "오늘"
#define S_ADDED_WEEK       "이번 주"
#define S_TOP_TAGS         "상위 태그"
#define S_HEALTH           "볼트 상태"
#define S_LINK_DENSITY     "링크 밀도"
#define S_ORPHAN_RATE      "고아 비율"
#define S_LAST_SYNC        "마지막 동기화"
#define S_PER_NOTE         "개 / 노트"

/* --- page 1: 그래프 ------------------------------------------------------- */

#define S_PAGE_GRAPH       "링크 그래프"
#define S_GRAPH_HUBS       "허브"
#define S_GRAPH_LINKS      "링크"
#define S_GRAPH_LEGEND     "원 크기 = 연결 수"

/* --- page 2: 에이전트 ----------------------------------------------------- */

#define S_PAGE_AGENTS      "에이전트"
#define S_AGENTS_RUNNING   "실행 중"
#define S_AGENT_QUEUED     "대기"
#define S_AGENT_DONE_N     "처리"
#define S_STATE_RUNNING    "실행 중"
#define S_STATE_IDLE       "대기"
#define S_STATE_ERROR      "오류"
#define S_STATE_DONE       "완료"

/* --- page 3: 노트 --------------------------------------------------------- */

#define S_PAGE_NOTES       "최근 노트"
#define S_RECENT           "최근 수정"
#define S_INBOX            "수신함"
#define S_DAYS_SUFFIX      "일"
#define S_EMPTY_RECENT     "수정된 노트 없음"
#define S_EMPTY_INBOX      "비어 있음"

/* --- provisioning overlay ------------------------------------------------- */

#define S_WIFI_TITLE       "Wi-Fi 설정"
#define S_RESTARTING       "재시작 중..."

/* Every character that only ever appears in a runtime-composed string —
 * snprintf'd digits, separators, units. gen_fonts.py folds this into the
 * subset face verbatim; the full faces already cover it.
 *
 * This constant exists because of a real bug class in the project this board
 * forked from: a Korean label rendered fine but the space in "%s %s" came out
 * as a tofu box, because a space is drawn from the label's own font and no
 * source literal happened to contain one. */
#define S_COMPOSED_CHARS   "0123456789 .,:/%()[]-+↔·%"

/* Punctuation that arrives in DATA rather than in this file — note titles, tag
 * names, agent notes. The 완성형 set covers every Hangul syllable those can
 * contain, but not the typography around it, and an em dash in a note title is
 * not exotic: it is what a Mac inserts when you type two hyphens.
 *
 * This list is curated, not derived, because there is nothing to derive it
 * from. It is the accepted limit of the font: a symbol outside 완성형, ASCII
 * and this line will render as a tofu box. The simulator checks every string in
 * the snapshot against the font, so if that ever happens in practice it fails
 * on a laptop with the offending codepoint printed, not silently on the glass.
 */
#define S_DATA_PUNCT       "—–‐…“”‘’「」『』《》〈〉·•※°→←↑↓↔×÷±≈≠≤≥§¶©®™€£¥№"
