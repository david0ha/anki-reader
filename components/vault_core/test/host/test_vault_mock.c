/*
 * Host unit tests tying the built-in demo snapshot to the wire contract.
 *
 * vault_mock.c is what an unconfigured board shows; tools/mock_vault_server.py
 * is the reference producer, and fixtures/vault.json is its committed output.
 * Those are two hand-written descriptions of the same data, in two languages,
 * and they will drift the first time somebody edits one of them.
 *
 * So the main test here is: parse the fixture, and assert it fingerprints
 * identically to the C snapshot. If someone adds an agent to the demo screen
 * and forgets the server (or vice versa), this fails with the reason attached
 * rather than showing up as a screenshot that no longer matches the docs.
 *
 * The rest of the file checks that the demo snapshot is internally legal — it
 * is, after all, the one snapshot that never goes through the parser's
 * clamping, so nothing else would catch an out-of-range value in it.
 */
#include "th.h"

#include "vault_mock.h"
#include "vault_model.h"
#include "vault_parse.h"

static void test_mock_matches_the_wire_fixture(void)
{
    size_t len = 0;
    char *json = th_slurp(FIXDIR "/vault.json", &len);

    vault_t wire;
    CHECK(vault_parse(json, len, &wire) == true);
    free(json);

    vault_t mock;
    vault_mock(&mock);

    /* The one field that legitimately differs: `demo` is how the header knows
     * to show the DEMO badge, and a snapshot that arrived over the network is
     * by definition not the demo. Normalise it and everything else must match
     * exactly. */
    CHECK(mock.demo == true);
    CHECK(wire.demo == false);
    mock.demo = false;

    if (vault_hash(&mock) != vault_hash(&wire)) {
        g_total++; g_fail++;
        printf("  FAIL vault_mock.c and tools/mock_vault_server.py have diverged\n");
        /* Narrow it down for whoever has to fix it, rather than leaving them
         * to diff two files by eye. */
        CHECK_STR(mock.vault, wire.vault);
        CHECK_STR(mock.generated_at, wire.generated_at);
        CHECK_INT(mock.stats.notes, wire.stats.notes);
        CHECK_INT(mock.stats.links, wire.stats.links);
        CHECK_INT(mock.stats.orphans, wire.stats.orphans);
        CHECK_INT(mock.stats.tags, wire.stats.tags);
        CHECK_INT(mock.stats.added_today, wire.stats.added_today);
        CHECK_INT(mock.stats.added_7d, wire.stats.added_7d);
        for (int i = 0; i < VAULT_DAILY_DAYS; i++) {
            CHECK_INT(mock.stats.daily[i], wire.stats.daily[i]);
        }
        CHECK_INT(mock.tag_count, wire.tag_count);
        for (int i = 0; i < mock.tag_count && i < wire.tag_count; i++) {
            CHECK_STR(mock.tags[i].name, wire.tags[i].name);
            CHECK_INT(mock.tags[i].count, wire.tags[i].count);
        }
        CHECK_INT(mock.agent_count, wire.agent_count);
        for (int i = 0; i < mock.agent_count && i < wire.agent_count; i++) {
            CHECK_STR(mock.agents[i].name, wire.agents[i].name);
            CHECK_INT(mock.agents[i].state, wire.agents[i].state);
            CHECK_STR(mock.agents[i].last_run, wire.agents[i].last_run);
            CHECK_INT(mock.agents[i].processed, wire.agents[i].processed);
            CHECK_INT(mock.agents[i].queued, wire.agents[i].queued);
            CHECK_INT(mock.agents[i].progress, wire.agents[i].progress);
            CHECK_STR(mock.agents[i].note, wire.agents[i].note);
        }
        CHECK_INT(mock.node_count, wire.node_count);
        for (int i = 0; i < mock.node_count && i < wire.node_count; i++) {
            CHECK_STR(mock.nodes[i].title, wire.nodes[i].title);
            CHECK_INT(mock.nodes[i].deg, wire.nodes[i].deg);
        }
        CHECK_INT(mock.edge_count, wire.edge_count);
        for (int i = 0; i < mock.edge_count && i < wire.edge_count; i++) {
            CHECK_INT(mock.edges[i].a, wire.edges[i].a);
            CHECK_INT(mock.edges[i].b, wire.edges[i].b);
        }
        CHECK_INT(mock.recent_count, wire.recent_count);
        for (int i = 0; i < mock.recent_count && i < wire.recent_count; i++) {
            CHECK_STR(mock.recent[i].title, wire.recent[i].title);
            CHECK_STR(mock.recent[i].time, wire.recent[i].time);
            CHECK_INT(mock.recent[i].links, wire.recent[i].links);
        }
        CHECK_INT(mock.inbox_count, wire.inbox_count);
        CHECK_INT(mock.inbox_total, wire.inbox_total);
        for (int i = 0; i < mock.inbox_count && i < wire.inbox_count; i++) {
            CHECK_STR(mock.inbox[i].title, wire.inbox[i].title);
            CHECK_INT(mock.inbox[i].age_days, wire.inbox[i].age_days);
        }
    } else {
        g_total++;
    }
}

static void test_mock_is_internally_legal(void)
{
    vault_t v;
    vault_mock(&v);

    CHECK(v.valid == true);
    CHECK(v.tag_count    >= 0 && v.tag_count    <= VAULT_TAGS_MAX);
    CHECK(v.agent_count  >= 0 && v.agent_count  <= VAULT_AGENTS_MAX);
    CHECK(v.node_count   >= 0 && v.node_count   <= VAULT_NODES_MAX);
    CHECK(v.edge_count   >= 0 && v.edge_count   <= VAULT_EDGES_MAX);
    CHECK(v.recent_count >= 0 && v.recent_count <= VAULT_RECENT_MAX);
    CHECK(v.inbox_count  >= 0 && v.inbox_count  <= VAULT_INBOX_MAX);

    /* An edge into a node that does not exist would index past the position
     * array in the page's draw callback. */
    for (int i = 0; i < v.edge_count; i++) {
        CHECK(v.edges[i].a < v.node_count);
        CHECK(v.edges[i].b < v.node_count);
        CHECK(v.edges[i].a != v.edges[i].b);
    }

    /* ui_graph places by index, so the demo must already be in the order the
     * parser would have produced. */
    for (int i = 1; i < v.node_count; i++) {
        CHECK(v.nodes[i].deg <= v.nodes[i - 1].deg);
    }

    for (int i = 0; i < v.agent_count; i++) {
        CHECK(v.agents[i].progress >= -1 && v.agents[i].progress <= 100);
        CHECK(v.agents[i].state < AGENT_STATE_COUNT);
        CHECK(v.agents[i].name[0] != '\0');
    }

    /* The header shows inbox_total, the list shows inbox_count. Claiming fewer
     * than are on screen would be visibly wrong. */
    CHECK(v.inbox_total >= v.inbox_count);
}

static void test_mock_exercises_every_agent_state(void)
{
    /* The demo screen is the picture in the README and the thing the simulator
     * renders by default. If it only ever showed RUNNING agents, the idle,
     * error and done rows would go out untested and unseen. */
    vault_t v;
    vault_mock(&v);

    bool seen[AGENT_STATE_COUNT] = { false };
    for (int i = 0; i < v.agent_count; i++) seen[v.agents[i].state] = true;
    for (int s = 0; s < AGENT_STATE_COUNT; s++) {
        if (!seen[s]) {
            g_total++; g_fail++;
            printf("  FAIL demo snapshot has no agent in state %s\n",
                   vault_agent_state_name((agent_state_t)s));
        } else {
            g_total++;
        }
    }
}

static void test_mock_is_the_layouts_worst_case(void)
{
    /* Real data is easy. The demo is deliberately the widest thing the pages
     * will be asked to draw, because it is what the simulator asserts against.
     * These are the properties that make it so. */
    vault_t v;
    vault_mock(&v);

    CHECK_INT(v.node_count, VAULT_NODES_MAX);      /* the fullest graph */
    CHECK_INT(v.recent_count, VAULT_RECENT_MAX);   /* every note row    */
    CHECK_INT(v.inbox_count, VAULT_INBOX_MAX);     /* every inbox row   */
    CHECK(v.inbox_total > v.inbox_count);          /* an overflowing queue */
    CHECK(v.stats.notes >= 1000);                  /* a grouped 4-digit counter */

    bool has_zero_day = false;
    for (int i = 0; i < VAULT_DAILY_DAYS; i++) {
        if (v.stats.daily[i] == 0) has_zero_day = true;
    }
    CHECK(has_zero_day);                           /* the divide-by-value trap */
}

int main(void)
{
    test_mock_matches_the_wire_fixture();
    test_mock_is_internally_legal();
    test_mock_exercises_every_agent_state();
    test_mock_is_the_layouts_worst_case();
    TH_REPORT("vault_mock");
}
