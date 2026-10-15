#include "graph_layout.h"

typedef struct {
    guint count, time, components;
    gint *index;
    guint *low, *component;
    gboolean *on_stack;
    GArray *stack;
    const GArray *edges;
} Search;

static void visit(Search *s, guint node)
{
    s->index[node] = (gint)s->time;
    s->low[node] = s->time++;
    s->on_stack[node] = TRUE;
    g_array_append_val(s->stack, node);
    for (guint i = 0; i < s->edges->len; i++) {
        LayoutEdge e = g_array_index(s->edges, LayoutEdge, i);
        if (e.from != node || e.to >= s->count) continue;
        if (s->index[e.to] < 0) { visit(s, e.to); s->low[node] = MIN(s->low[node], s->low[e.to]); }
        else if (s->on_stack[e.to]) s->low[node] = MIN(s->low[node], (guint)s->index[e.to]);
    }
    if (s->low[node] != (guint)s->index[node]) return;
    guint popped;
    do {
        popped = g_array_index(s->stack, guint, s->stack->len - 1);
        g_array_set_size(s->stack, s->stack->len - 1);
        s->on_stack[popped] = FALSE;
        s->component[popped] = s->components;
    } while (popped != node);
    s->components++;
}

typedef struct { double score; guint first; } Order;
static gint compare_order(gconstpointer a, gconstpointer b, gpointer data)
{
    const Order *order = data;
    guint x = *(const guint *)a, y = *(const guint *)b;
    if (order[x].score != order[y].score) return order[x].score < order[y].score ? -1 : 1;
    return order[x].first < order[y].first ? -1 : order[x].first != order[y].first;
}

GraphLayout *graph_layout_build(guint count, const GArray *edges)
{
    count = MIN(count, GRAPH_LAYOUT_LIMIT);
    GraphLayout *result = g_new0(GraphLayout, 1);
    result->nodes = g_array_sized_new(FALSE, TRUE, sizeof(LayoutNode), count);
    g_array_set_size(result->nodes, count);
    result->width = 280; result->height = 160;
    if (!count) return result;
    g_autofree gint *index = g_new(gint, count);
    g_autofree guint *low = g_new0(guint, count), *component = g_new0(guint, count);
    g_autofree gboolean *on_stack = g_new0(gboolean, count);
    g_autoptr(GArray) stack = g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint i = 0; i < count; i++) index[i] = -1;
    Search s = {count, 0, 0, index, low, component, on_stack, stack, edges};
    for (guint i = 0; i < count; i++) if (index[i] < 0) visit(&s, i);
    guint n = s.components;
    g_autofree guint *size = g_new0(guint, n), *rank = g_new0(guint, n), *degree = g_new0(guint, n);
    g_autofree gboolean *cycle = g_new0(gboolean, n);
    g_autofree Order *order = g_new0(Order, n);
    g_autofree double *position = g_new0(double, n);
    for (guint c = 0; c < n; c++) order[c].first = G_MAXUINT;
    for (guint i = 0; i < count; i++) { size[component[i]]++; order[component[i]].first = MIN(order[component[i]].first, i); }
    g_autoptr(GArray) dag = g_array_new(FALSE, FALSE, sizeof(LayoutEdge));
    for (guint i = 0; i < edges->len; i++) {
        LayoutEdge e = g_array_index(edges, LayoutEdge, i);
        if (e.from >= count || e.to >= count) continue;
        guint a = component[e.from], b = component[e.to];
        if (a == b) { cycle[a] = TRUE; continue; }
        gboolean duplicate = FALSE;
        for (guint j = 0; j < dag->len; j++) {
            LayoutEdge old = g_array_index(dag, LayoutEdge, j);
            if (old.from == a && old.to == b) { duplicate = TRUE; break; }
        }
        if (!duplicate) { LayoutEdge ce = {a,b}; g_array_append_val(dag, ce); degree[b]++; }
    }
    GQueue ready = G_QUEUE_INIT;
    for (guint c = 0; c < n; c++) {
        if (!degree[c]) g_queue_push_tail(&ready, GUINT_TO_POINTER(c + 1));
        if (cycle[c]) result->cycles++;
    }
    guint levels = 1;
    while (!g_queue_is_empty(&ready)) {
        guint c = GPOINTER_TO_UINT(g_queue_pop_head(&ready)) - 1;
        for (guint i = 0; i < dag->len; i++) {
            LayoutEdge e = g_array_index(dag, LayoutEdge, i);
            if (e.from != c) continue;
            rank[e.to] = MAX(rank[e.to], rank[c] + 1);
            levels = MAX(levels, rank[e.to] + 1);
            if (!--degree[e.to]) g_queue_push_tail(&ready, GUINT_TO_POINTER(e.to + 1));
        }
    }
    g_autoptr(GPtrArray) layers = g_ptr_array_new_with_free_func((GDestroyNotify)g_array_unref);
    for (guint l = 0; l < levels; l++) g_ptr_array_add(layers, g_array_new(FALSE, FALSE, sizeof(guint)));
    for (guint c = 0; c < n; c++) {
        GArray *layer = g_ptr_array_index(layers, rank[c]);
        g_array_append_val(layer, c); order[c].score = order[c].first;
    }
    /* Stable barycentric sweeps order connected components without splitting cycles. */
    for (guint sweep = 0; sweep < 7; sweep++) {
        for (guint step = 0; step < levels; step++) {
            guint l = sweep % 2 ? levels - step - 1 : step;
            GArray *layer = g_ptr_array_index(layers, l);
            for (guint j = 0; j < layer->len; j++) {
                guint c = g_array_index(layer, guint, j);
                if (sweep) {
                    double sum = 0; guint neighbors = 0;
                    for (guint k = 0; k < dag->len; k++) {
                        LayoutEdge e = g_array_index(dag, LayoutEdge, k);
                        if (!(sweep % 2) && e.to == c) { sum += position[e.from]; neighbors++; }
                        if (sweep % 2 && e.from == c) { sum += position[e.to]; neighbors++; }
                    }
                    order[c].score = neighbors ? sum / neighbors : position[c];
                }
            }
            g_array_sort_with_data(layer, compare_order, order);
            double row = 0;
            for (guint j = 0; j < layer->len; j++) {
                guint c = g_array_index(layer, guint, j);
                position[c] = row + (size[c] - 1) / 2.0;
                row += size[c];
            }
        }
    }
    guint max_rows = 1;
    for (guint l = 0; l < levels; l++) {
        GArray *layer = g_ptr_array_index(layers, l);
        guint row = 0;
        for (guint j = 0; j < layer->len; j++) {
            guint c = g_array_index(layer, guint, j);
            for (guint i = 0; i < count; i++) if (component[i] == c) {
                LayoutNode node = {40 + l * 280.0, 64 + row++ * 100.0, l, c, cycle[c]};
                g_array_index(result->nodes, LayoutNode, i) = node;
            }
        }
        max_rows = MAX(max_rows, row);
    }
    result->width = 80 + (levels - 1) * 280.0 + 200;
    result->height = 100 + (max_rows - 1) * 100.0 + 62;
    return result;
}

void graph_layout_group(GraphLayout *layout, const char *const *groups)
{
    if (!layout || !groups) return;
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    double top = 80;
    for (guint first = 0; first < layout->nodes->len; first++) {
        const char *group = groups[first];
        if (g_hash_table_contains(seen, group)) continue;
        g_hash_table_add(seen, (gpointer)group);
        /* Input positions already contain the crossing-reduced order. */
        double bottom = top;
        /* Store the new order separately before changing any positions. */
        g_autofree double *positions = g_new(double, layout->nodes->len);
        for (guint i = 0; i < layout->nodes->len; i++) {
            if (!g_str_equal(groups[i], group)) continue;
            LayoutNode node = g_array_index(layout->nodes, LayoutNode, i);
            guint row = 0;
            for (guint j = 0; j < layout->nodes->len; j++) {
                LayoutNode other = g_array_index(layout->nodes, LayoutNode, j);
                if (g_str_equal(groups[j], group) && other.layer == node.layer && other.y < node.y) row++;
            }
            positions[i] = top + row * 100.0;
            bottom = MAX(bottom, positions[i] + 62);
        }
        for (guint i = 0; i < layout->nodes->len; i++)
            if (g_str_equal(groups[i], group)) g_array_index(layout->nodes, LayoutNode, i).y = positions[i];
        top = bottom + 86;
    }
    layout->height = MAX(160, top - 40);
}

void graph_layout_free(GraphLayout *layout)
{
    if (!layout) return;
    g_array_unref(layout->nodes); g_free(layout);
}
