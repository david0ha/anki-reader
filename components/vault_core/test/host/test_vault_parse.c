/*
 * Host unit tests for vault_parse.c.
 *
 * The producer of this JSON is somebody's script on their laptop. It is going
 * to send a float where an int belongs, a null where a string belongs, an
 * array of nine hundred entries, an edge pointing at a node it did not include,
 * and — the day the laptop sleeps mid-response — half a document. None of that
 * may crash the board, and none of it may replace a good screen with a blank
 * one.
 *
 * So these tests are mostly not about the happy path. The happy path is one
 * test at the top; everything after it is a way of being wrong.
 */
#include "th.h"

#include "vault_model.h"
#include "vault_parse.h"

#define PARSE(json, out) vault_parse((json), strlen(json), (out))

/* --- the happy path, from the committed contract fixture ------------------ */

static void test_fixture(void)
{
    size_t len = 0;
    char *json = th_slurp(FIXDIR "/vault.json", &len);

    vault_t v;
    memset(&v, 0xAA, sizeof(v));            /* poison: every field must be written */
    CHECK(vault_parse(json, len, &v) == true);

    CHECK(v.valid == true);
    CHECK(v.demo == false);                  /* a fetched snapshot is not the demo */
    CHECK_STR(v.vault, "second-brain");
    CHECK_STR(v.generated_at, "21:04");

    CHECK_INT(v.stats.notes, 1428);
    CHECK_INT(v.stats.links, 3910);
    CHECK_INT(v.stats.orphans, 37);
    CHECK_INT(v.stats.tags, 212);
    CHECK_INT(v.stats.added_today, 6);
    CHECK_INT(v.stats.added_7d, 41);
    const int daily[] = { 3, 9, 12, 4, 0, 7, 6 };
    for (int i = 0; i < VAULT_DAILY_DAYS; i++) CHECK_INT(v.stats.daily[i], daily[i]);

    CHECK_INT(v.tag_count, 6);
    CHECK_STR(v.tags[0].name, "프로젝트");
    CHECK_INT(v.tags[0].count, 186);

    CHECK_INT(v.agent_count, 5);
    CHECK_STR(v.agents[0].name, "indexer");
    CHECK_INT(v.agents[0].state, AGENT_RUNNING);
    CHECK_INT(v.agents[0].progress, 78);
    CHECK_STR(v.agents[0].note, "새 노트 6건 임베딩 중");
    CHECK_INT(v.agents[2].state, AGENT_IDLE);
    CHECK_INT(v.agents[2].progress, -1);     /* "no bar", not "0%" */
    CHECK_INT(v.agents[3].state, AGENT_ERROR);
    CHECK_INT(v.agents[4].state, AGENT_DONE);

    CHECK_INT(v.node_count, 14);
    CHECK_STR(v.nodes[0].title, "MOC/연구");
    CHECK_INT(v.nodes[0].deg, 24);
    CHECK_INT(v.edge_count, 27);

    CHECK_INT(v.recent_count, 8);
    CHECK_STR(v.recent[0].title, "주간 회고 2026-W32");
    CHECK_INT(v.recent[0].links, 12);

    CHECK_INT(v.inbox_count, 8);
    CHECK_INT(v.inbox_total, 11);            /* the header shows the real backlog */

    CHECK_INT(vault_running_agents(&v), 2);
    CHECK_INT(vault_link_density_x100(&v), 274);
    CHECK_INT(vault_orphan_rate_x10(&v), 26);
    CHECK_INT(vault_daily_peak(&v), 12);

    free(json);
}

/* --- rejection: *out must survive ----------------------------------------- */

/* Every rejection path is checked the same way: fill `out` with a known good
 * snapshot first, and assert it is byte-identical afterwards. That is the
 * actual product requirement — a bad poll leaves the previous dashboard on the
 * glass — and it is not something "returns false" alone guarantees. */
static void check_rejects_and_preserves(const char *label, const char *json, size_t len)
{
    size_t flen = 0;
    char *good = th_slurp(FIXDIR "/vault.json", &flen);

    vault_t v;
    CHECK(vault_parse(good, flen, &v) == true);
    uint32_t before = vault_hash(&v);

    bool ok = vault_parse(json, len, &v);
    if (ok) {
        g_fail++; g_total++;
        printf("  FAIL %s: accepted\n", label);
    } else {
        CHECK_INT(vault_hash(&v), before);
    }
    free(good);
}

static void test_rejections(void)
{
    check_rejects_and_preserves("empty", "", 0);
    check_rejects_and_preserves("not json", "<html>hi</html>", 15);
    check_rejects_and_preserves("array root", "[1,2,3]", 7);
    check_rejects_and_preserves("string root", "\"hello\"", 7);

    /* The laptop closed its lid mid-response. cJSON must not read past the
     * length it was handed. */
    size_t flen = 0;
    char *full = th_slurp(FIXDIR "/vault.json", &flen);
    check_rejects_and_preserves("truncated at half", full, flen / 2);
    check_rejects_and_preserves("truncated to 1 byte", full, 1);
    free(full);

    /* Well-formed and empty. This is what a captive-portal login page, a "{}"
     * health endpoint, or an error envelope parses down to, and replacing a
     * good dashboard with blankness is the one failure a user actually
     * notices. */
    check_rejects_and_preserves("empty object", "{}", 2);
    check_rejects_and_preserves("only a schema",
                                "{\"schema\":1,\"vault\":\"x\"}", 24);
    check_rejects_and_preserves("error envelope",
                                "{\"error\":\"unauthorized\",\"code\":401}", 34);
}

/* --- individual bad fields are NOT rejections ----------------------------- */

static void test_type_confusion_clamps(void)
{
    /* Every field here is the wrong type. The document still carries a note
     * count, so it is a usable snapshot with defaults everywhere else — the
     * alternative, rejecting the lot, would blank the board because one
     * producer wrote a string for `orphans`. */
    const char *json =
        "{\"vault\":123,"
        " \"generated_at\":null,"
        " \"stats\":{\"notes\":1000,\"links\":\"lots\",\"orphans\":null,"
        "            \"tags\":[],\"daily\":\"nope\"},"
        " \"tags\":{\"not\":\"an array\"},"
        " \"agents\":\"nope\","
        " \"graph\":42,"
        " \"recent\":[1,2,3],"
        " \"inbox\":null}";
    vault_t v;
    CHECK(PARSE(json, &v) == true);
    CHECK_STR(v.vault, "");
    CHECK_STR(v.generated_at, "");
    CHECK_INT(v.stats.notes, 1000);
    CHECK_INT(v.stats.links, 0);
    CHECK_INT(v.stats.orphans, 0);
    CHECK_INT(v.tag_count, 0);
    CHECK_INT(v.agent_count, 0);
    CHECK_INT(v.node_count, 0);
    CHECK_INT(v.edge_count, 0);
    CHECK_INT(v.recent_count, 0);       /* array of numbers: no objects to read */
    CHECK_INT(v.inbox_count, 0);
}

static void test_negative_numbers_floor_at_zero(void)
{
    /* A negative count would reach a width calculation and draw a bar to the
     * left of its own origin. */
    const char *json =
        "{\"stats\":{\"notes\":500,\"links\":-9,\"orphans\":-1,"
        "            \"daily\":[-3,-1,0,1,2,3,4]},"
        " \"tags\":[{\"name\":\"t\",\"count\":-7}],"
        " \"agents\":[{\"name\":\"a\",\"processed\":-5,\"queued\":-2,\"progress\":-9}]}";
    vault_t v;
    CHECK(PARSE(json, &v) == true);
    CHECK_INT(v.stats.links, 0);
    CHECK_INT(v.stats.orphans, 0);
    CHECK_INT(v.stats.daily[0], 0);
    CHECK_INT(v.stats.daily[6], 4);
    CHECK_INT(v.tags[0].count, 0);
    CHECK_INT(v.agents[0].processed, 0);
    CHECK_INT(v.agents[0].queued, 0);
    CHECK_INT(v.agents[0].progress, -1);   /* any negative means "no bar" */
}

static void test_progress_clamps_high(void)
{
    const char *json =
        "{\"stats\":{\"notes\":1},"
        " \"agents\":[{\"name\":\"a\",\"progress\":9999},"
        "             {\"name\":\"b\",\"progress\":0},"
        "             {\"name\":\"c\"}]}";
    vault_t v;
    CHECK(PARSE(json, &v) == true);
    CHECK_INT(v.agents[0].progress, 100);
    CHECK_INT(v.agents[1].progress, 0);    /* 0% is a real value, not "no bar" */
    CHECK_INT(v.agents[2].progress, -1);   /* absent means "no bar"            */
}

static void test_agent_state_words(void)
{
    const char *json =
        "{\"stats\":{\"notes\":1},"
        " \"agents\":[{\"name\":\"a\",\"state\":\"RUNNING\"},"
        "             {\"name\":\"b\",\"state\":\"Failed\"},"
        "             {\"name\":\"c\",\"state\":\"done\"},"
        "             {\"name\":\"d\",\"state\":\"asleep\"},"
        "             {\"name\":\"e\"}]}";
    vault_t v;
    CHECK(PARSE(json, &v) == true);
    CHECK_INT(v.agents[0].state, AGENT_RUNNING);   /* case-insensitive */
    CHECK_INT(v.agents[1].state, AGENT_ERROR);     /* "failed" is an alias */
    CHECK_INT(v.agents[2].state, AGENT_DONE);
    CHECK_INT(v.agents[3].state, AGENT_IDLE);      /* unknown -> idle, not garbage */
    CHECK_INT(v.agents[4].state, AGENT_IDLE);
}

/* --- capacity ------------------------------------------------------------- */

static void test_oversized_arrays_are_capped(void)
{
    /* Build a payload with far more of everything than the panel can show. */
    static char json[16384];
    int n = snprintf(json, sizeof(json), "{\"stats\":{\"notes\":9},\"tags\":[");
    for (int i = 0; i < 40; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s{\"name\":\"t%d\",\"count\":%d}",
                      i ? "," : "", i, 100 - i);
    }
    n += snprintf(json + n, sizeof(json) - n, "],\"agents\":[");
    for (int i = 0; i < 40; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s{\"name\":\"a%d\"}", i ? "," : "", i);
    }
    n += snprintf(json + n, sizeof(json) - n, "],\"recent\":[");
    for (int i = 0; i < 40; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s{\"title\":\"r%d\"}", i ? "," : "", i);
    }
    n += snprintf(json + n, sizeof(json) - n, "],\"inbox\":[");
    for (int i = 0; i < 40; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s{\"title\":\"i%d\"}", i ? "," : "", i);
    }
    n += snprintf(json + n, sizeof(json) - n, "],\"graph\":{\"nodes\":[");
    for (int i = 0; i < 40; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s{\"id\":%d,\"title\":\"n%d\",\"deg\":%d}",
                      i ? "," : "", i, i, 100 - i);
    }
    n += snprintf(json + n, sizeof(json) - n, "],\"edges\":[");
    for (int i = 0; i < 60; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s[%d,%d]", i ? "," : "", i % 40, (i + 1) % 40);
    }
    snprintf(json + n, sizeof(json) - n, "]}}");

    vault_t v;
    CHECK(PARSE(json, &v) == true);
    CHECK_INT(v.tag_count, VAULT_TAGS_MAX);
    CHECK_INT(v.agent_count, VAULT_AGENTS_MAX);
    CHECK_INT(v.recent_count, VAULT_RECENT_MAX);
    CHECK_INT(v.inbox_count, VAULT_INBOX_MAX);
    CHECK_INT(v.node_count, VAULT_NODES_MAX);
    CHECK(v.edge_count <= VAULT_EDGES_MAX);

    /* The inbox header must still report the truth, not the visible count. */
    CHECK_INT(v.inbox_total, 40);

    /* Dropping nodes 14..39 must drop every edge that touched them — a line
     * drawn to a node that is not on the canvas is worse than no line. */
    for (int i = 0; i < v.edge_count; i++) {
        CHECK(v.edges[i].a < v.node_count);
        CHECK(v.edges[i].b < v.node_count);
    }
}

static void test_daily_is_right_aligned(void)
{
    /* Three days sent: they are the three most recent, so they belong at the
     * right-hand end of the chart. Left-aligning them would draw last week's
     * shape and label it "today". */
    const char *json = "{\"stats\":{\"notes\":1,\"daily\":[5,6,7]}}";
    vault_t v;
    CHECK(PARSE(json, &v) == true);
    CHECK_INT(v.stats.daily[0], 0);
    CHECK_INT(v.stats.daily[3], 0);
    CHECK_INT(v.stats.daily[4], 5);
    CHECK_INT(v.stats.daily[5], 6);
    CHECK_INT(v.stats.daily[6], 7);

    /* Ten days sent: keep the last seven, drop the oldest three. */
    const char *long_json =
        "{\"stats\":{\"notes\":1,\"daily\":[1,2,3,4,5,6,7,8,9,10]}}";
    CHECK(PARSE(long_json, &v) == true);
    CHECK_INT(v.stats.daily[0], 4);
    CHECK_INT(v.stats.daily[6], 10);
}

/* --- the graph ------------------------------------------------------------ */

static void test_nodes_are_sorted_by_degree(void)
{
    /* ui_graph places by index — biggest hub at the centre — so the order is
     * load-bearing, and a producer that emits them unsorted must not change
     * the picture. */
    const char *json =
        "{\"stats\":{\"notes\":1},\"graph\":{\"nodes\":["
        "{\"id\":7,\"title\":\"small\",\"deg\":2},"
        "{\"id\":9,\"title\":\"big\",\"deg\":30},"
        "{\"id\":3,\"title\":\"mid\",\"deg\":11}],"
        "\"edges\":[[7,9],[3,9]]}}";
    vault_t v;
    CHECK(PARSE(json, &v) == true);
    CHECK_INT(v.node_count, 3);
    CHECK_STR(v.nodes[0].title, "big");
    CHECK_STR(v.nodes[1].title, "mid");
    CHECK_STR(v.nodes[2].title, "small");

    /* And the edges must have followed the sort, not kept the wire indices. */
    CHECK_INT(v.edge_count, 2);
    CHECK((v.edges[0].a == 2 && v.edges[0].b == 0) ||
          (v.edges[0].a == 0 && v.edges[0].b == 2));
    CHECK((v.edges[1].a == 1 && v.edges[1].b == 0) ||
          (v.edges[1].a == 0 && v.edges[1].b == 1));
}

static void test_bad_edges_are_dropped(void)
{
    const char *json =
        "{\"stats\":{\"notes\":1},\"graph\":{\"nodes\":["
        "{\"id\":0,\"title\":\"a\",\"deg\":3},"
        "{\"id\":1,\"title\":\"b\",\"deg\":2}],"
        "\"edges\":[[0,1],[1,0],[0,0],[0,99],[\"a\",\"b\"],[0],[0,1]]}}";
    vault_t v;
    CHECK(PARSE(json, &v) == true);
    /* [0,1] once; its mirror, the self-edge, the dangling one, the
     * string pair, the one-element array and the duplicate are all gone. */
    CHECK_INT(v.edge_count, 1);
    CHECK_INT(v.edges[0].a, 0);
    CHECK_INT(v.edges[0].b, 1);
}

static void test_sparse_wire_ids(void)
{
    /* Node ids are the producer's, not array indices: they may be sparse,
     * unordered, or huge. */
    const char *json =
        "{\"stats\":{\"notes\":1},\"graph\":{\"nodes\":["
        "{\"id\":1000,\"title\":\"a\",\"deg\":5},"
        "{\"id\":7,\"title\":\"b\",\"deg\":9}],"
        "\"edges\":[[7,1000]]}}";
    vault_t v;
    CHECK(PARSE(json, &v) == true);
    CHECK_STR(v.nodes[0].title, "b");
    CHECK_INT(v.edge_count, 1);
    CHECK_INT(v.edges[0].a, 0);
    CHECK_INT(v.edges[0].b, 1);
}

/* --- strings -------------------------------------------------------------- */

static void test_long_korean_title_truncates_on_a_boundary(void)
{
    /* VAULT_TITLE_MAX is a byte count and Hangul is three bytes a syllable, so
     * the cut lands mid-sequence unless the copy is UTF-8 aware. A half
     * syllable does not render as "the title was long" — it renders as a tofu
     * box, and can walk LVGL's decoder past the NUL. */
    static char json[1024];
    char title[512] = {0};
    for (int i = 0; i < 60; i++) strcat(title, "가");   /* 180 bytes */
    snprintf(json, sizeof(json),
             "{\"stats\":{\"notes\":1},\"recent\":[{\"title\":\"%s\"}]}", title);

    vault_t v;
    CHECK(PARSE(json, &v) == true);
    CHECK_INT(v.recent_count, 1);

    const char *t = v.recent[0].title;
    size_t len = strlen(t);
    CHECK(len < VAULT_TITLE_MAX);
    CHECK_INT(len % 3, 0);                 /* whole syllables only */
    for (size_t i = 0; i < len; i += 3) {
        CHECK(memcmp(t + i, "가", 3) == 0);
    }
}

static void test_entries_without_a_title_are_skipped(void)
{
    /* A row with no title is a row of blank space with a number beside it. */
    const char *json =
        "{\"stats\":{\"notes\":1},"
        " \"recent\":[{\"title\":\"\",\"links\":5},{\"title\":\"ok\",\"links\":2}],"
        " \"tags\":[{\"count\":9},{\"name\":\"t\",\"count\":1}],"
        " \"agents\":[{\"state\":\"running\"},{\"name\":\"a\"}]}";
    vault_t v;
    CHECK(PARSE(json, &v) == true);
    CHECK_INT(v.recent_count, 1);
    CHECK_STR(v.recent[0].title, "ok");
    CHECK_INT(v.tag_count, 1);
    CHECK_STR(v.tags[0].name, "t");
    CHECK_INT(v.agent_count, 1);
    CHECK_STR(v.agents[0].name, "a");
}

/* --- the fingerprint ------------------------------------------------------ */

static void test_hash_is_content_addressed(void)
{
    /* This is what stops the panel refreshing every five minutes forever, so
     * it gets its own tests: identical content must hash identically even when
     * the two structs were built by different code paths and had different
     * garbage in their unused array slots. */
    size_t len = 0;
    char *json = th_slurp(FIXDIR "/vault.json", &len);

    vault_t a, b;
    memset(&a, 0x00, sizeof(a));
    memset(&b, 0xFF, sizeof(b));
    CHECK(vault_parse(json, len, &a) == true);
    CHECK(vault_parse(json, len, &b) == true);
    CHECK_INT(vault_hash(&a), vault_hash(&b));

    /* And any visible change must move it. */
    b.stats.notes++;
    CHECK(vault_hash(&a) != vault_hash(&b));

    vault_t c = a;
    c.agents[0].queued++;
    CHECK(vault_hash(&a) != vault_hash(&c));

    vault_t d = a;
    d.recent[7].links++;
    CHECK(vault_hash(&a) != vault_hash(&d));

    vault_t e = a;
    e.nodes[13].deg++;
    CHECK(vault_hash(&a) != vault_hash(&e));

    free(json);
}

static void test_hash_separates_adjacent_strings(void)
{
    /* "ab" + "c" must not hash the same as "a" + "bc": without a separator
     * between fields, a note renamed from "GPU" to "GP" while the next one
     * gains a "U" would leave the panel showing the old titles forever. */
    vault_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.valid = b.valid = true;
    a.recent_count = b.recent_count = 2;
    vault_str_copy(a.recent[0].title, VAULT_TITLE_MAX, "ab");
    vault_str_copy(a.recent[1].title, VAULT_TITLE_MAX, "c");
    vault_str_copy(b.recent[0].title, VAULT_TITLE_MAX, "a");
    vault_str_copy(b.recent[1].title, VAULT_TITLE_MAX, "bc");
    CHECK(vault_hash(&a) != vault_hash(&b));
}

/* --- the UTF-8-safe copy itself ------------------------------------------- */

static void test_str_copy(void)
{
    char buf[8];

    CHECK_INT(vault_str_copy(buf, sizeof(buf), "abc"), 3);
    CHECK_STR(buf, "abc");

    /* 7 bytes of room: two 3-byte syllables fit, the third does not. */
    CHECK_INT(vault_str_copy(buf, sizeof(buf), "가나다"), 6);
    CHECK_STR(buf, "가나");

    /* A source that is itself truncated mid-sequence: drop the partial glyph
     * rather than copy a lone lead byte out. */
    CHECK_INT(vault_str_copy(buf, sizeof(buf), "가\xEA\xB0"), 3);
    CHECK_STR(buf, "가");

    CHECK_INT(vault_str_copy(buf, sizeof(buf), NULL), 0);
    CHECK_STR(buf, "");

    /* Never writes past the end, and always terminates. */
    char tiny[2];
    CHECK_INT(vault_str_copy(tiny, sizeof(tiny), "가"), 0);
    CHECK_STR(tiny, "");
    CHECK_INT(vault_str_copy(tiny, sizeof(tiny), "xy"), 1);
    CHECK_STR(tiny, "x");
}

int main(void)
{
    test_fixture();
    test_rejections();
    test_type_confusion_clamps();
    test_negative_numbers_floor_at_zero();
    test_progress_clamps_high();
    test_agent_state_words();
    test_oversized_arrays_are_capped();
    test_daily_is_right_aligned();
    test_nodes_are_sorted_by_degree();
    test_bad_edges_are_dropped();
    test_sparse_wire_ids();
    test_long_korean_title_truncates_on_a_boundary();
    test_entries_without_a_title_are_skipped();
    test_hash_is_content_addressed();
    test_hash_separates_adjacent_strings();
    test_str_copy();
    TH_REPORT("vault_parse");
}
