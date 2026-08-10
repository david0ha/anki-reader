/*
 * Host unit tests for ui_graph.c.
 *
 * The link graph is the one page whose correctness is geometric rather than
 * textual, and geometry has a failure mode the simulator's ink checks cannot
 * catch on their own: a node placed *just* off the canvas draws nothing at all,
 * so the page looks fine — thirteen circles instead of fourteen — and nobody
 * counts.
 *
 * So the invariant is asserted directly, for every node count the model
 * permits and for canvases both wider and taller than they are long.
 */
#include "th.h"

#include "ui_graph.h"
#include "vault_model.h"

/* Build a snapshot of `n` nodes with descending degrees, the way the parser
 * hands them over. */
static void make(vault_t *v, int n, int top_deg)
{
    memset(v, 0, sizeof(*v));
    v->valid = true;
    v->node_count = n;
    for (int i = 0; i < n; i++) {
        snprintf(v->nodes[i].title, sizeof(v->nodes[i].title), "n%d", i);
        v->nodes[i].deg = top_deg - i;
        if (v->nodes[i].deg < 1) v->nodes[i].deg = 1;
    }
}

static void check_inside(const char *label, const graph_pos_t *p, int n, int w, int h)
{
    for (int i = 0; i < n; i++) {
        if (p[i].x - p[i].r < 0 || p[i].x + p[i].r >= w ||
            p[i].y - p[i].r < 0 || p[i].y + p[i].r >= h) {
            g_total++; g_fail++;
            printf("  FAIL %s: node %d circle (%d,%d r%d) escapes %dx%d\n",
                   label, i, p[i].x, p[i].y, p[i].r, w, h);
        } else {
            g_total++;
        }
        if (p[i].label_x < 0 || p[i].label_x + p[i].label_w > w ||
            p[i].label_y < 0 || p[i].label_y + GRAPH_LABEL_H > h) {
            g_total++; g_fail++;
            printf("  FAIL %s: node %d label (%d,%d %dx%d) escapes %dx%d\n",
                   label, i, p[i].label_x, p[i].label_y,
                   p[i].label_w, GRAPH_LABEL_H, w, h);
        } else {
            g_total++;
        }
    }
}

static void test_everything_stays_on_the_canvas(void)
{
    /* The real page's canvas, plus two deliberately awkward shapes: a tall
     * narrow one (where the horizontal radius budget runs out first) and a
     * short wide one (where the label height does). */
    const int sizes[][2] = { { 620, 360 }, { 300, 400 }, { 620, 150 }, { 200, 200 } };

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        for (int n = 1; n <= VAULT_NODES_MAX; n++) {
            vault_t v;
            make(&v, n, 30);
            graph_pos_t pos[VAULT_NODES_MAX];
            int got = ui_graph_layout(&v, sizes[s][0], sizes[s][1], pos, VAULT_NODES_MAX);
            CHECK_INT(got, n);
            char label[64];
            snprintf(label, sizeof(label), "%dx%d n=%d", sizes[s][0], sizes[s][1], n);
            check_inside(label, pos, got, sizes[s][0], sizes[s][1]);
        }
    }
}

static void test_layout_is_deterministic(void)
{
    /* The simulator's screenshots are a test, not a preview. That only holds if
     * the same data produces the same pixels on an x86 laptop and an Xtensa
     * core — which is why ui_graph.c uses an integer sine table instead of
     * libm. */
    vault_t v;
    make(&v, 14, 24);

    graph_pos_t a[VAULT_NODES_MAX], b[VAULT_NODES_MAX];
    int na = ui_graph_layout(&v, 620, 360, a, VAULT_NODES_MAX);
    int nb = ui_graph_layout(&v, 620, 360, b, VAULT_NODES_MAX);
    CHECK_INT(na, nb);
    CHECK_INT(memcmp(a, b, sizeof(graph_pos_t) * (size_t)na), 0);
}

static void test_biggest_hub_is_the_centre(void)
{
    vault_t v;
    make(&v, 10, 24);
    graph_pos_t p[VAULT_NODES_MAX];
    int n = ui_graph_layout(&v, 620, 360, p, VAULT_NODES_MAX);
    CHECK_INT(n, 10);
    CHECK_INT(p[0].x, 620 / 2);
    CHECK_INT(p[0].y, 360 / 2);
}

static void test_radius_tracks_degree(void)
{
    vault_t v;
    make(&v, 14, 30);
    graph_pos_t p[VAULT_NODES_MAX];
    int n = ui_graph_layout(&v, 620, 360, p, VAULT_NODES_MAX);

    /* Degrees descend, so radii must not ascend. Equal is fine — the scale is
     * integer and two adjacent degrees can land on the same pixel. */
    for (int i = 1; i < n; i++) {
        CHECK(p[i].r <= p[i - 1].r);
    }
    CHECK(p[0].r > p[n - 1].r);      /* but the extremes must actually differ */
}

static void test_uniform_degrees_do_not_divide_by_zero(void)
{
    /* A vault where every hub has the same degree is not exotic — it is what a
     * small, tidy vault looks like. The radius scale divides by (hi - lo). */
    vault_t v;
    memset(&v, 0, sizeof(v));
    v.valid = true;
    v.node_count = 6;
    for (int i = 0; i < 6; i++) {
        snprintf(v.nodes[i].title, sizeof(v.nodes[i].title), "n%d", i);
        v.nodes[i].deg = 4;
    }
    graph_pos_t p[VAULT_NODES_MAX];
    int n = ui_graph_layout(&v, 620, 360, p, VAULT_NODES_MAX);
    CHECK_INT(n, 6);
    for (int i = 0; i < n; i++) CHECK(p[i].r > 0);
    check_inside("uniform", p, n, 620, 360);
}

static void test_degenerate_inputs(void)
{
    vault_t v;
    make(&v, 4, 10);
    graph_pos_t p[VAULT_NODES_MAX];

    CHECK_INT(ui_graph_layout(NULL, 620, 360, p, VAULT_NODES_MAX), 0);
    CHECK_INT(ui_graph_layout(&v, 620, 360, NULL, VAULT_NODES_MAX), 0);
    CHECK_INT(ui_graph_layout(&v, 0, 360, p, VAULT_NODES_MAX), 0);
    CHECK_INT(ui_graph_layout(&v, 620, 0, p, VAULT_NODES_MAX), 0);
    CHECK_INT(ui_graph_layout(&v, 620, 360, p, 0), 0);

    /* An empty graph is a normal state (a brand-new vault), not an error. */
    vault_t empty;
    memset(&empty, 0, sizeof(empty));
    empty.valid = true;
    CHECK_INT(ui_graph_layout(&empty, 620, 360, p, VAULT_NODES_MAX), 0);

    /* Asking for fewer than there are must truncate, not overflow. */
    CHECK_INT(ui_graph_layout(&v, 620, 360, p, 2), 2);
}

static void test_nodes_do_not_all_land_on_one_spot(void)
{
    /* The ring arithmetic is integer; a bad step could collapse a whole ring
     * onto a single coordinate, which reads as "the graph did not render". */
    vault_t v;
    make(&v, 14, 24);
    graph_pos_t p[VAULT_NODES_MAX];
    int n = ui_graph_layout(&v, 620, 360, p, VAULT_NODES_MAX);

    int collisions = 0;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (p[i].x == p[j].x && p[i].y == p[j].y) collisions++;
        }
    }
    CHECK_INT(collisions, 0);
}

int main(void)
{
    test_everything_stays_on_the_canvas();
    test_layout_is_deterministic();
    test_biggest_hub_is_the_centre();
    test_radius_tracks_degree();
    test_uniform_degrees_do_not_divide_by_zero();
    test_degenerate_inputs();
    test_nodes_do_not_all_land_on_one_spot();
    TH_REPORT("graph_layout");
}
