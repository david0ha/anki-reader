/*
 * ui_graph.c — see ui_graph.h.
 *
 * Concentric rings, integer arithmetic throughout. The trigonometry is a
 * 91-entry table rather than libm on purpose: the simulator's assertions
 * compare pixel positions against what the device draws, and sin() is only
 * guaranteed to agree between an x86 host and an Xtensa target to within an
 * ulp — which is exactly enough to move a node one pixel and fail a test for a
 * reason that has nothing to do with the layout.
 */
#include "ui_graph.h"

/* sin(0..90 degrees) x 32767. Symmetry gives the rest. */
static const int32_t SIN_Q15[91] = {
         0,    572,   1144,   1715,   2286,   2856,   3425,   3993,   4560,   5126,
      5690,   6252,   6813,   7371,   7927,   8481,   9032,   9580,  10126,  10668,
     11207,  11743,  12275,  12803,  13328,  13848,  14364,  14876,  15383,  15886,
     16383,  16876,  17364,  17846,  18323,  18794,  19260,  19720,  20173,  20621,
     21062,  21497,  21925,  22347,  22762,  23170,  23571,  23964,  24351,  24730,
     25101,  25465,  25821,  26169,  26509,  26841,  27165,  27481,  27788,  28087,
     28377,  28659,  28932,  29196,  29451,  29697,  29934,  30162,  30381,  30591,
     30791,  30982,  31163,  31335,  31498,  31650,  31794,  31927,  32051,  32165,
     32269,  32364,  32448,  32523,  32587,  32642,  32687,  32722,  32747,  32762,
     32767,
};

/* sin of an angle in whole degrees, x32767. Any integer degree is accepted. */
static int32_t sin_q15(int deg)
{
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg <= 90)  return  SIN_Q15[deg];
    if (deg <= 180) return  SIN_Q15[180 - deg];
    if (deg <= 270) return -SIN_Q15[deg - 180];
    return -SIN_Q15[360 - deg];
}

static int32_t cos_q15(int deg) { return sin_q15(deg + 90); }

/* --- node sizing ---------------------------------------------------------- */

#define NODE_R_MIN   7
#define NODE_R_MAX   22

/* Radius from link degree, linear between the smallest and largest node in this
 * particular snapshot. Scaling against the local range rather than an absolute
 * one means a vault of uniformly-linked notes still shows *some* variation
 * instead of fourteen identical dots. */
static int node_radius(int deg, int lo, int hi)
{
    if (hi <= lo) return (NODE_R_MIN + NODE_R_MAX) / 2;
    int r = NODE_R_MIN + ((deg - lo) * (NODE_R_MAX - NODE_R_MIN)) / (hi - lo);
    if (r < NODE_R_MIN) r = NODE_R_MIN;
    if (r > NODE_R_MAX) r = NODE_R_MAX;
    return r;
}

/* --- ring plan ------------------------------------------------------------ */

/* How many nodes each ring holds. Ring 0 is the single centre node. The caps
 * grow with circumference; with VAULT_NODES_MAX at 14 only the first two rings
 * are ever used, but the arithmetic does not depend on that. */
#define RING_MAX 4
static const int RING_CAP[RING_MAX] = { 6, 10, 14, 18 };

int ui_graph_layout(const vault_t *v, int w, int h, graph_pos_t *out, int out_max)
{
    if (!v || !out || out_max <= 0 || w <= 0 || h <= 0) return 0;

    int n = v->node_count;
    if (n > out_max) n = out_max;
    if (n <= 0) return 0;

    /* Degree range. Nodes arrive sorted descending, but do not rely on it —
     * a producer that sends them flat should still get sensible circles. */
    int lo = v->nodes[0].deg, hi = v->nodes[0].deg;
    for (int i = 1; i < n; i++) {
        if (v->nodes[i].deg < lo) lo = v->nodes[i].deg;
        if (v->nodes[i].deg > hi) hi = v->nodes[i].deg;
    }

    /* Work out how many rings this many nodes needs, so the outermost ring can
     * be placed at the canvas edge rather than wherever a fixed step lands. */
    int rings = 0;
    for (int left = n - 1; left > 0 && rings < RING_MAX; rings++) {
        left -= RING_CAP[rings];
    }

    const int cx = w / 2;
    const int cy = h / 2;

    /* The usable radius has to leave room for the biggest circle AND the label
     * box hanging below it, or an outer node's title runs off the canvas. This
     * is the whole reason the label geometry is computed here rather than in
     * the page: the constraint is a layout constraint. */
    const int label_w = 96;
    int rx_max = cx - NODE_R_MAX - label_w / 2 - 2;
    int ry_max = cy - NODE_R_MAX - GRAPH_LABEL_H - 4;
    if (rx_max < 0) rx_max = 0;
    if (ry_max < 0) ry_max = 0;

    /* The innermost ring cannot start at zero: the centre node is up to
     * NODE_R_MAX across and carries a label box under it, and a first ring
     * placed proportionally would sit inside that. The floor is "clear the
     * centre node, clear its label, clear the ring node's own radius". */
    int rx_min = NODE_R_MAX + label_w / 2 + NODE_R_MAX + 8;
    int ry_min = NODE_R_MAX + GRAPH_LABEL_H + NODE_R_MAX + 8;
    if (rx_min > rx_max) rx_min = rx_max;
    if (ry_min > ry_max) ry_min = ry_max;

    int idx = 0;

    /* Ring 0: the biggest hub, dead centre. */
    {
        graph_pos_t *p = &out[idx];
        p->r = node_radius(v->nodes[idx].deg, lo, hi);
        p->x = cx;
        p->y = cy;
        p->label_x = cx - label_w / 2;
        p->label_y = cy + p->r + 2;
        p->label_w = label_w;
        idx++;
    }

    for (int ring = 0; ring < rings && idx < n; ring++) {
        int count = n - idx;
        if (count > RING_CAP[ring]) count = RING_CAP[ring];

        /* Rings are spaced evenly between the centre clearance and the edge:
         * one ring sits at the edge, two at the floor and the edge, three at
         * the floor, the midpoint and the edge. */
        int rx = (rings == 1) ? rx_max : rx_min + (rx_max - rx_min) * ring / (rings - 1);
        int ry = (rings == 1) ? ry_max : ry_min + (ry_max - ry_min) * ring / (rings - 1);

        /* Start at the top and, on every ring after the first, rotate by half a
         * step so nodes do not line up radially with the ring inside them. */
        int step  = count > 0 ? 360 / count : 360;
        int start = -90 + (ring & 1 ? step / 2 : 0);

        for (int k = 0; k < count && idx < n; k++, idx++) {
            int deg = start + k * step;
            graph_pos_t *p = &out[idx];
            p->r = node_radius(v->nodes[idx].deg, lo, hi);
            p->x = cx + (int)(((int64_t)rx * cos_q15(deg)) >> 15);
            p->y = cy + (int)(((int64_t)ry * sin_q15(deg)) >> 15);
            p->label_x = p->x - label_w / 2;
            p->label_y = p->y + p->r + 2;
            p->label_w = label_w;
        }
    }

    /* Final clamp. The radius budget above should make this a no-op, and it is
     * asserted to be one in the host test — but a clamp here is the difference
     * between an ugly picture and a label drawn outside the canvas, and the
     * budget depends on constants somebody will eventually change. */
    for (int i = 0; i < idx; i++) {
        graph_pos_t *p = &out[i];
        if (p->x - p->r < 0)  p->x = p->r;
        if (p->x + p->r >= w) p->x = w - 1 - p->r;
        if (p->y - p->r < 0)  p->y = p->r;
        if (p->y + p->r >= h) p->y = h - 1 - p->r;

        p->label_x = p->x - p->label_w / 2;
        p->label_y = p->y + p->r + 2;
        if (p->label_x < 0) p->label_x = 0;
        if (p->label_x + p->label_w > w) p->label_x = w - p->label_w;
        if (p->label_y + GRAPH_LABEL_H > h) p->label_y = h - GRAPH_LABEL_H;
        if (p->label_y < 0) p->label_y = 0;
    }

    return idx;
}
