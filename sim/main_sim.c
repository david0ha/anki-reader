/*
 * LVGL desktop simulator for the Obsidian board (headless -> BMP).
 *
 * This is not a preview. It renders the real ui_vault.c at the real 648x480,
 * binarizes with the device's exact rule (px < 0x7FFF ? black : white), writes
 * one bitmap per page, and then asserts things about the pixels — that every
 * string the UI will draw has a glyph, that every row of every list actually
 * inked, that nothing ran past the panel. It exits non-zero when any of that
 * fails, so `./sim.sh` is a test that happens to leave screenshots behind.
 *
 * The two failure modes it exists for are the ones that cost the most to find
 * on hardware: a missing glyph (a tofu box, visible only after a four-second
 * refresh) and a row that silently rendered nothing because a label was
 * positioned past its parent and LVGL clipped it away.
 *
 *   ./sim.sh                                              # built-in demo data
 *   VAULT_URL=http://localhost:8123/vault.json ./sim.sh   # the device's own path
 */
#include "lvgl.h"

#include "ui_vault.h"
#include "ui_internal.h"      /* the shared grid — see sim/CMakeLists.txt */
#include "ui_fonts.h"
#include "ui_graph.h"
#include "ui_icons.h"
#include "ui_strings.h"
#include "vault_mock.h"
#include "vault_model.h"
#include "vault_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define HOR UI_W
#define VER UI_H

static uint8_t  fb[HOR * VER * 2];
static uint16_t capture[HOR * VER];
static uint32_t g_tick = 0;
static int      g_fail = 0;

#define FAILV(fmt, ...) do { g_fail++; printf("  FAIL " fmt "\n", __VA_ARGS__); } while (0)
#define FAIL(msg)       do { g_fail++; printf("  FAIL %s\n", (msg)); } while (0)

static uint32_t tick_cb(void) { return g_tick; }

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    if (w == HOR && h == VER) memcpy(capture, px, sizeof(capture));
    lv_display_flush_ready(d);
}

static void run_refresh(int steps)
{
    for (int i = 0; i < steps; i++) { g_tick += 16; lv_timer_handler(); }
}

static int is_black(int x, int y)
{
    if (x < 0 || y < 0 || x >= HOR || y >= VER) return 0;
    return capture[y * HOR + x] < 0x7FFF;
}

/* capture[] (RGB565) -> 24bit BMP, using the device's binarization rule. */
static void write_mono_bmp(const char *path)
{
    int W = HOR, H = VER, rowsize = (W * 3 + 3) & ~3, datasize = rowsize * H;
    int filesize = 54 + datasize;
    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=filesize; hdr[3]=filesize>>8; hdr[4]=filesize>>16; hdr[5]=filesize>>24;
    hdr[10]=54; hdr[14]=40;
    hdr[18]=W; hdr[19]=W>>8; hdr[20]=W>>16; hdr[21]=W>>24;
    hdr[22]=H; hdr[23]=H>>8; hdr[24]=H>>16; hdr[25]=H>>24;
    hdr[26]=1; hdr[28]=24;
    hdr[34]=datasize; hdr[35]=datasize>>8; hdr[36]=datasize>>16; hdr[37]=datasize>>24;
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot open %s\n", path); return; }
    fwrite(hdr, 1, 54, f);
    uint8_t *row = calloc(1, rowsize);
    if (!row) { fclose(f); return; }
    for (int y = H - 1; y >= 0; y--) {
        for (int x = 0; x < W; x++) {
            uint8_t v = is_black(x, y) ? 0 : 255;
            row[x*3]=v; row[x*3+1]=v; row[x*3+2]=v;
        }
        fwrite(row, 1, rowsize, f);
    }
    free(row); fclose(f);
}

/* How much of the panel is inked. A screen that renders nothing (missing font,
 * mis-sized container) comes out at 0%; one that has gone solid black comes out
 * near 100%. Both are bugs a human skimming filenames would miss. */
static double ink_pct(void)
{
    long on = 0;
    for (int i = 0; i < HOR * VER; i++) if (capture[i] < 0x7FFF) on++;
    return 100.0 * on / (HOR * VER);
}

static void shot(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.bmp", dir, name);
    write_mono_bmp(path);
    printf("  %-16s %5.1f%% ink   %s\n", name, ink_pct(), path);
}

/* --- pixel predicates ----------------------------------------------------- */

static int any_ink(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (is_black(x, y)) return 1;
    return 0;
}

static void want_ink(const char *what, int x0, int y0, int x1, int y1)
{
    if (!any_ink(x0, y0, x1, y1)) {
        FAILV("%s: nothing rendered in x[%d..%d) y[%d..%d)", what, x0, x1, y0, y1);
    }
}

static void want_blank(const char *what, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (is_black(x, y)) {
                FAILV("%s: unexpected ink at (%d,%d)", what, x, y);
                return;
            }
        }
    }
}

/* A horizontal rule must be black across essentially its whole width. */
static void want_rule(const char *what, int y, int x0, int x1)
{
    int black = 0;
    for (int x = x0; x < x1; x++) black += is_black(x, y);
    if (black < (x1 - x0) - 2) {
        FAILV("%s: row y=%d is %d/%d black — rule missing or broken",
              what, y, black, x1 - x0);
    }
}

static void want_vrule(const char *what, int x, int y0, int y1)
{
    int black = 0;
    for (int y = y0; y < y1; y++) black += is_black(x, y);
    if (black < (y1 - y0) - 2) {
        FAILV("%s: column x=%d is %d/%d black — rule missing or broken",
              what, x, black, y1 - y0);
    }
}

/* --- glyph coverage -------------------------------------------------------
 *
 * The tempting version of this check looks at the bitmap for the hollow
 * rectangle LVGL draws in place of a missing glyph — unreliable, and
 * unnecessary, because the font will simply tell us. Ask it whether it has each
 * codepoint of each string it is going to be asked to draw.
 *
 * On this board that matters for a reason it did not on the fortune board this
 * forked from: half these strings arrive over the network at runtime, so the
 * check has to run over the *data*, not just over the source literals. */

static uint32_t utf8_next(const char *s, int *i)
{
    unsigned char c = (unsigned char)s[*i];
    int extra = c < 0x80 ? 0 : (c < 0xE0 ? 1 : (c < 0xF0 ? 2 : 3));
    uint32_t cp = c < 0x80 ? c : (c & (0x3F >> extra));
    int k = 0;
    while (k < extra && s[*i + 1 + k]) {
        cp = (cp << 6) | ((unsigned char)s[*i + 1 + k] & 0x3F);
        k++;
    }
    *i += k + 1;
    return cp;
}

static void cover(const lv_font_t *font, const char *label, const char *text)
{
    if (!text) return;
    int i = 0;
    while (text[i]) {
        int at = i;
        uint32_t cp = utf8_next(text, &i);
        if (cp == '\n' || cp == '\r') continue;
        lv_font_glyph_dsc_t dsc;
        if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
            FAILV("%s: U+%04X (byte %d of \"%s\") missing from the font -> tofu",
                  label, cp, at, text);
        }
    }
}

/* Both faces are full 완성형, so a string drawn at one size must be drawable at
 * the other — and the UI moves strings between the two often enough that
 * checking only the face currently in use would let a regression through. */
static void cover_both(const char *label, const char *text)
{
    cover(&ui_font_kr_16, label, text);
    cover(&ui_font_kr_20, label, text);
}

static void check_fixed_strings(void)
{
    static const char *fixed[] = {
        S_BRAND, S_BADGE_DEMO, S_BADGE_STALE, S_BADGE_OFFLINE, S_NO_DATA, S_WAITING,
        S_KEY_PAGE, S_KEY_REFRESH, S_KEY_WIFI,
        S_PAGE_STATS, S_STAT_NOTES, S_STAT_LINKS, S_STAT_ORPHANS, S_STAT_TAGS,
        S_ACTIVITY, S_ADDED_TODAY, S_ADDED_WEEK, S_TOP_TAGS, S_HEALTH,
        S_LINK_DENSITY, S_ORPHAN_RATE, S_LAST_SYNC, S_PER_NOTE,
        S_PAGE_GRAPH, S_GRAPH_HUBS, S_GRAPH_LINKS, S_GRAPH_LEGEND,
        S_PAGE_AGENTS, S_AGENTS_RUNNING, S_AGENT_QUEUED, S_AGENT_DONE_N,
        S_STATE_RUNNING, S_STATE_IDLE, S_STATE_ERROR, S_STATE_DONE,
        S_PAGE_NOTES, S_RECENT, S_INBOX, S_DAYS_SUFFIX, S_EMPTY_RECENT, S_EMPTY_INBOX,
        S_WIFI_TITLE, S_RESTARTING,
        /* Characters that exist only in runtime-composed strings — the ↔ after a
         * link count, the interpunct between footer hints, the digits of every
         * number. This is the check that catches the whole class of bug where a
         * label renders but the space inside it comes out as a box. */
        S_COMPOSED_CHARS,
        S_DATA_PUNCT,
        "0123456789",
        "일월화수목금토",           /* ui_vault_tick's weekday table */
    };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        cover_both("fixed string", fixed[i]);
    }
}

/* Every string in the snapshot that will be drawn. With a full 완성형 face this
 * should never fail on Hangul — which is the point: if it ever does, the title
 * contains something outside 완성형 (old Hangul, a rare syllable, an emoji) and
 * that is a real decision to make, not a mystery box on the glass. */
static void check_data_strings(const vault_t *v)
{
    cover_both("vault name", v->vault);
    cover_both("generated_at", v->generated_at);
    for (int i = 0; i < v->tag_count; i++)    cover_both("tag", v->tags[i].name);
    for (int i = 0; i < v->agent_count; i++) {
        cover_both("agent name", v->agents[i].name);
        cover_both("agent note", v->agents[i].note);
        cover_both("agent last_run", v->agents[i].last_run);
    }
    for (int i = 0; i < v->node_count; i++)   cover_both("node title", v->nodes[i].title);
    for (int i = 0; i < v->recent_count; i++) {
        cover_both("recent title", v->recent[i].title);
        cover_both("recent time", v->recent[i].time);
    }
    for (int i = 0; i < v->inbox_count; i++)  cover_both("inbox title", v->inbox[i].title);
}

/* --- chrome --------------------------------------------------------------
 *
 * Mirrors ui_vault.c's header/footer grid. If those constants move, move these
 * with them — that is the trade for asserting on pixels rather than on the
 * widget tree. */
#define H_BADGE_X   250
#define H_CLOCK_X   506
#define H_CLOCK_W    92
#define H_BATT_X    (UI_W - UI_PAD - 28)
#define F_Y         (UI_H - UI_FOOTER_H)
#define F_DOT_X     178
#define F_DOT_SZ     10
#define F_DOT_GAP    16
#define F_LEGEND_X  (F_DOT_X + UI_PAGE_COUNT * F_DOT_GAP + 12)

static void check_chrome(const char *page, int page_index, bool expect_badge)
{
    want_ink("brand", UI_PAD, 8, UI_PAD + 226, 40);
    want_ink("date", 356, 10, 500, 38);
    want_ink("clock", H_CLOCK_X, 4, H_CLOCK_X + H_CLOCK_W, 40);
    want_ink("battery/plug icon", H_BATT_X, 6, H_BATT_X + 28, 38);

    want_rule("header rule", UI_HEADER_H, 0, UI_W);
    want_rule("footer rule", F_Y - UI_RULE, 0, UI_W);

    if (expect_badge) {
        want_ink("header badge", H_BADGE_X, 10, H_BADGE_X + 96, 36);
    }

    /* The page indicator: exactly one filled dot, and it must be this page's.
     * A filled dot inks its centre; a hollow one does not. */
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        int cx = F_DOT_X + i * F_DOT_GAP + F_DOT_SZ / 2;
        int cy = F_Y + 8 + 5 + F_DOT_SZ / 2;
        int filled = is_black(cx, cy);
        if (i == page_index && !filled) {
            FAILV("%s: page dot %d should be filled", page, i);
        }
        if (i != page_index && filled) {
            FAILV("%s: page dot %d should be hollow", page, i);
        }
    }

    want_ink("footer page title", UI_PAD, F_Y + 4, UI_PAD + 158, UI_H);
    want_ink("footer legend", F_LEGEND_X, F_Y + 4, UI_W - UI_PAD, UI_H);

    /* The legend is right-aligned into its slot; if it has outgrown the slot,
     * LVGL clips it and the left end vanishes rather than overlapping. Ink in
     * the gap between the dots and the legend slot means the layout has drifted. */
    want_blank("legend gap", F_DOT_X + UI_PAGE_COUNT * F_DOT_GAP, F_Y + 2,
               F_LEGEND_X - 4, UI_H);
}

/* --- page 0: stats -------------------------------------------------------
 * Mirrors ui_page_stats.c's grid. */
#define CW           (UI_W - 2 * UI_PAD)
#define CELL_W       (CW / 4)
#define ROW_A_H      112
#define ACT_BASE_Y   234
#define ACT_SLOT_W   (CW / VAULT_DAILY_DAYS)
#define ACT_BAR_W    (ACT_SLOT_W - 14)
#define ROW_C_Y      248
#define ROW_C_ROW_H  20
#define ROW_C_FIRST  (ROW_C_Y + 28)
#define TAG_NAME_W   92
#define TAG_BAR_X    (UI_PAD + TAG_NAME_W + 8)
#define TAG_BAR_W    138
#define HEALTH_X     344
#define HEALTH_LAB_W 130

static void check_page_stats(const vault_t *v)
{
    const int Y = UI_CONTENT_Y;

    for (int i = 0; i < 4; i++) {
        int x = UI_PAD + i * CELL_W;
        char what[48];
        snprintf(what, sizeof(what), "counter %d", i);
        want_ink(what, x, Y + 12, x + CELL_W, Y + 68);
        snprintf(what, sizeof(what), "caption %d", i);
        want_ink(what, x, Y + 72, x + CELL_W, Y + 100);
    }
    want_rule("counters rule", Y + ROW_A_H, UI_PAD, UI_W - UI_PAD);

    want_ink("activity heading", UI_PAD, Y + 118, UI_PAD + 240, Y + 146);
    want_ink("activity summary", 300, Y + 118, UI_W - UI_PAD, Y + 146);
    want_rule("activity baseline", Y + ACT_BASE_Y, UI_PAD, UI_W - UI_PAD);

    /* Every day gets a column, including the zero day — a chart that divides by
     * the value rather than the peak renders that one as nothing at all. */
    for (int i = 0; i < VAULT_DAILY_DAYS; i++) {
        int x = UI_PAD + i * ACT_SLOT_W + (ACT_SLOT_W - ACT_BAR_W) / 2;
        char what[48];
        snprintf(what, sizeof(what), "activity bar %d (value %d)", i, v->stats.daily[i]);
        want_ink(what, x, Y + ACT_BASE_Y - 4, x + ACT_BAR_W, Y + ACT_BASE_Y);
        snprintf(what, sizeof(what), "activity value %d", i);
        want_ink(what, x - 6, Y + 148, x + ACT_BAR_W + 6, Y + ACT_BASE_Y);
    }

    want_ink("top tags heading", UI_PAD, Y + ROW_C_Y, UI_PAD + 200, Y + ROW_C_Y + 26);
    for (int i = 0; i < v->tag_count; i++) {
        int y = Y + ROW_C_FIRST + i * ROW_C_ROW_H;
        char what[64];
        snprintf(what, sizeof(what), "tag %d name (%s)", i, v->tags[i].name);
        want_ink(what, UI_PAD, y, UI_PAD + TAG_NAME_W, y + ROW_C_ROW_H);
        snprintf(what, sizeof(what), "tag %d bar", i);
        want_ink(what, TAG_BAR_X, y, TAG_BAR_X + TAG_BAR_W, y + ROW_C_ROW_H);
        snprintf(what, sizeof(what), "tag %d count", i);
        want_ink(what, TAG_BAR_X + TAG_BAR_W + 6, y, UI_PAD + 304, y + ROW_C_ROW_H);
    }

    want_ink("health heading", HEALTH_X, Y + ROW_C_Y, HEALTH_X + 200, Y + ROW_C_Y + 26);
    for (int i = 0; i < 4; i++) {
        int y = Y + ROW_C_FIRST + i * ROW_C_ROW_H;
        char what[48];
        snprintf(what, sizeof(what), "health %d label", i);
        want_ink(what, HEALTH_X, y, HEALTH_X + HEALTH_LAB_W, y + ROW_C_ROW_H);
        snprintf(what, sizeof(what), "health %d value", i);
        want_ink(what, HEALTH_X + HEALTH_LAB_W, y, UI_W - UI_PAD, y + ROW_C_ROW_H);
    }
}

/* --- page 1: graph ------------------------------------------------------- */
#define GR_CANVAS_X  UI_PAD
#define GR_CANVAS_Y  34
#define GR_CANVAS_W  (UI_W - 2 * UI_PAD)
#define GR_CANVAS_H  (UI_CONTENT_H - GR_CANVAS_Y - 4)

static void check_page_graph(const vault_t *v)
{
    const int Y = UI_CONTENT_Y;
    want_ink("graph heading", UI_PAD, Y + 2, UI_PAD + 380, Y + 30);

    graph_pos_t pos[VAULT_NODES_MAX];
    int n = ui_graph_layout(v, GR_CANVAS_W, GR_CANVAS_H, pos, VAULT_NODES_MAX);
    if (n != v->node_count) {
        FAILV("graph: laid out %d of %d nodes", n, v->node_count);
    }

    const int ox = GR_CANVAS_X, oy = Y + GR_CANVAS_Y;
    for (int i = 0; i < n; i++) {
        char what[80];
        /* The circle. Every node is either a filled disc or a ring, so the band
         * just inside its radius must have ink either way. */
        snprintf(what, sizeof(what), "node %d circle (%s)", i, v->nodes[i].title);
        want_ink(what,
                 ox + pos[i].x - pos[i].r, oy + pos[i].y - pos[i].r,
                 ox + pos[i].x + pos[i].r + 1, oy + pos[i].y + pos[i].r + 1);

        /* The title. This is the check that catches a label pushed outside the
         * canvas: LVGL clips it, so the box is simply empty. */
        snprintf(what, sizeof(what), "node %d label (%s)", i, v->nodes[i].title);
        want_ink(what,
                 ox + pos[i].label_x, oy + pos[i].label_y,
                 ox + pos[i].label_x + pos[i].label_w,
                 oy + pos[i].label_y + GRAPH_LABEL_H + 4);
    }

    /* Edges: sample the midpoint of each. It can legitimately be covered by a
     * node's white disc, so this only requires that SOME edge midpoints ink —
     * a graph that drew no edges at all is the failure worth catching. */
    int mid_ink = 0;
    for (int i = 0; i < v->edge_count; i++) {
        int a = v->edges[i].a, b = v->edges[i].b;
        if (a >= n || b >= n) continue;
        int mx = ox + (pos[a].x + pos[b].x) / 2;
        int my = oy + (pos[a].y + pos[b].y) / 2;
        if (any_ink(mx - 2, my - 2, mx + 3, my + 3)) mid_ink++;
    }
    if (v->edge_count > 0 && mid_ink < v->edge_count / 2) {
        FAILV("graph: only %d of %d edge midpoints inked — edges not drawn?",
              mid_ink, v->edge_count);
    }
}

/* --- page 2: agents -----------------------------------------------------
 * Mirrors ui_page_agents.c's grid. */
#define AG_ROW_Y     38
#define AG_ROW_H     60
#define AG_NAME_X    38
#define AG_NAME_W    152
#define AG_CHIP_X    194
#define AG_CHIP_W    78
#define AG_BAR_X     566
#define AG_BAR_W     68
#define AG_NOTE_DY   30

static void check_page_agents(const vault_t *v)
{
    const int Y = UI_CONTENT_Y;
    want_ink("agents heading", UI_PAD, Y + 2, UI_PAD + 380, Y + 30);
    want_rule("agents rule", Y + 30, UI_PAD, UI_W - UI_PAD);

    for (int i = 0; i < v->agent_count; i++) {
        int y = Y + AG_ROW_Y + i * AG_ROW_H;
        const vault_agent_t *a = &v->agents[i];
        char what[96];

        snprintf(what, sizeof(what), "agent %d bullet (%s)", i, a->name);
        want_ink(what, UI_PAD, y + 4, UI_PAD + 16, y + 24);

        snprintf(what, sizeof(what), "agent %d name (%s)", i, a->name);
        want_ink(what, AG_NAME_X, y, AG_NAME_X + AG_NAME_W, y + 28);

        /* The state chip is a filled black rectangle, so it inks whatever the
         * word inside it is; that also proves the white-on-black label did not
         * paint the whole chip out. */
        snprintf(what, sizeof(what), "agent %d state chip", i);
        want_ink(what, AG_CHIP_X, y + 2, AG_CHIP_X + AG_CHIP_W, y + 24);

        snprintf(what, sizeof(what), "agent %d counters", i);
        want_ink(what, 282, y + 4, 558, y + 28);

        if (a->progress >= 0) {
            snprintf(what, sizeof(what), "agent %d progress bar", i);
            want_ink(what, AG_BAR_X, y + 5, AG_BAR_X + AG_BAR_W, y + 20);
        } else {
            snprintf(what, sizeof(what), "agent %d has no bar", i);
            want_blank(what, AG_BAR_X, y + 5, AG_BAR_X + AG_BAR_W, y + 20);
        }

        if (a->note[0]) {
            snprintf(what, sizeof(what), "agent %d note (%s)", i, a->note);
            want_ink(what, AG_NAME_X, y + AG_NOTE_DY, UI_W - UI_PAD, y + AG_NOTE_DY + 24);
        }
    }

    /* Rows beyond the agent count must be empty, not left showing the previous
     * snapshot's contents. */
    for (int i = v->agent_count; i < VAULT_AGENTS_MAX; i++) {
        int y = Y + AG_ROW_Y + i * AG_ROW_H;
        char what[48];
        snprintf(what, sizeof(what), "unused agent row %d", i);
        want_blank(what, UI_PAD, y, UI_W - UI_PAD, y + AG_ROW_H - 6);
    }
}

/* --- page 3: notes ------------------------------------------------------
 * Mirrors ui_page_notes.c's grid. */
#define NT_ROW_Y     40
#define NT_ROW_H     42
#define NT_SPLIT_X   332
#define RC_TIME_W    50
#define RC_TITLE_X   (UI_PAD + RC_TIME_W + 8)
#define RC_LINKS_X   270
#define IB_X         (NT_SPLIT_X + 14)
#define IB_TITLE_X   (IB_X + 12 + 10)
#define IB_AGE_X     578

static void check_page_notes(const vault_t *v)
{
    const int Y = UI_CONTENT_Y;
    want_ink("recent heading", UI_PAD, Y + 2, UI_PAD + 200, Y + 30);
    want_ink("inbox heading", IB_X, Y + 2, IB_X + 200, Y + 30);
    want_vrule("column divider", NT_SPLIT_X, Y + 4, Y + UI_CONTENT_H - 8);

    for (int i = 0; i < v->recent_count; i++) {
        int y = Y + NT_ROW_Y + i * NT_ROW_H;
        char what[96];
        snprintf(what, sizeof(what), "recent %d time (%s)", i, v->recent[i].time);
        want_ink(what, UI_PAD, y, UI_PAD + RC_TIME_W, y + 26);
        snprintf(what, sizeof(what), "recent %d title (%s)", i, v->recent[i].title);
        want_ink(what, RC_TITLE_X, y, RC_LINKS_X - 8, y + 26);
        snprintf(what, sizeof(what), "recent %d links", i);
        want_ink(what, RC_LINKS_X, y, RC_LINKS_X + 52, y + 26);
    }

    for (int i = 0; i < v->inbox_count; i++) {
        int y = Y + NT_ROW_Y + i * NT_ROW_H;
        char what[96];
        snprintf(what, sizeof(what), "inbox %d checkbox", i);
        want_ink(what, IB_X, y + 2, IB_X + 14, y + 20);
        snprintf(what, sizeof(what), "inbox %d title (%s)", i, v->inbox[i].title);
        want_ink(what, IB_TITLE_X, y, IB_AGE_X - 8, y + 26);
        snprintf(what, sizeof(what), "inbox %d age", i);
        want_ink(what, IB_AGE_X, y, IB_AGE_X + 56, y + 26);
    }
}

/* --- empty-state pass ----------------------------------------------------- */

static void check_empty_state(void)
{
    /* A board whose first poll has not landed yet, or whose vault is brand new.
     * Every page must still render its chrome and say something, rather than
     * going blank — a blank e-Paper panel is indistinguishable from a dead one. */
    ui_vault_set_data(NULL);
    for (int p = 0; p < UI_PAGE_COUNT; p++) {
        ui_vault_show_page((ui_page_t)p);
        run_refresh(6);
        double ink = ink_pct();
        if (ink < 0.5) {
            FAILV("empty page %d rendered almost nothing (%.2f%% ink)", p, ink);
        }
        want_rule("empty header rule", UI_HEADER_H, 0, UI_W);
        want_rule("empty footer rule", F_Y - UI_RULE, 0, UI_W);
    }
}

/* --- main ----------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *outdir = (argc > 1) ? argv[1] : "shots";

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(HOR, VER);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, fb, NULL, sizeof(fb), LV_DISPLAY_RENDER_MODE_FULL);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_screen_load(scr);
    ui_vault_create(scr);

    /* Content: the built-in demo snapshot, or — with VAULT_URL set — the real
     * fetch+parse path the device runs, against the real server. Same code,
     * same bytes, same pixels. */
    vault_t v;
    const char *url = getenv("VAULT_URL");
    if (url && *url) {
        vault_fetch_result_t r = vault_service_fetch(url, &v);
        if (r == VAULT_FETCH_OK) {
            printf("fetched %s -> %d notes, %d agents, %d nodes\n",
                   url, v.stats.notes, v.agent_count, v.node_count);
        } else {
            printf("fetch of %s failed (%s) — falling back to the demo snapshot\n",
                   url, vault_fetch_result_name(r));
            vault_mock(&v);
        }
    } else {
        vault_mock(&v);
        printf("using the built-in demo snapshot (set VAULT_URL=... for a live fetch)\n");
    }

    printf("checking glyph coverage\n");
    check_fixed_strings();
    check_data_strings(&v);

    ui_status_t st = {
        .online = true, .stale = false, .battery_present = true, .battery_pct = 84,
    };
    ui_vault_set_data(&v);
    ui_vault_set_status(&st);
    ui_vault_tick();

    printf("rendering %s/\n", outdir);

    static const char *names[UI_PAGE_COUNT] = { "0_stats", "1_graph", "2_agents", "3_notes" };
    for (int p = 0; p < UI_PAGE_COUNT; p++) {
        ui_vault_show_page((ui_page_t)p);
        run_refresh(8);
        shot(outdir, names[p]);
        check_chrome(names[p], p, v.demo);
        switch (p) {
        case UI_PAGE_STATS:  check_page_stats(&v);  break;
        case UI_PAGE_GRAPH:  check_page_graph(&v);  break;
        case UI_PAGE_AGENTS: check_page_agents(&v); break;
        case UI_PAGE_NOTES:  check_page_notes(&v);  break;
        default: break;
        }
    }

    /* The offline/stale header, which no normal render exercises. */
    ui_vault_show_page(UI_PAGE_STATS);
    st.online = false;
    st.stale = true;
    st.battery_present = false;
    ui_vault_set_status(&st);
    run_refresh(6);
    shot(outdir, "4_offline");
    want_ink("offline badge", H_BADGE_X, 10, H_BADGE_X + 96, 36);

    /* The provisioning overlay. */
    ui_vault_set_overlay(S_WIFI_TITLE,
                         "1. Join Wi-Fi:\nObsidian Board-1A2B\n\n"
                         "2. Stay connected,\nthen open the page it offers");
    run_refresh(8);
    shot(outdir, "5_setup");
    want_ink("overlay title", 60, 150, UI_W - 60, 190);
    want_ink("overlay body", 60, 195, UI_W - 60, 330);
    /* The overlay must be opaque: on e-Paper a "hidden" page is still
     * physically on the glass until something covers it. */
    want_blank("overlay covers the footer", UI_PAD, F_Y, UI_W - UI_PAD, UI_H);
    ui_vault_set_overlay(NULL, NULL);

    check_empty_state();

    printf("%s — %d layout/glyph problem(s)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail ? 1 : 0;
}
