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
    bool parsed = vault_parse(json, len, &v);
    CHECK(parsed == true);
    if (!parsed) {
        free(json);
        return;
    }

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

    CHECK(v.artwork.valid);
    CHECK_INT(v.artwork.headline_count, ARTWORK_HEADLINE_MAX);
    CHECK_STR(v.artwork.headline[0].text, "기억한 것은");
    CHECK_STR(v.artwork.definition.headword, "우연한 연결");
    CHECK_STR(v.artwork.definition.meta, "명사");
    CHECK_INT(v.artwork.definition.line_count, ARTWORK_DEFINITION_MAX);
    CHECK_STR(v.artwork.note.title, "우연한 연결");
    CHECK_STR(v.artwork.note.path, "00 Daily/2026-08-13.md");
    CHECK_INT(v.artwork.note.backlink_total, 6);
    CHECK_INT(v.artwork.note.backlink_count, ARTWORK_BACKLINKS_MAX);
    CHECK_INT(v.artwork.node_count, ARTWORK_NODES_MAX);
    CHECK_INT(v.artwork.edge_count, ARTWORK_EDGES_MAX);

    CHECK(v.daily_tarot.valid);
    CHECK_STR(v.daily_tarot.date, "2026-08-13");
    CHECK_STR(v.daily_tarot.timezone, "Asia/Seoul");
    CHECK_STR(v.daily_tarot.card_id, "major-02");
    CHECK_STR(v.daily_tarot.orientation, "upright");
    CHECK_INT(v.daily_tarot.copy_version, 1);
    CHECK_INT(v.daily_tarot.headline.line_count, 2);
    CHECK_STR(v.daily_tarot.headline.lines[0], "고요히 살피면");
    CHECK_INT(v.daily_tarot.flow.line_count, 2);
    CHECK_INT(v.daily_tarot.caution.line_count, 2);
    CHECK_INT(v.daily_tarot.action.line_count, 2);

    free(json);
}

static void test_daily_tarot_is_bounded_and_utf8_safe(void)
{
    char long_text[512] = {0};
    for (int i = 0; i < 100; i++) strcat(long_text, "가");

    char json[4096];
    snprintf(json, sizeof(json),
             "{\"schema\":3,\"artwork\":{\"headline\":[\"visible\"],"
             "\"note\":{\"title\":\"focus\"}},\"daily_tarot\":{"
             "\"date\":\"2026-08-13\",\"timezone\":\"Asia/Seoul\","
             "\"card_id\":\"wands-14\",\"orientation\":\"upright\","
             "\"copy_version\":7,"
             "\"headline\":[\"%s\",\"둘\"],"
             "\"flow\":[\"흐름\"],\"caution\":[\"주의\"],"
             "\"action\":[\"행동\"]}}",
             long_text);

    vault_t v;
    CHECK(PARSE(json, &v));
    CHECK(v.daily_tarot.valid);
    CHECK_INT(v.daily_tarot.headline.line_count, TAROT_LINES_MAX);
    CHECK_STR(v.daily_tarot.headline.lines[1], "둘");
    CHECK(strlen(v.daily_tarot.headline.lines[0]) < TAROT_LINE_MAX);
    CHECK_INT(strlen(v.daily_tarot.headline.lines[0]) % 3, 0);
}

static void test_bad_daily_tarot_is_ignored_without_rejecting_schema_three(void)
{
    static const char *BAD[] = {
        "{\"date\":\"20260813\",\"timezone\":\"Asia/Seoul\",\"card_id\":\"major-00\",\"orientation\":\"upright\",\"copy_version\":1,\"headline\":[\"x\"],\"flow\":[\"x\"],\"caution\":[\"x\"],\"action\":[\"x\"]}",
        "{\"date\":\"2026-08-13\",\"timezone\":\"UTC\",\"card_id\":\"major-00\",\"orientation\":\"upright\",\"copy_version\":1,\"headline\":[\"x\"],\"flow\":[\"x\"],\"caution\":[\"x\"],\"action\":[\"x\"]}",
        "{\"date\":\"2026-08-13\",\"timezone\":\"Asia/Seoul\",\"card_id\":\"major-22\",\"orientation\":\"upright\",\"copy_version\":1,\"headline\":[\"x\"],\"flow\":[\"x\"],\"caution\":[\"x\"],\"action\":[\"x\"]}",
        "{\"date\":\"2026-08-13\",\"timezone\":\"Asia/Seoul\",\"card_id\":\"cups-01\",\"orientation\":\"reversed\",\"copy_version\":1,\"headline\":[\"x\"],\"flow\":[\"x\"],\"caution\":[\"x\"],\"action\":[\"x\"]}",
        "{\"date\":\"2026-08-13\",\"timezone\":\"Asia/Seoul\",\"card_id\":\"cups-01\",\"orientation\":\"upright\",\"copy_version\":0,\"headline\":[\"x\"],\"flow\":[\"x\"],\"caution\":[\"x\"],\"action\":[\"x\"]}",
        "{\"date\":\"2026-08-13\",\"timezone\":\"Asia/Seoul\",\"card_id\":\"cups-01\",\"orientation\":\"upright\",\"copy_version\":1,\"headline\":\"x\",\"flow\":[\"x\"],\"caution\":[\"x\"],\"action\":[\"x\"]}",
        "{\"date\":\"2026-08-13\",\"timezone\":\"Asia/Seoul\",\"card_id\":\"cups-01\",\"orientation\":\"upright\",\"copy_version\":1,\"headline\":[\"x\",\"y\",\"z\"],\"flow\":[\"x\"],\"caution\":[\"x\"],\"action\":[\"x\"]}",
        "{\"date\":\"2026-08-13\",\"timezone\":\"Asia/Seoul\",\"card_id\":\"cups-01\",\"orientation\":\"upright\",\"copy_version\":1,\"headline\":[\"two\\nrows\"],\"flow\":[\"x\"],\"caution\":[\"x\"],\"action\":[\"x\"]}",
        "{\"date\":\"2026-08-13\",\"timezone\":\"Asia/Seoul\",\"card_id\":\"cups-01\",\"orientation\":\"upright\",\"copy_version\":1,\"headline\":[\"two\\tcolumns\"],\"flow\":[\"x\"],\"caution\":[\"x\"],\"action\":[\"x\"]}",
        "{\"date\":\"2026-08-13\",\"timezone\":\"Asia/Seoul\",\"card_id\":\"cups-01\",\"orientation\":\"upright\",\"copy_version\":1,\"headline\":[\"x\"],\"flow\":[\"x\"],\"caution\":[\"x\"]}",
    };
    char json[2048];
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        snprintf(json, sizeof(json),
                 "{\"schema\":3,\"artwork\":{\"headline\":[\"visible\"],"
                 "\"note\":{\"title\":\"focus\"}},\"daily_tarot\":%s}", BAD[i]);
        vault_t v;
        CHECK(PARSE(json, &v));
        CHECK(!v.daily_tarot.valid);
        CHECK(v.artwork.valid);
    }
}

static void test_tarot_hash_tracks_only_tarot_pixels(void)
{
    vault_t a, b;
    memset(&a, 0, sizeof(a));
    a.valid = a.daily_tarot.valid = true;
    vault_str_copy(a.daily_tarot.date, sizeof(a.daily_tarot.date), "2026-08-13");
    vault_str_copy(a.daily_tarot.timezone, sizeof(a.daily_tarot.timezone), "Asia/Seoul");
    vault_str_copy(a.daily_tarot.card_id, sizeof(a.daily_tarot.card_id), "major-02");
    vault_str_copy(a.daily_tarot.orientation, sizeof(a.daily_tarot.orientation), "upright");
    a.daily_tarot.copy_version = 1;
    a.daily_tarot.headline.line_count = 1;
    vault_str_copy(a.daily_tarot.headline.lines[0], TAROT_LINE_MAX, "오늘의 문장");
    b = a;

    b.stats.notes = 999;
    b.artwork.valid = true;
    vault_str_copy(b.artwork.note.title, sizeof(b.artwork.note.title), "unrelated");
    CHECK_INT(vault_tarot_hash(&a), vault_tarot_hash(&b));

    vault_str_copy(b.daily_tarot.card_id, sizeof(b.daily_tarot.card_id), "major-03");
    CHECK(vault_tarot_hash(&a) != vault_tarot_hash(&b));
    b = a;
    b.daily_tarot.copy_version = 2;
    CHECK_INT(vault_tarot_hash(&a), vault_tarot_hash(&b));
    b = a;
    b.demo = true;
    CHECK(vault_tarot_hash(&a) != vault_tarot_hash(&b));
    b = a;
    vault_str_copy(b.daily_tarot.headline.lines[0], TAROT_LINE_MAX, "다른 문장");
    CHECK(vault_tarot_hash(&a) != vault_tarot_hash(&b));
}

static void test_artwork_payload_is_normalized_and_clamped(void)
{
    static const char JSON[] =
        "{\"schema\":3,\"artwork\":{"
        "\"headline\":[\"작은 발견이\",\"큰 지도가 된다.\",\"headline overflow\"],"
        "\"definition\":{\"headword\":\"우연한 연결\",\"meta\":\"명사\","
          "\"lines\":[\"뜻밖의 연결은\",\"생각의 방향을 바꾼다.\",\"definition overflow\"]},"
        "\"note\":{\"title\":\"우연한 연결\",\"path\":\"00 Daily/2026-08-13.md\","
          "\"backlink_total\":1,\"backlinks\":[\"아이디어\",\"MOC/연구\",\"프로젝트/보드\",\"backlink overflow\"]},"
        "\"graph\":{"
          "\"nodes\":[{\"id\":100,\"title\":\"우연한 연결\",\"slot\":5},"
                       "{\"id\":200,\"title\":\"아이디어\",\"slot\":0},"
                       "{\"id\":300,\"title\":\"MOC/연구\",\"slot\":1},"
                       "{\"id\":400,\"title\":\"프로젝트/보드\",\"slot\":99},"
                       "{\"id\":500,\"title\":\"논문\",\"slot\":4},"
                       "{\"id\":600,\"title\":\"ESP32\",\"slot\":4},"
                       "{\"id\":700,\"title\":\"node overflow\",\"slot\":2}],"
          "\"edges\":[[100,200],[200,100],[100,100],[100,999],[\"a\",\"b\"],[100],"
                         "[1.5,200],[1e100,200],"
                         "[200,300],[300,400],[400,500],[500,600],[100,300],[100,400],"
                         "[100,500],[100,600],[200,400]]"
        "}}}";

    vault_t v;
    memset(&v, 0xAA, sizeof(v));
    CHECK(PARSE(JSON, &v));
    CHECK(v.valid);
    CHECK(v.artwork.valid);
    CHECK_INT(v.artwork.headline_count, ARTWORK_HEADLINE_MAX);
    CHECK_STR(v.artwork.headline[0].text, "작은 발견이");
    CHECK_STR(v.artwork.headline[1].text, "큰 지도가 된다.");
    CHECK_STR(v.artwork.definition.headword, "우연한 연결");
    CHECK_STR(v.artwork.definition.meta, "명사");
    CHECK_INT(v.artwork.definition.line_count, ARTWORK_DEFINITION_MAX);
    CHECK_STR(v.artwork.definition.lines[0].text, "뜻밖의 연결은");
    CHECK_STR(v.artwork.definition.lines[1].text, "생각의 방향을 바꾼다.");
    CHECK_STR(v.artwork.note.title, "우연한 연결");
    CHECK_STR(v.artwork.note.path, "00 Daily/2026-08-13.md");
    CHECK_INT(v.artwork.note.backlink_count, ARTWORK_BACKLINKS_MAX);
    CHECK_STR(v.artwork.note.backlinks[2], "프로젝트/보드");
    CHECK_INT(v.artwork.note.backlink_total, ARTWORK_BACKLINKS_MAX);
    CHECK_INT(v.artwork.node_count, ARTWORK_NODES_MAX);
    const int expected_slots[] = {5, 0, 1, 2, 4, 3};
    for (int i = 0; i < v.artwork.node_count; i++) {
        CHECK_INT(v.artwork.nodes[i].slot, expected_slots[i]);
    }
    CHECK_INT(v.artwork.edge_count, ARTWORK_EDGES_MAX);
    CHECK_INT(v.artwork.edges[0].a, 0);
    CHECK_INT(v.artwork.edges[0].b, 1);
    CHECK_INT(v.artwork.edges[1].a, 0);
    CHECK_INT(v.artwork.edges[1].b, 2);
}

static void test_artwork_requires_an_object_definition(void)
{
    static const char JSON[] =
        "{\"schema\":3,\"artwork\":{"
        "\"headline\":[\"보이는 문장\"],"
        "\"definition\":[\"schema-2 definition must be ignored\"],"
        "\"note\":{\"title\":\"선택 노트\"}}}";
    vault_t v;
    CHECK(PARSE(JSON, &v));
    CHECK_STR(v.artwork.definition.headword, "");
    CHECK_STR(v.artwork.definition.meta, "");
    CHECK_INT(v.artwork.definition.line_count, 0);
}

static void test_artwork_strings_truncate_on_utf8_boundaries(void)
{
    char long_text[512] = {0};
    for (int i = 0; i < 100; i++) strcat(long_text, "가");

    char json[4096];
    snprintf(json, sizeof(json),
             "{\"schema\":3,\"artwork\":{"
             "\"headline\":[\"%s\"],"
             "\"definition\":{\"headword\":\"%s\",\"meta\":\"%s\",\"lines\":[\"%s\"]},"
             "\"note\":{\"title\":\"%s\",\"path\":\"%s\",\"backlinks\":[\"%s\"]},"
             "\"graph\":{\"nodes\":[{\"id\":0,\"title\":\"%s\",\"slot\":0}]}}}",
             long_text, long_text, long_text, long_text,
             long_text, long_text, long_text, long_text);

    vault_t v;
    CHECK(PARSE(json, &v));
    const char *fields[] = {
        v.artwork.headline[0].text,
        v.artwork.definition.headword,
        v.artwork.definition.meta,
        v.artwork.definition.lines[0].text,
        v.artwork.note.title,
        v.artwork.note.path,
        v.artwork.note.backlinks[0],
        v.artwork.nodes[0].title,
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        CHECK(strlen(fields[i]) > 0);
        CHECK_INT(strlen(fields[i]) % 3, 0);
    }
    CHECK(strlen(v.artwork.headline[0].text) < ARTWORK_LINE_MAX);
    CHECK(strlen(v.artwork.note.path) < ARTWORK_PATH_MAX);
}

static void test_artwork_hash_tracks_every_visible_field_only(void)
{
    vault_t a, b;
    memset(&a, 0, sizeof(a));
    a.valid = a.artwork.valid = true;
    a.artwork.headline_count = 1;
    vault_str_copy(a.artwork.headline[0].text, sizeof(a.artwork.headline[0].text), "문장");
    vault_str_copy(a.artwork.definition.headword, sizeof(a.artwork.definition.headword), "용어");
    vault_str_copy(a.artwork.definition.meta, sizeof(a.artwork.definition.meta), "메타");
    a.artwork.definition.line_count = 1;
    vault_str_copy(a.artwork.definition.lines[0].text,
                   sizeof(a.artwork.definition.lines[0].text), "정의");
    vault_str_copy(a.artwork.note.title, sizeof(a.artwork.note.title), "노트");
    vault_str_copy(a.artwork.note.path, sizeof(a.artwork.note.path), "daily/note.md");
    a.artwork.note.backlink_total = 2;
    a.artwork.note.backlink_count = 1;
    vault_str_copy(a.artwork.note.backlinks[0], sizeof(a.artwork.note.backlinks[0]), "백링크");
    a.artwork.node_count = 2;
    vault_str_copy(a.artwork.nodes[0].title, sizeof(a.artwork.nodes[0].title), "노트");
    vault_str_copy(a.artwork.nodes[1].title, sizeof(a.artwork.nodes[1].title), "관련");
    a.artwork.nodes[0].slot = 0;
    a.artwork.nodes[1].slot = 1;
    a.artwork.edge_count = 1;
    a.artwork.edges[0] = (artwork_edge_t){0, 1};
    b = a;

    b.stats.notes += 100;
    b.demo = true;
    vault_str_copy(b.vault, sizeof(b.vault), "renamed legacy vault");
    vault_str_copy(b.generated_at, sizeof(b.generated_at), "23:59");
    b.agent_count = 1;
    vault_str_copy(b.agents[0].name, sizeof(b.agents[0].name), "legacy agent");
    b.recent_count = 1;
    vault_str_copy(b.recent[0].title, sizeof(b.recent[0].title), "legacy recent");
    vault_str_copy(b.artwork.headline[1].text,
                   sizeof(b.artwork.headline[1].text), "unused array slot");
    CHECK_INT(vault_artwork_hash(&a), vault_artwork_hash(&b));

    b = a; b.valid = false;
    CHECK(vault_artwork_hash(&a) != vault_artwork_hash(&b));
    b = a;
    vault_str_copy(b.artwork.headline[0].text,
                   sizeof(b.artwork.headline[0].text), "다른 문장");
    CHECK(vault_artwork_hash(&a) != vault_artwork_hash(&b));
    b = a; vault_str_copy(b.artwork.definition.headword,
                          sizeof(b.artwork.definition.headword), "다른 용어");
    CHECK(vault_artwork_hash(&a) != vault_artwork_hash(&b));
    b = a; vault_str_copy(b.artwork.definition.meta,
                          sizeof(b.artwork.definition.meta), "형용사");
    CHECK(vault_artwork_hash(&a) != vault_artwork_hash(&b));
    b = a; vault_str_copy(b.artwork.definition.lines[0].text,
                          sizeof(b.artwork.definition.lines[0].text), "다른 정의");
    CHECK(vault_artwork_hash(&a) != vault_artwork_hash(&b));
    b = a; vault_str_copy(b.artwork.note.title, sizeof(b.artwork.note.title), "다른 노트");
    CHECK(vault_artwork_hash(&a) != vault_artwork_hash(&b));
    b = a; vault_str_copy(b.artwork.note.path, sizeof(b.artwork.note.path), "other.md");
    CHECK(vault_artwork_hash(&a) != vault_artwork_hash(&b));
    b = a; b.artwork.note.backlink_total++;
    CHECK(vault_artwork_hash(&a) != vault_artwork_hash(&b));
    b = a; vault_str_copy(b.artwork.note.backlinks[0],
                          sizeof(b.artwork.note.backlinks[0]), "다른 백링크");
    CHECK(vault_artwork_hash(&a) != vault_artwork_hash(&b));
    b = a; b.artwork.nodes[1].slot = 2;
    CHECK(vault_artwork_hash(&a) != vault_artwork_hash(&b));
    b = a; b.artwork.edges[0].b = 0;
    CHECK(vault_artwork_hash(&a) != vault_artwork_hash(&b));
    /* Invalid content is not drawn: all such snapshots render the same empty
     * fallback, regardless of garbage in fields hidden behind the valid gate. */
    vault_t hidden_a = a;
    vault_t hidden_b = a;
    hidden_a.artwork.valid = false;
    hidden_b.artwork.valid = false;
    vault_str_copy(hidden_b.artwork.headline[0].text,
                   sizeof(hidden_b.artwork.headline[0].text), "hidden change");
    CHECK_INT(vault_artwork_hash(&hidden_a), vault_artwork_hash(&hidden_b));

    hidden_a.valid = false;
    hidden_a.artwork.valid = true;
    CHECK_INT(vault_artwork_hash(&hidden_a), vault_artwork_hash(&hidden_b));
}

/* --- rejection: *out must survive ----------------------------------------- */

/* Every rejection path is checked the same way: fill `out` with a known good
 * snapshot first, and assert it is byte-identical afterwards. That is the
 * actual product requirement — a bad poll leaves the previous dashboard on the
 * glass — and it is not something "returns false" alone guarantees. */
static void check_rejects_and_preserves(const char *label, const char *json, size_t len)
{
    static const char GOOD[] =
        "{\"schema\":3,\"artwork\":{\"headline\":[\"good\"],"
        "\"note\":{\"title\":\"selected\"}}}";

    vault_t v;
    CHECK(PARSE(GOOD, &v) == true);
    vault_t before = v;

    bool ok = vault_parse(json, len, &v);
    if (ok) {
        g_fail++; g_total++;
        printf("  FAIL %s: accepted\n", label);
    } else {
        CHECK_INT(memcmp(&v, &before, sizeof(v)), 0);
    }
}

static void test_parser_consumes_the_whole_bounded_buffer(void)
{
    static const char JSON[] =
        "{\"schema\":3,\"artwork\":{\"headline\":[\"visible\"],"
        "\"note\":{\"title\":\"selected\"}}}";
    char buffer[256];
    size_t json_len = strlen(JSON);

    memcpy(buffer, JSON, json_len);
    memcpy(buffer + json_len, " garbage", 8);
    check_rejects_and_preserves("trailing garbage", buffer, json_len + 8);

    memcpy(buffer, JSON, json_len);
    memcpy(buffer + json_len, "{}", 2);
    check_rejects_and_preserves("second JSON value", buffer, json_len + 2);

    memcpy(buffer, JSON, json_len);
    memcpy(buffer + json_len, " \t\r\n", 4);
    vault_t v;
    CHECK(vault_parse(buffer, json_len + 4, &v));
    CHECK_STR(v.artwork.note.title, "selected");
}

static void check_bad_utf8_rejected(const char *label,
                                    const unsigned char *bad, size_t bad_len)
{
    static const char PREFIX[] =
        "{\"schema\":3,\"artwork\":{\"headline\":[\"";
    static const char SUFFIX[] =
        "\"],\"note\":{\"title\":\"selected\"}}}";
    unsigned char buffer[256];
    size_t n = 0;
    memcpy(buffer + n, PREFIX, sizeof(PREFIX) - 1); n += sizeof(PREFIX) - 1;
    memcpy(buffer + n, bad, bad_len); n += bad_len;
    memcpy(buffer + n, SUFFIX, sizeof(SUFFIX) - 1); n += sizeof(SUFFIX) - 1;
    check_rejects_and_preserves(label, (const char *)buffer, n);
}

static void test_parser_rejects_malformed_utf8_and_raw_controls(void)
{
    static const unsigned char LONE_CONT[] = {0x80};
    static const unsigned char BAD_CONT[] = {0xE2, 0x28, 0xA1};
    static const unsigned char OVERLONG[] = {0xC0, 0xAF};
    static const unsigned char SURROGATE[] = {0xED, 0xA0, 0x80};
    static const unsigned char TOO_HIGH[] = {0xF4, 0x90, 0x80, 0x80};
    static const unsigned char INCOMPLETE[] = {0xF0, 0x90, 0x80};
    static const unsigned char RAW_CONTROL[] = {0x01};
    static const unsigned char RAW_NUL[] = {0x00};
    static const struct {
        const char *label;
        const unsigned char *bytes;
        size_t len;
    } BAD[] = {
        {"lone UTF-8 continuation", LONE_CONT, sizeof(LONE_CONT)},
        {"bad UTF-8 continuation", BAD_CONT, sizeof(BAD_CONT)},
        {"overlong UTF-8", OVERLONG, sizeof(OVERLONG)},
        {"UTF-8 surrogate", SURROGATE, sizeof(SURROGATE)},
        {"UTF-8 above U+10FFFF", TOO_HIGH, sizeof(TOO_HIGH)},
        {"incomplete UTF-8", INCOMPLETE, sizeof(INCOMPLETE)},
        {"raw JSON control", RAW_CONTROL, sizeof(RAW_CONTROL)},
        {"raw NUL", RAW_NUL, sizeof(RAW_NUL)},
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        check_bad_utf8_rejected(BAD[i].label, BAD[i].bytes, BAD[i].len);
    }

    static const unsigned char VALID_BOUNDARIES[] = {
        0xC2, 0x80,                         /* U+0080 */
        0xE0, 0xA0, 0x80,                   /* U+0800 */
        0xF0, 0x90, 0x80, 0x80,             /* U+10000 */
        0xF4, 0x8F, 0xBF, 0xBF,             /* U+10FFFF */
    };
    static const char PREFIX[] =
        "{\"schema\":3,\"artwork\":{\"headline\":[\"";
    static const char SUFFIX[] =
        "\"],\"note\":{\"title\":\"selected\"}}}";
    unsigned char buffer[256];
    size_t n = 0;
    memcpy(buffer + n, PREFIX, sizeof(PREFIX) - 1); n += sizeof(PREFIX) - 1;
    memcpy(buffer + n, VALID_BOUNDARIES, sizeof(VALID_BOUNDARIES));
    n += sizeof(VALID_BOUNDARIES);
    memcpy(buffer + n, SUFFIX, sizeof(SUFFIX) - 1); n += sizeof(SUFFIX) - 1;
    vault_t v;
    CHECK(vault_parse((const char *)buffer, n, &v));
}

static void test_artwork_requires_drawable_content(void)
{
    static const char EMPTY_ART[] =
        "{\"schema\":3,\"artwork\":{\"note\":{\"title\":\"selected\"},"
        "\"graph\":{\"nodes\":[],\"edges\":[]}}}";
    check_rejects_and_preserves("artwork without text", EMPTY_ART, strlen(EMPTY_ART));

    static const char MISSING_NOTE[] =
        "{\"schema\":3,\"artwork\":{\"headline\":[\"보이는 문장\"]}}";
    check_rejects_and_preserves("artwork without selected note", MISSING_NOTE,
                                strlen(MISSING_NOTE));

    static const char EMPTY_NOTE_TITLE[] =
        "{\"schema\":3,\"artwork\":{\"headline\":[\"보이는 문장\"],"
        "\"note\":{\"title\":\"\",\"path\":\"x.md\"}}}";
    check_rejects_and_preserves("artwork with empty note title", EMPTY_NOTE_TITLE,
                                strlen(EMPTY_NOTE_TITLE));

    vault_t v;
    static const char MINIMAL[] =
        "{\"schema\":3,\"artwork\":{\"headline\":[\"여백\"],"
        "\"note\":{\"title\":\"선택 노트\"}}}";
    CHECK(PARSE(MINIMAL, &v));
    CHECK(v.artwork.valid);

    static const char LEGACY_ONLY[] =
        "{\"schema\":1,\"stats\":{\"notes\":10},"
        "\"artwork\":{\"term\":\"old\",\"quote\":[\"portrait\"]}}";
    check_rejects_and_preserves("official legacy artwork", LEGACY_ONLY, strlen(LEGACY_ONLY));
}

static void test_only_explicit_schema_three_is_supported(void)
{
    static const char VALID_ART[] =
        "\"artwork\":{\"headline\":[\"visible\"],"
        "\"note\":{\"title\":\"selected\"}}";
    char json[512];
    const char *bad_schema[] = {"4", "2", "0", "-1", "3.5", "\"3\"", "null"};
    for (size_t i = 0; i < sizeof(bad_schema) / sizeof(bad_schema[0]); i++) {
        snprintf(json, sizeof(json), "{\"schema\":%s,%s}", bad_schema[i], VALID_ART);
        check_rejects_and_preserves("unsupported explicit schema", json, strlen(json));
    }

    static const char MISSING_SCHEMA[] =
        "{\"artwork\":{\"headline\":[\"visible\"],"
        "\"note\":{\"title\":\"focus\"}}}";
    check_rejects_and_preserves("missing mandatory schema", MISSING_SCHEMA,
                                strlen(MISSING_SCHEMA));
}

static void test_artwork_node_ids_must_be_exact_integers(void)
{
    static const char JSON[] =
        "{\"schema\":3,\"artwork\":{\"headline\":[\"visible\"],"
        "\"note\":{\"title\":\"selected\"},\"graph\":{"
        "\"nodes\":[{\"id\":1.9,\"title\":\"fractional\",\"slot\":0},"
                     "{\"id\":2,\"title\":\"focus\",\"slot\":1},"
                     "{\"id\":1e100,\"title\":\"huge\",\"slot\":2},"
                     "{\"title\":\"missing\",\"slot\":3}],"
        "\"edges\":[[1,2],[2,1e100]]}}}";

    vault_t v;
    CHECK(PARSE(JSON, &v));
    CHECK_INT(v.artwork.node_count, 1);
    CHECK_STR(v.artwork.nodes[0].title, "focus");
    CHECK_INT(v.artwork.nodes[0].slot, 0);
    CHECK_INT(v.artwork.edge_count, 0);
}

static void test_artwork_slots_preserve_unique_declared_semantics(void)
{
    static const char WITH_UNORDERED_FOCUS[] =
        "{\"schema\":3,\"artwork\":{\"headline\":[\"visible\"],"
        "\"note\":{\"title\":\"selected\"},\"graph\":{\"nodes\":["
        "{\"id\":10,\"title\":\"five\",\"slot\":5},"
        "{\"id\":20,\"title\":\"focus\",\"slot\":0},"
        "{\"id\":30,\"title\":\"two\",\"slot\":2},"
        "{\"id\":40,\"title\":\"duplicate\",\"slot\":2},"
        "{\"id\":50,\"title\":\"fractional\",\"slot\":1.5},"
        "{\"id\":60,\"title\":\"missing\"}]}}}";
    vault_t v;
    CHECK(PARSE(WITH_UNORDERED_FOCUS, &v));
    const int expected[] = {5, 0, 2, 1, 3, 4};
    for (int i = 0; i < ARTWORK_NODES_MAX; i++) {
        CHECK_INT(v.artwork.nodes[i].slot, expected[i]);
    }

    static const char WITHOUT_FOCUS[] =
        "{\"schema\":3,\"artwork\":{\"headline\":[\"visible\"],"
        "\"note\":{\"title\":\"selected\"},\"graph\":{\"nodes\":["
        "{\"id\":1,\"title\":\"first\",\"slot\":5},"
        "{\"id\":2,\"title\":\"second\",\"slot\":2},"
        "{\"id\":3,\"title\":\"invalid\",\"slot\":99}]}}}";
    CHECK(PARSE(WITHOUT_FOCUS, &v));
    CHECK_INT(v.artwork.nodes[0].slot, 5);
    CHECK_INT(v.artwork.nodes[1].slot, 2);
    CHECK_INT(v.artwork.nodes[2].slot, 0);
}

static void test_artwork_edges_are_canonical_and_order_independent(void)
{
    static const char PREFIX[] =
        "{\"schema\":3,\"artwork\":{\"headline\":[\"visible\"],"
        "\"note\":{\"title\":\"selected\"},\"graph\":{\"nodes\":["
        "{\"id\":0,\"title\":\"n0\",\"slot\":0},"
        "{\"id\":1,\"title\":\"n1\",\"slot\":1},"
        "{\"id\":2,\"title\":\"n2\",\"slot\":2},"
        "{\"id\":3,\"title\":\"n3\",\"slot\":3},"
        "{\"id\":4,\"title\":\"n4\",\"slot\":4},"
        "{\"id\":5,\"title\":\"n5\",\"slot\":5}],\"edges\":";
    static const char EDGES_A[] =
        "[[4,1],[3,1],[2,1],[5,0],[4,0],[3,0],[2,0],[1,0],[5,1],[4,2],[1,4]]";
    static const char EDGES_B[] =
        "[[0,1],[0,2],[0,3],[0,4],[0,5],[1,2],[1,3],[1,4],[1,5],[2,4]]";
    char a_json[2048], b_json[2048];
    snprintf(a_json, sizeof(a_json), "%s%s}}}", PREFIX, EDGES_A);
    snprintf(b_json, sizeof(b_json), "%s%s}}}", PREFIX, EDGES_B);

    vault_t a, b;
    CHECK(PARSE(a_json, &a));
    CHECK(PARSE(b_json, &b));
    CHECK_INT(a.artwork.edge_count, ARTWORK_EDGES_MAX);
    CHECK_INT(b.artwork.edge_count, ARTWORK_EDGES_MAX);
    CHECK_INT(vault_artwork_hash(&a), vault_artwork_hash(&b));
    for (int i = 0; i < ARTWORK_EDGES_MAX; i++) {
        CHECK_INT(a.artwork.edges[i].a, b.artwork.edges[i].a);
        CHECK_INT(a.artwork.edges[i].b, b.artwork.edges[i].b);
        CHECK(a.artwork.edges[i].a < a.artwork.edges[i].b);
        if (i > 0) {
            const artwork_edge_t *prev = &a.artwork.edges[i - 1];
            const artwork_edge_t *cur = &a.artwork.edges[i];
            CHECK(prev->a < cur->a || (prev->a == cur->a && prev->b < cur->b));
        }
    }
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

#define VALID_ART_SUFFIX \
    ",\"artwork\":{\"headline\":[\"visible\"],\"note\":{\"title\":\"focus\"}}}"

static void test_type_confusion_clamps(void)
{
    /* Every field here is the wrong type. The document still carries a note
     * count, so it is a usable snapshot with defaults everywhere else — the
     * alternative, rejecting the lot, would blank the board because one
     * producer wrote a string for `orphans`. */
    const char *json =
        "{\"schema\":3,\"vault\":123,"
        " \"generated_at\":null,"
        " \"stats\":{\"notes\":1000,\"links\":\"lots\",\"orphans\":null,"
        "            \"tags\":[],\"daily\":\"nope\"},"
        " \"tags\":{\"not\":\"an array\"},"
        " \"agents\":\"nope\","
        " \"graph\":42,"
        " \"recent\":[1,2,3],"
        " \"inbox\":null" VALID_ART_SUFFIX;
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
        "{\"schema\":3,\"stats\":{\"notes\":500,\"links\":-9,\"orphans\":-1,"
        "            \"daily\":[-3,-1,0,1,2,3,4]},"
        " \"tags\":[{\"name\":\"t\",\"count\":-7}],"
        " \"agents\":[{\"name\":\"a\",\"processed\":-5,\"queued\":-2,\"progress\":-9}]"
        VALID_ART_SUFFIX;
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
        "{\"schema\":3,\"stats\":{\"notes\":1},"
        " \"agents\":[{\"name\":\"a\",\"progress\":9999},"
        "             {\"name\":\"b\",\"progress\":0},"
        "             {\"name\":\"c\"}]" VALID_ART_SUFFIX;
    vault_t v;
    CHECK(PARSE(json, &v) == true);
    CHECK_INT(v.agents[0].progress, 100);
    CHECK_INT(v.agents[1].progress, 0);    /* 0% is a real value, not "no bar" */
    CHECK_INT(v.agents[2].progress, -1);   /* absent means "no bar"            */
}

static void test_agent_state_words(void)
{
    const char *json =
        "{\"schema\":3,\"stats\":{\"notes\":1},"
        " \"agents\":[{\"name\":\"a\",\"state\":\"RUNNING\"},"
        "             {\"name\":\"b\",\"state\":\"Failed\"},"
        "             {\"name\":\"c\",\"state\":\"done\"},"
        "             {\"name\":\"d\",\"state\":\"asleep\"},"
        "             {\"name\":\"e\"}]" VALID_ART_SUFFIX;
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
    int n = snprintf(json, sizeof(json), "{\"schema\":3,\"stats\":{\"notes\":9},\"tags\":[");
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
    snprintf(json + n, sizeof(json) - n, "]},\"artwork\":{\"headline\":[\"visible\"],"
             "\"note\":{\"title\":\"focus\"}}}");

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
    const char *json = "{\"schema\":3,\"stats\":{\"notes\":1,\"daily\":[5,6,7]}"
                       VALID_ART_SUFFIX;
    vault_t v;
    CHECK(PARSE(json, &v) == true);
    CHECK_INT(v.stats.daily[0], 0);
    CHECK_INT(v.stats.daily[3], 0);
    CHECK_INT(v.stats.daily[4], 5);
    CHECK_INT(v.stats.daily[5], 6);
    CHECK_INT(v.stats.daily[6], 7);

    /* Ten days sent: keep the last seven, drop the oldest three. */
    const char *long_json =
        "{\"schema\":3,\"stats\":{\"notes\":1,\"daily\":[1,2,3,4,5,6,7,8,9,10]}"
        VALID_ART_SUFFIX;
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
        "{\"schema\":3,\"stats\":{\"notes\":1},\"graph\":{\"nodes\":["
        "{\"id\":7,\"title\":\"small\",\"deg\":2},"
        "{\"id\":9,\"title\":\"big\",\"deg\":30},"
        "{\"id\":3,\"title\":\"mid\",\"deg\":11}],"
        "\"edges\":[[7,9],[3,9]]}" VALID_ART_SUFFIX;
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
        "{\"schema\":3,\"stats\":{\"notes\":1},\"graph\":{\"nodes\":["
        "{\"id\":0,\"title\":\"a\",\"deg\":3},"
        "{\"id\":1,\"title\":\"b\",\"deg\":2}],"
        "\"edges\":[[0,1],[1,0],[0,0],[0,99],[\"a\",\"b\"],[0],[0,1]]}"
        VALID_ART_SUFFIX;
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
        "{\"schema\":3,\"stats\":{\"notes\":1},\"graph\":{\"nodes\":["
        "{\"id\":1000,\"title\":\"a\",\"deg\":5},"
        "{\"id\":7,\"title\":\"b\",\"deg\":9}],"
        "\"edges\":[[7,1000]]}" VALID_ART_SUFFIX;
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
             "{\"schema\":3,\"stats\":{\"notes\":1},\"recent\":[{\"title\":\"%s\"}]%s",
             title, VALID_ART_SUFFIX);

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
        "{\"schema\":3,\"stats\":{\"notes\":1},"
        " \"recent\":[{\"title\":\"\",\"links\":5},{\"title\":\"ok\",\"links\":2}],"
        " \"tags\":[{\"count\":9},{\"name\":\"t\",\"count\":1}],"
        " \"agents\":[{\"state\":\"running\"},{\"name\":\"a\"}]"
        VALID_ART_SUFFIX;
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
    test_daily_tarot_is_bounded_and_utf8_safe();
    test_bad_daily_tarot_is_ignored_without_rejecting_schema_three();
    test_tarot_hash_tracks_only_tarot_pixels();
    test_artwork_payload_is_normalized_and_clamped();
    test_artwork_requires_an_object_definition();
    test_artwork_strings_truncate_on_utf8_boundaries();
    test_artwork_hash_tracks_every_visible_field_only();
    test_artwork_requires_drawable_content();
    test_only_explicit_schema_three_is_supported();
    test_artwork_node_ids_must_be_exact_integers();
    test_artwork_slots_preserve_unique_declared_semantics();
    test_artwork_edges_are_canonical_and_order_independent();
    test_parser_consumes_the_whole_bounded_buffer();
    test_parser_rejects_malformed_utf8_and_raw_controls();
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
