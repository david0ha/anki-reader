/*
 * vault_mock.c — the built-in demo snapshot.
 *
 * The board is a finished object with no PC running: when no vault_url has been
 * provisioned, this is what is on the glass, with a DEMO badge in the header so
 * nobody mistakes it for their own vault.
 *
 * It is also the layout's worst case, on purpose. Real data is easy; this
 * carries long Korean titles, a four-digit counter, all four agent states, a
 * zero day in the activity chart, a full node list and an inbox that overflows
 * its display cap — so the simulator's assertions run against the widest thing
 * the pages will ever be asked to draw.
 *
 * It must stay byte-equivalent to tools/mock_vault_server.py's payload:
 * test_vault_mock.c parses that server's committed output and asserts the two
 * hash identically. That is what keeps the demo screen and the wire contract
 * from drifting apart.
 */
#include "vault_mock.h"

#include <string.h>

/* Local shorthand: every string in this file is a literal that fits, but going
 * through the same UTF-8-safe copy as the parser means the mock cannot become
 * the one path that produces a half-truncated syllable. */
#define CP(dst, src) vault_str_copy((dst), sizeof(dst), (src))

static void add_tag(vault_t *v, const char *name, int count)
{
    if (v->tag_count >= VAULT_TAGS_MAX) return;
    vault_tag_t *t = &v->tags[v->tag_count++];
    CP(t->name, name);
    t->count = count;
}

static void add_agent(vault_t *v, const char *name, agent_state_t state,
                      const char *last_run, int processed, int queued,
                      int progress, const char *note)
{
    if (v->agent_count >= VAULT_AGENTS_MAX) return;
    vault_agent_t *a = &v->agents[v->agent_count++];
    CP(a->name, name);
    a->state     = state;
    CP(a->last_run, last_run);
    a->processed = processed;
    a->queued    = queued;
    a->progress  = progress;
    CP(a->note, note);
}

static void add_node(vault_t *v, const char *title, int deg)
{
    if (v->node_count >= VAULT_NODES_MAX) return;
    vault_node_t *n = &v->nodes[v->node_count++];
    CP(n->title, title);
    n->deg = deg;
}

static void add_edge(vault_t *v, int a, int b)
{
    if (v->edge_count >= VAULT_EDGES_MAX) return;
    if (a < 0 || b < 0 || a >= v->node_count || b >= v->node_count || a == b) return;
    v->edges[v->edge_count].a = (uint8_t)a;
    v->edges[v->edge_count].b = (uint8_t)b;
    v->edge_count++;
}

static void add_recent(vault_t *v, const char *time, const char *title, int links)
{
    if (v->recent_count >= VAULT_RECENT_MAX) return;
    vault_note_t *n = &v->recent[v->recent_count++];
    CP(n->time, time);
    CP(n->title, title);
    n->links = links;
}

static void add_inbox(vault_t *v, const char *title, int age_days)
{
    if (v->inbox_count >= VAULT_INBOX_MAX) return;
    vault_inbox_t *n = &v->inbox[v->inbox_count++];
    CP(n->title, title);
    n->age_days = age_days;
}

void vault_mock(vault_t *v)
{
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->valid = true;
    v->demo  = true;

    CP(v->vault, "second-brain");
    CP(v->generated_at, "21:04");

    v->stats.notes       = 1428;
    v->stats.links       = 3910;
    v->stats.orphans     = 37;
    v->stats.tags        = 212;
    v->stats.added_today = 6;
    v->stats.added_7d    = 41;
    /* A zero day is in here deliberately: a bar chart that divides by the
     * value rather than the peak renders this as a blank column or a crash. */
    static const int daily[VAULT_DAILY_DAYS] = { 3, 9, 12, 4, 0, 7, 6 };
    memcpy(v->stats.daily, daily, sizeof(daily));

    add_tag(v, "프로젝트", 186);
    add_tag(v, "데일리",   141);
    add_tag(v, "영역",      88);
    add_tag(v, "자료",      63);
    add_tag(v, "아카이브",  41);
    add_tag(v, "회의",      29);

    add_agent(v, "indexer",    AGENT_RUNNING, "20:55", 1428,  3,  78, "새 노트 6건 임베딩 중");
    add_agent(v, "linker",     AGENT_RUNNING, "20:52",  910,  0,  41, "백링크 후보 생성 중");
    add_agent(v, "summarizer", AGENT_IDLE,    "18:30",  412,  0,  -1, "");
    add_agent(v, "archiver",   AGENT_ERROR,   "17:02",    0, 12,  -1, "볼트 잠금이 해제되지 않음");
    add_agent(v, "tagger",     AGENT_DONE,    "16:10", 1428,  0, 100, "");

    /* Degrees are descending — ui_graph relies on the producer having sorted
     * them, and the parser enforces it, so the mock models the same order. */
    add_node(v, "MOC/연구",      24);
    add_node(v, "데일리/2026",   19);
    add_node(v, "프로젝트/보드", 17);
    add_node(v, "아이디어",      14);
    add_node(v, "논문",          12);
    add_node(v, "ESP32",         11);
    add_node(v, "회의록",         9);
    add_node(v, "e-Paper",        8);
    add_node(v, "Obsidian",       7);
    add_node(v, "에이전트",       6);
    add_node(v, "독서",           5);
    add_node(v, "루틴",           4);
    add_node(v, "레시피",         3);
    add_node(v, "여행",           2);

    /* Hub-and-spoke from the top three, plus a handful of cross links so the
     * picture is not a pure star. */
    static const int8_t E[][2] = {
        {0,1},{0,2},{0,3},{0,4},{0,5},{0,6},{0,8},
        {1,2},{1,6},{1,11},{1,12},
        {2,5},{2,7},{2,9},{2,3},
        {3,4},{3,9},{3,10},
        {4,8},{4,10},
        {5,7},{5,9},
        {6,11},
        {7,8},
        {9,10},
        {11,13},{12,13},
    };
    for (size_t i = 0; i < sizeof(E) / sizeof(E[0]); i++) {
        add_edge(v, E[i][0], E[i][1]);
    }

    add_recent(v, "21:02", "주간 회고 2026-W32",        12);
    add_recent(v, "20:41", "UC8179 드라이버 정리",        4);
    add_recent(v, "19:58", "옵시디언 보드 설계",         18);
    add_recent(v, "18:12", "e-Paper 부분 갱신 실험",      6);
    add_recent(v, "16:44", "에이전트 오케스트레이션 노트", 9);
    add_recent(v, "15:20", "읽기: 세컨드 브레인",          3);
    add_recent(v, "13:05", "ESP32-S3 PSRAM 메모",          7);
    add_recent(v, "11:31", "데일리/2026-08-10",           21);

    add_inbox(v, "todo: 스펙 정리하기",         3);
    add_inbox(v, "회의록 미정리 (8/7 스탠드업)", 2);
    add_inbox(v, "읽기: e-Paper LUT 논문",       5);
    add_inbox(v, "링크 끊김 확인 — 논문 폴더",   1);
    add_inbox(v, "태그 정리: 영역 vs 자료",      9);
    add_inbox(v, "웹클리핑 정리",               12);
    add_inbox(v, "todo: 폰트 서브셋 재생성",     1);
    add_inbox(v, "아이디어 덤프 분류",           4);
    /* More than fits: the header shows the real total, the list shows what the
     * panel can hold. */
    v->inbox_total = 11;
}
