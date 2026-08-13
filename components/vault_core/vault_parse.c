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

#include <limits.h>
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

static bool exact_int(const cJSON *v, int *out)
{
    if (!cJSON_IsNumber(v) || !out) return false;
    double d = cJSON_GetNumberValue(v);
    if (d < (double)INT_MIN || d > (double)INT_MAX) return false;
    int n = (int)d;
    if ((double)n != d) return false;
    *out = n;
    return true;
}

static bool valid_json_utf8(const char *json, size_t len)
{
    const unsigned char *s = (const unsigned char *)json;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < len;) {
        unsigned char c = s[i++];
        if (c < 0x20) {
            /* JSON permits space/tab/CR/LF between tokens, but every control
             * inside a string must be escaped. A raw NUL must never silently
             * terminate a bounded response. */
            if (in_string || (c != '\t' && c != '\n' && c != '\r')) return false;
            continue;
        }
        if (c < 0x80) {
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
            } else if (c == '"') {
                in_string = true;
            }
            continue;
        }

        size_t continuation = 0;
        uint32_t codepoint = 0;
        uint32_t minimum = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            continuation = 1;
            codepoint = (uint32_t)(c & 0x1F);
            minimum = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            continuation = 2;
            codepoint = (uint32_t)(c & 0x0F);
            minimum = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            continuation = 3;
            codepoint = (uint32_t)(c & 0x07);
            minimum = 0x10000;
        } else {
            return false;
        }
        if (continuation > len - i) return false;
        for (size_t k = 0; k < continuation; k++) {
            unsigned char tail = s[i++];
            if ((tail & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (uint32_t)(tail & 0x3F);
        }
        if (codepoint < minimum || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
    }
    return true;
}

static bool only_json_whitespace(const char *p, const char *end)
{
    while (p < end) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return false;
        p++;
    }
    return true;
}

static int edge_compare(const artwork_edge_t *a, const artwork_edge_t *b)
{
    if (a->a != b->a) return (int)a->a - (int)b->a;
    return (int)a->b - (int)b->b;
}

static bool valid_tarot_date(const char *date)
{
    if (!date || strlen(date) != 10 || date[4] != '-' || date[7] != '-') return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (date[i] < '0' || date[i] > '9') return false;
    }
    int year = (date[0] - '0') * 1000 + (date[1] - '0') * 100 +
               (date[2] - '0') * 10 + (date[3] - '0');
    int month = (date[5] - '0') * 10 + (date[6] - '0');
    int day = (date[8] - '0') * 10 + (date[9] - '0');
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1 || month < 1 || month > 12) return false;
    int max_day = days[month];
    if (month == 2 && (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0))) max_day++;
    return day >= 1 && day <= max_day;
}

static bool valid_tarot_card_id(const char *card_id)
{
    if (!card_id) return false;
    const char *dash = strrchr(card_id, '-');
    if (!dash || dash[1] < '0' || dash[1] > '9' ||
        dash[2] < '0' || dash[2] > '9' || dash[3] != '\0') return false;
    int number = (dash[1] - '0') * 10 + (dash[2] - '0');
    size_t suit_len = (size_t)(dash - card_id);
    if (suit_len == 5 && memcmp(card_id, "major", 5) == 0) {
        return number >= 0 && number <= 21;
    }
    bool minor = (suit_len == 4 && memcmp(card_id, "cups", 4) == 0) ||
                 (suit_len == 5 && memcmp(card_id, "wands", 5) == 0) ||
                 (suit_len == 6 && memcmp(card_id, "swords", 6) == 0) ||
                 (suit_len == 9 && memcmp(card_id, "pentacles", 9) == 0);
    return minor && number >= 1 && number <= 14;
}

static bool parse_tarot_lines(const cJSON *owner, const char *key, tarot_lines_t *out)
{
    const cJSON *arr = jarr(owner, key);
    if (!arr || cJSON_GetArraySize(arr) < 1 ||
        cJSON_GetArraySize(arr) > TAROT_LINES_MAX) return false;
    int limit = cJSON_GetArraySize(arr);
    for (int i = 0; i < limit; i++) {
        const cJSON *line = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsString(line) || !line->valuestring || !line->valuestring[0]) return false;
        /* One wire element is exactly one fixed display row. Embedded line
         * breaks would make LVGL wrap inside a one-line box and hide pixels. */
        for (const unsigned char *p = (const unsigned char *)line->valuestring; *p; p++) {
            if (*p < 0x20) return false;
        }
        vault_str_copy(out->lines[out->line_count], TAROT_LINE_MAX, line->valuestring);
        if (!out->lines[out->line_count][0]) return false;
        out->line_count++;
    }
    return out->line_count > 0;
}

static void parse_daily_tarot(const cJSON *root, vault_t *v)
{
    const cJSON *wire = jobj(root, "daily_tarot");
    if (!wire) return;

    daily_tarot_t tarot;
    memset(&tarot, 0, sizeof(tarot));
    const char *date = jstr(wire, "date");
    const char *timezone = jstr(wire, "timezone");
    const char *card_id = jstr(wire, "card_id");
    const char *orientation = jstr(wire, "orientation");
    int copy_version = 0;
    if (!valid_tarot_date(date) || strcmp(timezone, "Asia/Seoul") != 0 ||
        !valid_tarot_card_id(card_id) || strcmp(orientation, "upright") != 0 ||
        !exact_int(cJSON_GetObjectItemCaseSensitive(wire, "copy_version"), &copy_version) ||
        copy_version <= 0 ||
        !parse_tarot_lines(wire, "headline", &tarot.headline) ||
        !parse_tarot_lines(wire, "flow", &tarot.flow) ||
        !parse_tarot_lines(wire, "caution", &tarot.caution) ||
        !parse_tarot_lines(wire, "action", &tarot.action)) {
        return;
    }
    vault_str_copy(tarot.date, sizeof(tarot.date), date);
    vault_str_copy(tarot.timezone, sizeof(tarot.timezone), timezone);
    vault_str_copy(tarot.card_id, sizeof(tarot.card_id), card_id);
    vault_str_copy(tarot.orientation, sizeof(tarot.orientation), orientation);
    tarot.copy_version = copy_version;
    tarot.valid = true;
    v->daily_tarot = tarot;
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

static int parse_artwork_lines(const cJSON *owner, const char *key,
                               artwork_line_t *out, int out_max)
{
    const cJSON *arr = jarr(owner, key);
    if (!arr) return 0;
    int count = 0;
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        if (count >= out_max) break;
        if (!cJSON_IsString(e) || !e->valuestring || !e->valuestring[0]) continue;
        vault_str_copy(out[count].text, sizeof(out[count].text), e->valuestring);
        if (out[count].text[0]) count++;
    }
    return count;
}

static void parse_artwork(const cJSON *root, vault_t *v)
{
    const cJSON *a = jobj(root, "artwork");
    if (!a) return;

    vault_artwork_t *art = &v->artwork;
    art->headline_count = parse_artwork_lines(a, "headline", art->headline,
                                               ARTWORK_HEADLINE_MAX);

    /* Schema 3 deliberately changed definition from a bare string array to an
     * object.  Requiring the actual object prevents a schema-2 payload from
     * being mistaken for the new normalized artwork contract. */
    const cJSON *definition = jobj(a, "definition");
    if (definition) {
        vault_str_copy(art->definition.headword, sizeof(art->definition.headword),
                       jstr(definition, "headword"));
        vault_str_copy(art->definition.meta, sizeof(art->definition.meta),
                       jstr(definition, "meta"));
        art->definition.line_count =
            parse_artwork_lines(definition, "lines", art->definition.lines,
                                ARTWORK_DEFINITION_MAX);
    }

    const cJSON *note = jobj(a, "note");
    if (note) {
        vault_str_copy(art->note.title, sizeof(art->note.title), jstr(note, "title"));
        vault_str_copy(art->note.path, sizeof(art->note.path), jstr(note, "path"));
        art->note.backlink_total = juint(note, "backlink_total", 0);

        const cJSON *backlinks = jarr(note, "backlinks");
        const cJSON *backlink = NULL;
        if (backlinks) {
            cJSON_ArrayForEach(backlink, backlinks) {
                if (art->note.backlink_count >= ARTWORK_BACKLINKS_MAX) break;
                if (!cJSON_IsString(backlink) || !backlink->valuestring ||
                    !backlink->valuestring[0]) continue;
                int i = art->note.backlink_count;
                vault_str_copy(art->note.backlinks[i], sizeof(art->note.backlinks[i]),
                               backlink->valuestring);
                if (art->note.backlinks[i][0]) art->note.backlink_count++;
            }
        }
        if (art->note.backlink_total < art->note.backlink_count) {
            art->note.backlink_total = art->note.backlink_count;
        }
    }

    const cJSON *g = jobj(a, "graph");
    const cJSON *arr = g ? jarr(g, "nodes") : NULL;
    const cJSON *e = NULL;
    int wire_id[ARTWORK_NODES_MAX];
    if (arr) {
        cJSON_ArrayForEach(e, arr) {
            if (art->node_count >= ARTWORK_NODES_MAX) break;
            if (!cJSON_IsObject(e)) continue;
            const char *title = jstr(e, "title");
            if (!title[0]) continue;

            int i = art->node_count;
            int id = 0;
            if (!exact_int(cJSON_GetObjectItemCaseSensitive(e, "id"), &id)) continue;
            bool duplicate_id = false;
            for (int j = 0; j < i; j++) {
                if (wire_id[j] == id) duplicate_id = true;
            }
            if (duplicate_id) continue;

            wire_id[i] = id;
            artwork_node_t *node = &art->nodes[art->node_count++];
            vault_str_copy(node->title, sizeof(node->title), title);

            int slot = 0;
            bool valid_slot = exact_int(cJSON_GetObjectItemCaseSensitive(e, "slot"), &slot) &&
                              slot >= 0 && slot < ARTWORK_NODES_MAX;
            node->slot = valid_slot ? (uint8_t)slot : UINT8_MAX;
        }
    }

    /* Reserve the first owner of every legal producer slot before assigning
     * fallbacks. This prevents an earlier malformed node from stealing a later
     * node's declared focus slot. */
    bool used_slot[ARTWORK_NODES_MAX] = {false};
    for (int i = 0; i < art->node_count; i++) {
        int slot = art->nodes[i].slot;
        if (slot < ARTWORK_NODES_MAX && !used_slot[slot]) {
            used_slot[slot] = true;
        } else {
            art->nodes[i].slot = UINT8_MAX;
        }
    }
    if (art->node_count > 0 && !used_slot[0]) {
        /* Prefer an already-unassigned node as the synthesized focus so every
         * valid producer slot survives. If all nodes declared unique slots,
         * promote the first node and release only its old slot. */
        int focus = -1;
        for (int i = 0; i < art->node_count && focus < 0; i++) {
            if (art->nodes[i].slot == UINT8_MAX) focus = i;
        }
        if (focus < 0) {
            focus = 0;
            used_slot[art->nodes[focus].slot] = false;
        }
        art->nodes[focus].slot = 0;
        used_slot[0] = true;
    }
    for (int i = 0; i < art->node_count; i++) {
        if (art->nodes[i].slot != UINT8_MAX) continue;
        int slot = 0;
        while (slot < ARTWORK_NODES_MAX && used_slot[slot]) slot++;
        art->nodes[i].slot = (uint8_t)slot;
        used_slot[slot] = true;
    }

    arr = g ? jarr(g, "edges") : NULL;
    if (arr) {
        cJSON_ArrayForEach(e, arr) {
            if (!cJSON_IsArray(e) || cJSON_GetArraySize(e) != 2) continue;
            const cJSON *ja = cJSON_GetArrayItem(e, 0);
            const cJSON *jb = cJSON_GetArrayItem(e, 1);
            int wa = 0, wb = 0;
            if (!exact_int(ja, &wa) || !exact_int(jb, &wb)) continue;
            int pa = -1, pb = -1;
            for (int i = 0; i < art->node_count; i++) {
                if (wire_id[i] == wa) pa = i;
                if (wire_id[i] == wb) pb = i;
            }
            if (pa < 0 || pb < 0 || pa == pb) continue;
            uint8_t a_idx = (uint8_t)(pa < pb ? pa : pb);
            uint8_t b_idx = (uint8_t)(pa < pb ? pb : pa);
            bool dup = false;
            for (int i = 0; i < art->edge_count; i++) {
                const artwork_edge_t *old = &art->edges[i];
                if (old->a == a_idx && old->b == b_idx) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            artwork_edge_t edge = {a_idx, b_idx};
            if (art->edge_count == ARTWORK_EDGES_MAX &&
                edge_compare(&edge, &art->edges[ARTWORK_EDGES_MAX - 1]) >= 0) {
                continue;
            }
            int pos = art->edge_count;
            if (pos == ARTWORK_EDGES_MAX) pos--;
            while (pos > 0 && edge_compare(&edge, &art->edges[pos - 1]) < 0) {
                if (pos < ARTWORK_EDGES_MAX) art->edges[pos] = art->edges[pos - 1];
                pos--;
            }
            art->edges[pos] = edge;
            if (art->edge_count < ARTWORK_EDGES_MAX) art->edge_count++;
        }
    }

    bool has_prose = art->headline_count > 0 ||
                     art->definition.headword[0] ||
                     art->definition.line_count > 0;
    art->valid = art->note.title[0] && has_prose;
}

/* --- public --------------------------------------------------------------- */

bool vault_parse(const char *json, size_t len, vault_t *out)
{
    if (!json || !out || len == 0) return false;
    if (!valid_json_utf8(json, len)) return false;

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &parse_end, false);
    if (!root) return false;                 /* truncated or not JSON at all */
    if (!parse_end || parse_end < json || parse_end > json + len ||
        !only_json_whitespace(parse_end, json + len)) {
        cJSON_Delete(root);
        return false;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *schema_value = cJSON_GetObjectItemCaseSensitive(root, "schema");
    int schema = 0;
    if (!exact_int(schema_value, &schema) || schema != 3) {
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
    parse_artwork(root, &v);
    parse_daily_tarot(root, &v);

    cJSON_Delete(root);

    /* Schema 3 always carries normalized artwork. Reject an incomplete payload
     * before it can erase the current glass. */
    if (!v.artwork.valid) {
        return false;
    }
    if (v.stats.notes == 0 && v.agent_count == 0 &&
        v.node_count == 0 && v.recent_count == 0 && v.inbox_count == 0 &&
        !v.artwork.valid) {
        return false;
    }

    v.valid = true;
    v.demo  = false;
    *out = v;
    return true;
}
