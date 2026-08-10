/*
 * ui_page_graph.c — page 1, 링크 그래프.
 *
 * The vault's highest-degree notes and the links between them. Where the
 * positions come from is ui_graph.c's problem — deterministic, testable, no
 * LVGL. This file only turns coordinates into pixels.
 *
 * Edges and circles are drawn immediately in a DRAW_MAIN callback rather than
 * built as widgets: fourteen nodes and thirty-two edges would be ninety-odd
 * objects rebuilt on every poll, and LVGL has no line-between-two-points widget
 * that composites cleanly on a 1-bit panel anyway. The titles ARE widgets,
 * because text needs a font.
 *
 * Draw order matters and is the only subtle thing here: edges first, then a
 * WHITE disc per node, then the node's outline. The white disc is what stops
 * six edges converging on a hub from turning it into an unreadable black star.
 */
#include "ui_internal.h"
#include "ui_graph.h"

#include <string.h>

#define GR_HEAD_Y      2
#define GR_CANVAS_X    UI_PAD
#define GR_CANVAS_Y    34
#define GR_CANVAS_W    (UI_W - 2 * UI_PAD)                        /* 620 */
#define GR_CANVAS_H    (UI_CONTENT_H - GR_CANVAS_Y - 4)           /* 360 */

/* A hub is drawn solid rather than outlined. Three is enough to give the eye an
 * anchor without the picture turning into a field of blobs. */
#define GR_SOLID_TOP   3

#define GR_EDGE_W      1
#define GR_RING_W      3

static lv_obj_t *s_root;
static lv_obj_t *s_head;
static lv_obj_t *s_canvas;
static lv_obj_t *s_label[VAULT_NODES_MAX];

/* What the draw callback needs. Written by update(), read by the callback —
 * both run on the task that owns LVGL, so no locking beyond what the caller
 * already holds. */
static graph_pos_t  s_pos[VAULT_NODES_MAX];
static int          s_pos_n;
static vault_edge_t s_edges[VAULT_EDGES_MAX];
static int          s_edge_n;

static void canvas_draw_cb(lv_event_t *e)
{
    lv_obj_t   *o = lv_event_get_target(e);
    lv_layer_t *L = lv_event_get_layer(e);
    if (!L || s_pos_n <= 0) return;

    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const int ox = a.x1, oy = a.y1;

    for (int i = 0; i < s_edge_n; i++) {
        int p = s_edges[i].a, q = s_edges[i].b;
        if (p >= s_pos_n || q >= s_pos_n) continue;
        ui_draw_line_abs(L,
                         ox + s_pos[p].x, oy + s_pos[p].y,
                         ox + s_pos[q].x, oy + s_pos[q].y,
                         GR_EDGE_W, false);
    }

    for (int i = 0; i < s_pos_n; i++) {
        int cx = ox + s_pos[i].x, cy = oy + s_pos[i].y, r = s_pos[i].r;
        ui_draw_disc_abs(L, cx, cy, r, true);           /* clear the edges */
        if (i < GR_SOLID_TOP) {
            ui_draw_disc_abs(L, cx, cy, r, false);
        } else {
            ui_draw_ring_abs(L, cx, cy, r, GR_RING_W, 0, 360);
        }
    }
}

lv_obj_t *ui_page_graph_create(lv_obj_t *par)
{
    s_root = ui_pane(par, 0, 0, UI_W, UI_CONTENT_H);

    s_head = ui_lab_w(s_root, UI_PAD, GR_HEAD_Y, 380, UI_F_HEAD,
                      LV_TEXT_ALIGN_LEFT, S_PAGE_GRAPH);
    ui_lab_w(s_root, 400, GR_HEAD_Y + 4, UI_W - UI_PAD - 400, UI_F_BODY,
             LV_TEXT_ALIGN_RIGHT, S_GRAPH_LEGEND);
    ui_fill(s_root, UI_PAD, GR_CANVAS_Y - 6, GR_CANVAS_W, 1);

    s_canvas = ui_pane(s_root, GR_CANVAS_X, GR_CANVAS_Y, GR_CANVAS_W, GR_CANVAS_H);
    lv_obj_add_event_cb(s_canvas, canvas_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    for (int i = 0; i < VAULT_NODES_MAX; i++) {
        s_label[i] = ui_lab_w(s_canvas, 0, 0, 96, UI_F_BODY,
                              LV_TEXT_ALIGN_CENTER, "");
        /* Opaque, unlike every other label on the board: these sit on top of a
         * field of edges, and a line running through the middle of a word makes
         * it unreadable on a panel with no greys to soften it. */
        ui_lab_opaque(s_label[i]);
        ui_show(s_label[i], false);
    }

    return s_root;
}

void ui_page_graph_update(const vault_t *v)
{
    if (!s_root) return;

    if (!v || !v->valid || v->node_count == 0) {
        s_pos_n  = 0;
        s_edge_n = 0;
        ui_set(s_head, S_PAGE_GRAPH);
        for (int i = 0; i < VAULT_NODES_MAX; i++) ui_show(s_label[i], false);
        lv_obj_invalidate(s_canvas);
        return;
    }

    ui_setf(s_head, "%s · %d %s / %d %s",
            S_PAGE_GRAPH, v->node_count, S_GRAPH_HUBS,
            v->stats.links, S_GRAPH_LINKS);

    s_pos_n = ui_graph_layout(v, GR_CANVAS_W, GR_CANVAS_H, s_pos, VAULT_NODES_MAX);

    s_edge_n = v->edge_count;
    if (s_edge_n > VAULT_EDGES_MAX) s_edge_n = VAULT_EDGES_MAX;
    memcpy(s_edges, v->edges, sizeof(vault_edge_t) * (size_t)s_edge_n);

    for (int i = 0; i < VAULT_NODES_MAX; i++) {
        if (i < s_pos_n) {
            lv_obj_set_pos(s_label[i], s_pos[i].label_x, s_pos[i].label_y);
            lv_obj_set_width(s_label[i], s_pos[i].label_w);
            ui_set(s_label[i], v->nodes[i].title);
            ui_show(s_label[i], true);
        } else {
            ui_show(s_label[i], false);
        }
    }

    lv_obj_invalidate(s_canvas);
}
