/*
 * vault_parse.c — the wire payload -> vault_t.
 *
 * The producer is somebody's Python script on their laptop. It will send a
 * float where an int belongs, a null where a string belongs, an empty array, a
 * 900-entry array, an edge that points at a node it did not include, and — the
 * day the laptop sleeps — half a response. None of that may take the board
 * down, and none of it may leave a half-written snapshot on the glass.
 *
 * So: parse into a local, validate and clamp every field, and only copy into
 * the caller's struct on success. A rejected payload leaves the previous
 * snapshot exactly as it was, which is why the header can honestly badge it
 * "오래됨" rather than going blank.
 *
 * Portable: cJSON only. test_vault_parse.c builds this file directly.
 */
#include "vault_parse.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* --- defensive accessors --------------------------------------------------
 * Every one of these takes "the key is missing" and "the key holds the wrong
 * type" to the same place: the default. That is the entire error policy for
 * individual fields, and it is why the field code below has no branches. */

static int jint(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v)) return def;
    double d = cJSON_GetNumberValue(v);
    if (d < -2147483000.0) return -2147483000;
    if (d >  2147483000.0) return  2147483000;
    return (int)d;
}

/* Non-negative int: counters that go backwards are a producer bug, and a
 * negative width or count would reach a drawing routine. */
static int juint(const cJSON *o, const char *key, int def)
{
    int v = jint(o, key, def);
    return v < 0 ? 0 : v;
}

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static const cJSON *jarr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsArray(v) ? v : NULL;
}

static const cJSON *jobj(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsObject(v) ? v : NULL;
}

/* --- sections ------------------------------------------------------------- */

static void parse_stats(const cJSON *root, vault_t *v)
{
    const cJSON *s = jobj(root, "stats");
    if (!s) return;

    v->stats.notes       = juint(s, "notes", 0);
    v->stats.links       = juint(s, "links", 0);
    v->stats.orphans     = juint(s, "orphans", 0);
    v->stats.tags        = juint(s, "tags", 0);
    v->stats.added_today = juint(s, "added_today", 0);
    v->stats.added_7d    = juint(s, "added_7d", 0);

    /* daily[] is right-aligned: today is always the last column, so a producer
     * that sends four days still puts them where "recent" means recent. */
    const cJSON *d = jarr(s, "daily");
    if (!d) return;
    int n = cJSON_GetArraySize(d);
    if (n > VAULT_DAILY_DAYS) n = VAULT_DAILY_DAYS;
    int skip = cJSON_GetArraySize(d) - n;           /* drop the oldest overflow */
    for (int i = 0; i < n; i++) {
        const cJSON *e = cJSON_GetArrayItem(d, skip + i);
        int val = cJSON_IsNumber(e) ? (int)cJSON_GetNumberValue(e) : 0;
        if (val < 0) val = 0;
        v->stats.daily[VAULT_DAILY_DAYS - n + i] = val;
    }
}

static void parse_tags(const cJSON *root, vault_t *v)
{
    const cJSON *arr = jarr(root, "tags");
    if (!arr) return;
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        if (v->tag_count >= VAULT_TAGS_MAX) break;
        if (!cJSON_IsObject(e)) continue;
        const char *name = jstr(e, "name");
        if (!name[0]) continue;
        vault_tag_t *t = &v->tags[v->tag_count++];
        vault_str_copy(t->name, sizeof(t->name), name);
        t->count = juint(e, "count", 0);
    }
}

static void parse_agents(const cJSON *root, vault_t *v)
{
    const cJSON *arr = jarr(root, "agents");
    if (!arr) return;
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        if (v->agent_count >= VAULT_AGENTS_MAX) break;
        if (!cJSON_IsObject(e)) continue;
        const char *name = jstr(e, "name");
        if (!name[0]) continue;

        vault_agent_t *a = &v->agents[v->agent_count++];
        vault_str_copy(a->name, sizeof(a->name), name);
        a->state = vault_agent_state_from(jstr(e, "state"));
        vault_str_copy(a->last_run, sizeof(a->last_run), jstr(e, "last_run"));
        a->processed = juint(e, "processed", 0);
        a->queued    = juint(e, "queued", 0);
        vault_str_copy(a->note, sizeof(a->note), jstr(e, "note"));

        /* -1 means "no progress bar", which is different from 0% — an idle
         * agent should not render a permanently empty bar. */
        int p = jint(e, "progress", -1);
        a->progress = (p < 0) ? -1 : (p > 100 ? 100 : p);
    }
}

/* Insertion sort by degree descending. ui_graph places nodes by index — the
 * biggest hub at the centre — so the order is load-bearing, and trusting the
 * producer to have sorted would make the picture depend on their script. n is
 * at most VAULT_NODES_MAX, so the O(n^2) is 200 comparisons. */
static void sort_nodes(vault_t *v, uint8_t *remap)
{
    for (int i = 0; i < v->node_count; i++) remap[i] = (uint8_t)i;

    for (int i = 1; i < v->node_count; i++) {
        vault_node_t key = v->nodes[i];
        uint8_t      kid = remap[i];
        int j = i - 1;
        while (j >= 0 && v->nodes[j].deg < key.deg) {
            v->nodes[j + 1] = v->nodes[j];
            remap[j + 1]    = remap[j];
            j--;
        }
        v->nodes[j + 1] = key;
        remap[j + 1]    = kid;
    }
}

static void parse_graph(const cJSON *root, vault_t *v)
{
    const cJSON *g = jobj(root, "graph");
    if (!g) return;

    /* The wire's node ids are arbitrary; the model's are array indices. Build
     * the translation while reading, then rewrite it once more after sorting. */
    int wire_id[VAULT_NODES_MAX];

    const cJSON *arr = jarr(g, "nodes");
    const cJSON *e = NULL;
    if (arr) {
        cJSON_ArrayForEach(e, arr) {
            if (v->node_count >= VAULT_NODES_MAX) break;
            if (!cJSON_IsObject(e)) continue;
            const char *title = jstr(e, "title");
            if (!title[0]) continue;
            wire_id[v->node_count] = jint(e, "id", v->node_count);
            vault_node_t *n = &v->nodes[v->node_count++];
            vault_str_copy(n->title, sizeof(n->title), title);
            n->deg = juint(e, "deg", 0);
        }
    }

    /* Sorting moves the nodes, so the edge list has to be translated twice:
     * wire id -> the index we read it at, then that -> where sorting put it. */
    int     pre_count = v->node_count;
    uint8_t remap[VAULT_NODES_MAX];              /* remap[new] = old */
    sort_nodes(v, remap);

    uint8_t post_of_pre[VAULT_NODES_MAX];        /* the inverse */
    for (int i = 0; i < pre_count; i++) post_of_pre[remap[i]] = (uint8_t)i;

    arr = jarr(g, "edges");
    if (!arr) return;
    cJSON_ArrayForEach(e, arr) {
        if (v->edge_count >= VAULT_EDGES_MAX) break;
        if (!cJSON_IsArray(e) || cJSON_GetArraySize(e) < 2) continue;
        const cJSON *ja = cJSON_GetArrayItem(e, 0);
        const cJSON *jb = cJSON_GetArrayItem(e, 1);
        if (!cJSON_IsNumber(ja) || !cJSON_IsNumber(jb)) continue;

        int wa = (int)cJSON_GetNumberValue(ja);
        int wb = (int)cJSON_GetNumberValue(jb);

        /* Translate wire ids to pre-sort indices by search — ids need not be
         * dense or ordered, and the list is 14 long. An edge that names a node
         * we truncated away, or names itself, is dropped: drawing a line to a
         * node that is not on screen is worse than not drawing it. */
        int pa = -1, pb = -1;
        for (int i = 0; i < pre_count; i++) {
            if (wire_id[i] == wa) pa = i;
            if (wire_id[i] == wb) pb = i;
        }
        if (pa < 0 || pb < 0 || pa == pb) continue;

        uint8_t a = post_of_pre[pa], b = post_of_pre[pb];

        /* Deduplicate: the same pair in both directions draws the same line
         * twice, which on a 1-bit panel is invisible but wastes the edge cap. */
        bool dup = false;
        for (int i = 0; i < v->edge_count; i++) {
            if ((v->edges[i].a == a && v->edges[i].b == b) ||
                (v->edges[i].a == b && v->edges[i].b == a)) { dup = true; break; }
        }
        if (dup) continue;

        v->edges[v->edge_count].a = a;
        v->edges[v->edge_count].b = b;
        v->edge_count++;
    }
}

static void parse_recent(const cJSON *root, vault_t *v)
{
    const cJSON *arr = jarr(root, "recent");
    if (!arr) return;
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        if (v->recent_count >= VAULT_RECENT_MAX) break;
        if (!cJSON_IsObject(e)) continue;
        const char *title = jstr(e, "title");
        if (!title[0]) continue;
        vault_note_t *n = &v->recent[v->recent_count++];
        vault_str_copy(n->title, sizeof(n->title), title);
        vault_str_copy(n->time, sizeof(n->time), jstr(e, "time"));
        n->links = juint(e, "links", 0);
    }
}

static void parse_inbox(const cJSON *root, vault_t *v)
{
    const cJSON *arr = jarr(root, "inbox");
    if (!arr) return;
    v->inbox_total = cJSON_GetArraySize(arr);
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        if (v->inbox_count >= VAULT_INBOX_MAX) break;
        if (!cJSON_IsObject(e)) continue;
        const char *title = jstr(e, "title");
        if (!title[0]) continue;
        vault_inbox_t *n = &v->inbox[v->inbox_count++];
        vault_str_copy(n->title, sizeof(n->title), title);
        n->age_days = juint(e, "age_days", 0);
    }
    /* An explicit total wins — the producer may be sending a window of a much
     * longer queue. Never let it claim fewer than we are showing. */
    int declared = juint(root, "inbox_total", 0);
    if (declared > v->inbox_total) v->inbox_total = declared;
    if (v->inbox_total < v->inbox_count) v->inbox_total = v->inbox_count;
}

/* --- public --------------------------------------------------------------- */

bool vault_parse(const char *json, size_t len, vault_t *out)
{
    if (!json || !out || len == 0) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;                 /* truncated or not JSON at all */
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    vault_t v;
    memset(&v, 0, sizeof(v));

    vault_str_copy(v.vault, sizeof(v.vault), jstr(root, "vault"));
    vault_str_copy(v.generated_at, sizeof(v.generated_at), jstr(root, "generated_at"));

    parse_stats(root, &v);
    parse_tags(root, &v);
    parse_agents(root, &v);
    parse_graph(root, &v);
    parse_recent(root, &v);
    parse_inbox(root, &v);

    cJSON_Delete(root);

    /* A well-formed object that carries no vault content at all is a rejection,
     * not an empty dashboard: it is what a login page or an error envelope
     * looks like after cJSON gets through with it, and replacing a good
     * snapshot with blankness is the one failure the user actually notices. */
    if (v.stats.notes == 0 && v.agent_count == 0 &&
        v.node_count == 0 && v.recent_count == 0 && v.inbox_count == 0) {
        return false;
    }

    v.valid = true;
    v.demo  = false;
    *out = v;
    return true;
}
