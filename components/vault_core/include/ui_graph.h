/*
 * ui_graph.h — where the link-graph nodes go.
 *
 * Deliberately NOT a force-directed layout. A physics relaxation would give a
 * prettier picture and three problems this board cannot afford:
 *
 *   - it is iterative, and this runs on the task that owns a panel;
 *   - it is seeded, so the simulator and the device would draw different
 *     pictures from identical data, which makes the simulator useless as a test;
 *   - it has no bound, so a node can end up off-canvas and nobody notices until
 *     it is on the glass.
 *
 * Instead: concentric rings, biggest hub in the middle, everything else spread
 * evenly outwards in degree order. Pure arithmetic, deterministic, and every
 * node provably inside the canvas — which test_graph_layout.c asserts.
 *
 * No LVGL and no ESP-IDF here; the page file turns these coordinates into
 * widgets, and the host test checks them without either.
 */
#pragma once

#include "vault_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x, y;          /* node centre, canvas-relative */
    int r;             /* node radius, scaled by link degree */
    int label_x;       /* left edge of the title's box */
    int label_y;       /* top edge of the title's box */
    int label_w;       /* width available to the title */
} graph_pos_t;

/* Height reserved under each node for its title. The page must use this same
 * value for the label's font size and box, or the assertions in the simulator
 * will disagree with the pixels. */
#define GRAPH_LABEL_H   20

/* Lay out v->nodes inside a `w` x `h` canvas.
 *
 * Writes v->node_count entries into out[] (caller supplies at least
 * VAULT_NODES_MAX). Every node — circle AND label box — is guaranteed to lie
 * within [0,w) x [0,h). Returns the number of entries written. */
int ui_graph_layout(const vault_t *v, int w, int h, graph_pos_t *out, int out_max);

#ifdef __cplusplus
}
#endif
