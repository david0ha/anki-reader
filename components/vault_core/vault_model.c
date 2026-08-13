/*
 * vault_model.c — the pure helpers declared in vault_model.h.
 *
 * No LVGL, no ESP-IDF, no allocation. Everything here is exercised directly by
 * test_vault_parse.c and indirectly by every page.
 */
#include "vault_model.h"

#include <string.h>

/* --- UTF-8-safe copy ------------------------------------------------------ */

/* Length in bytes of the UTF-8 sequence that starts with `c`, or 1 for a byte
 * that cannot start one (a stray continuation byte, or 0xF8..0xFF). Treating
 * junk as width 1 makes truncation degrade to a byte copy for invalid input
 * rather than looping or over-reading. */
static size_t seq_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t vault_str_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }

    size_t out = 0;
    size_t i = 0;
    for (;;) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\0') break;

        size_t n = seq_len(c);
        /* A sequence the source truncates is dropped whole: copying its first
         * two bytes would hand LVGL's decoder a codepoint that is not there. */
        for (size_t k = 1; k < n; k++) {
            if (src[i + k] == '\0') { n = 0; break; }
        }
        if (n == 0) break;

        if (out + n >= dst_size) break;      /* no room for this glyph + NUL */
        memcpy(dst + out, src + i, n);
        out += n;
        i   += n;
    }
    dst[out] = '\0';
    return out;
}

/* --- agent state ---------------------------------------------------------- */

const char *vault_agent_state_name(agent_state_t s)
{
    switch (s) {
    case AGENT_RUNNING: return "RUNNING";
    case AGENT_ERROR:   return "ERROR";
    case AGENT_DONE:    return "DONE";
    case AGENT_IDLE:    return "IDLE";
    default:            return "IDLE";
    }
}

/* Case-insensitive ASCII compare; the wire uses lowercase but a hand-written
 * producer will send "Running" sooner or later. */
static bool ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

agent_state_t vault_agent_state_from(const char *word)
{
    if (!word) return AGENT_IDLE;
    if (ieq(word, "running")) return AGENT_RUNNING;
    if (ieq(word, "error"))   return AGENT_ERROR;
    if (ieq(word, "failed"))  return AGENT_ERROR;
    if (ieq(word, "done"))    return AGENT_DONE;
    return AGENT_IDLE;
}

int vault_running_agents(const vault_t *v)
{
    if (!v) return 0;
    int n = 0;
    for (int i = 0; i < v->agent_count; i++) {
        if (v->agents[i].state == AGENT_RUNNING) n++;
    }
    return n;
}

/* --- derived statistics --------------------------------------------------- */

int vault_link_density_x100(const vault_t *v)
{
    if (!v || v->stats.notes <= 0) return 0;
    /* Integer math on purpose: no libm on the host tests' link line, and the
     * UI only ever prints two decimals. */
    return (int)(((long)v->stats.links * 100 + v->stats.notes / 2) / v->stats.notes);
}

int vault_orphan_rate_x10(const vault_t *v)
{
    if (!v || v->stats.notes <= 0) return 0;
    return (int)(((long)v->stats.orphans * 1000 + v->stats.notes / 2) / v->stats.notes);
}

int vault_daily_peak(const vault_t *v)
{
    if (!v) return 1;
    int peak = 1;
    for (int i = 0; i < VAULT_DAILY_DAYS; i++) {
        if (v->stats.daily[i] > peak) peak = v->stats.daily[i];
    }
    return peak;
}

/* --- fingerprint ---------------------------------------------------------- */

/* FNV-1a. Fed field by field rather than over the struct: struct padding is
 * never initialised, so hashing the raw bytes would make the fingerprint depend
 * on whatever was on the stack — and the whole point is that identical content
 * must hash identically, every boot, on device and in the simulator. */
#define FNV_OFFSET 2166136261u
#define FNV_PRIME  16777619u

static void h_bytes(uint32_t *h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= FNV_PRIME;
    }
}

static void h_str(uint32_t *h, const char *s)
{
    h_bytes(h, s, strlen(s));
    h_bytes(h, "\0", 1);        /* separator: "ab"+"c" must differ from "a"+"bc" */
}

static void h_int(uint32_t *h, int v)
{
    int32_t x = (int32_t)v;
    h_bytes(h, &x, sizeof(x));
}

static void h_artwork(uint32_t *h, const vault_artwork_t *a)
{
    h_int(h, a->valid);

    h_int(h, a->headline_count);
    for (int i = 0; i < a->headline_count; i++) {
        h_str(h, a->headline[i].text);
    }

    h_str(h, a->definition.headword);
    h_str(h, a->definition.meta);
    h_int(h, a->definition.line_count);
    for (int i = 0; i < a->definition.line_count; i++) {
        h_str(h, a->definition.lines[i].text);
    }

    h_str(h, a->note.title);
    h_str(h, a->note.path);
    h_int(h, a->note.backlink_total);
    h_int(h, a->note.backlink_count);
    for (int i = 0; i < a->note.backlink_count; i++) {
        h_str(h, a->note.backlinks[i]);
    }

    h_int(h, a->node_count);
    for (int i = 0; i < a->node_count; i++) {
        h_str(h, a->nodes[i].title);
        h_int(h, a->nodes[i].slot);
    }

    h_int(h, a->edge_count);
    for (int i = 0; i < a->edge_count; i++) {
        h_int(h, a->edges[i].a);
        h_int(h, a->edges[i].b);
    }

}

static void h_tarot_lines(uint32_t *h, const tarot_lines_t *lines)
{
    h_int(h, lines->line_count);
    for (int i = 0; i < lines->line_count; i++) h_str(h, lines->lines[i]);
}

static void h_tarot(uint32_t *h, const daily_tarot_t *t)
{
    h_int(h, t->valid);
    h_str(h, t->date);
    h_str(h, t->timezone);
    h_str(h, t->card_id);
    h_str(h, t->orientation);
    h_int(h, t->copy_version);
    h_tarot_lines(h, &t->headline);
    h_tarot_lines(h, &t->flow);
    h_tarot_lines(h, &t->caution);
    h_tarot_lines(h, &t->action);
}

static void h_tarot_pixels(uint32_t *h, const daily_tarot_t *t)
{
    /* timezone/orientation are validation constraints and copy_version is
     * producer metadata. None is printed; changing one must not flash e-paper. */
    h_str(h, t->date);
    h_str(h, t->card_id);
    h_tarot_lines(h, &t->headline);
    h_tarot_lines(h, &t->flow);
    h_tarot_lines(h, &t->caution);
    h_tarot_lines(h, &t->action);
}

uint32_t vault_hash(const vault_t *v)
{
    uint32_t h = FNV_OFFSET;
    if (!v) return h;

    h_int(&h, v->valid);
    h_int(&h, v->demo);
    h_str(&h, v->vault);
    h_str(&h, v->generated_at);

    h_int(&h, v->stats.notes);
    h_int(&h, v->stats.links);
    h_int(&h, v->stats.orphans);
    h_int(&h, v->stats.tags);
    h_int(&h, v->stats.added_today);
    h_int(&h, v->stats.added_7d);
    for (int i = 0; i < VAULT_DAILY_DAYS; i++) h_int(&h, v->stats.daily[i]);

    h_int(&h, v->tag_count);
    for (int i = 0; i < v->tag_count; i++) {
        h_str(&h, v->tags[i].name);
        h_int(&h, v->tags[i].count);
    }

    h_int(&h, v->agent_count);
    for (int i = 0; i < v->agent_count; i++) {
        const vault_agent_t *a = &v->agents[i];
        h_str(&h, a->name);
        h_int(&h, (int)a->state);
        h_str(&h, a->last_run);
        h_int(&h, a->processed);
        h_int(&h, a->queued);
        h_int(&h, a->progress);
        h_str(&h, a->note);
    }

    h_int(&h, v->node_count);
    for (int i = 0; i < v->node_count; i++) {
        h_str(&h, v->nodes[i].title);
        h_int(&h, v->nodes[i].deg);
    }
    h_int(&h, v->edge_count);
    for (int i = 0; i < v->edge_count; i++) {
        h_int(&h, v->edges[i].a);
        h_int(&h, v->edges[i].b);
    }

    h_int(&h, v->recent_count);
    for (int i = 0; i < v->recent_count; i++) {
        h_str(&h, v->recent[i].title);
        h_str(&h, v->recent[i].time);
        h_int(&h, v->recent[i].links);
    }

    h_int(&h, v->inbox_count);
    h_int(&h, v->inbox_total);
    for (int i = 0; i < v->inbox_count; i++) {
        h_str(&h, v->inbox[i].title);
        h_int(&h, v->inbox[i].age_days);
    }

    h_artwork(&h, &v->artwork);
    h_tarot(&h, &v->daily_tarot);

    return h;
}

uint32_t vault_tarot_hash(const vault_t *v)
{
    uint32_t h = FNV_OFFSET;
    if (!v || !v->valid || !v->daily_tarot.valid) return h;

    h_int(&h, v->demo);
    h_tarot_pixels(&h, &v->daily_tarot);
    return h;
}

uint32_t vault_artwork_hash(const vault_t *v)
{
    uint32_t h = FNV_OFFSET;
    if (!v || !v->valid || !v->artwork.valid) return h;

    h_artwork(&h, &v->artwork);
    return h;
}
