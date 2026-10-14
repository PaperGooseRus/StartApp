#include "graph.h"
#include "graph_layout.h"
#include "theme.h"
#include <math.h>

typedef struct {
    gint id;
    char *title, *subtitle, *group;
    GArray *members;
} Node;

struct Graph {
    GtkWidget *area;
    const Project *project;
    gint selected, selected_target, hovered;
    gboolean target_mode, overview, grouped, fit_pending, drag_moved;
    GPtrArray *nodes;
    GArray *edges;
    GraphLayout *layout;
    GHashTable *expanded;
    GraphOpenFile open_file;
    GraphSelectTarget select_target;
    gpointer user_data;
    guint omitted;
    double zoom, offset_x, offset_y, drag_x, drag_y, mouse_x, mouse_y;
};

static void node_free(gpointer data)
{
    Node *node = data;
    g_free(node->title); g_free(node->subtitle); g_free(node->group);
    g_array_unref(node->members); g_free(node);
}

static gint selected(Graph *graph) { return graph->target_mode ? graph->selected_target : graph->selected; }

static void project_edge(Graph *graph, guint index, guint *from, gint *to)
{
    if (graph->target_mode) {
        ProjectTargetEdge *edge = g_ptr_array_index(graph->project->target_edges, index);
        *from = edge->from; *to = (gint)edge->to;
    } else {
        ProjectEdge *edge = g_ptr_array_index(graph->project->edges, index);
        *from = edge->from; *to = edge->to;
    }
}

static void rebuild(Graph *graph)
{
    g_ptr_array_set_size(graph->nodes, 0); g_array_set_size(graph->edges, 0);
    graph_layout_free(graph->layout); graph->layout = NULL;
    graph->hovered = -1; graph->omitted = 0; graph->fit_pending = TRUE;
    if (!graph->project) { gtk_widget_queue_draw(graph->area); return; }
    guint count = graph->target_mode ? graph->project->targets->len : graph->project->files->len;
    guint edges = graph->target_mode ? graph->project->target_edges->len : graph->project->edges->len;
    g_autofree gboolean *visible = g_new0(gboolean, count);
    gint focus = selected(graph);
    gboolean overview = graph->overview || focus < 0;
    if (!overview && (guint)focus < count) {
        visible[focus] = TRUE;
        for (guint i = 0; i < edges; i++) {
            guint from; gint to; project_edge(graph, i, &from, &to);
            if (to < 0 || from >= count || (guint)to >= count) continue;
            if ((gint)from == focus) visible[to] = TRUE;
            if (to == focus) visible[from] = TRUE;
        }
    } else for (guint i = 0; i < count; i++) {
        visible[i] = graph->target_mode || ((ProjectFile *)g_ptr_array_index(graph->project->files, i))->source;
    }
    g_autofree gint *map = g_new(gint, count);
    for (guint i = 0; i < count; i++) map[i] = -1;
    g_autoptr(GHashTable) groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint step = 0; step < count + (!overview ? 1u : 0u); step++) {
        guint i = !overview && step == 0 ? (guint)focus : step - (!overview ? 1u : 0u);
        if (i >= count || !visible[i] || map[i] >= 0) continue;
        g_autofree char *group = NULL, *title = NULL, *subtitle = NULL;
        if (graph->target_mode) {
            ProjectTarget *target = g_ptr_array_index(graph->project->targets, i);
            group = g_strdup(target->type); title = g_strdup(target->name); subtitle = g_strdup(target->type);
        } else {
            ProjectFile *file = g_ptr_array_index(graph->project->files, i);
            group = g_path_get_dirname(file->path); title = g_path_get_basename(file->path);
            subtitle = g_strdup(file->path);
        }
        gboolean collapse = overview && graph->grouped && !g_hash_table_contains(graph->expanded, group);
        guint encoded = collapse ? GPOINTER_TO_UINT(g_hash_table_lookup(groups, group)) : 0;
        if (encoded) {
            Node *node = g_ptr_array_index(graph->nodes, encoded - 1);
            g_array_append_val(node->members, i); map[i] = (gint)encoded - 1; continue;
        }
        if (graph->nodes->len >= GRAPH_LAYOUT_LIMIT) { graph->omitted++; continue; }
        Node *node = g_new0(Node, 1);
        node->id = collapse ? -1 : (gint)i;
        node->title = collapse ? g_strdup(g_str_equal(group, ".") ? "Корень проекта" : group) : g_steal_pointer(&title);
        node->subtitle = g_steal_pointer(&subtitle); node->group = g_strdup(group);
        node->members = g_array_new(FALSE, FALSE, sizeof(guint)); g_array_append_val(node->members, i);
        map[i] = (gint)graph->nodes->len; g_ptr_array_add(graph->nodes, node);
        if (collapse) g_hash_table_insert(groups, g_strdup(group), GUINT_TO_POINTER(graph->nodes->len));
    }
    for (guint i = 0; i < graph->nodes->len; i++) {
        Node *node = g_ptr_array_index(graph->nodes, i);
        if (node->id < 0) {
            g_free(node->subtitle);
            node->subtitle = g_strdup_printf("%u %s · нажмите для раскрытия", node->members->len,
                graph->target_mode ? "целей" : "файлов");
        }
    }
    g_autoptr(GHashTable) unique = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < edges; i++) {
        guint from; gint to; project_edge(graph, i, &from, &to);
        if (to < 0 || from >= count || (guint)to >= count || map[from] < 0 || map[to] < 0) continue;
        if (map[from] == map[to] && ((Node *)g_ptr_array_index(graph->nodes, (guint)map[from]))->id < 0) continue;
        guint key = (guint)map[from] * GRAPH_LAYOUT_LIMIT + (guint)map[to] + 1;
        if (g_hash_table_contains(unique, GUINT_TO_POINTER(key))) continue;
        g_hash_table_add(unique, GUINT_TO_POINTER(key));
        LayoutEdge edge = {(guint)map[from], (guint)map[to]}; g_array_append_val(graph->edges, edge);
    }
    graph->layout = graph_layout_build(graph->nodes->len, graph->edges);
    gtk_widget_queue_draw(graph->area);
}

void graph_fit(Graph *graph)
{
    if (!graph->layout) return;
    double w = MAX(1, gtk_widget_get_width(graph->area) - 24);
    double h = MAX(1, gtk_widget_get_height(graph->area) - 64);
    graph->zoom = CLAMP(MIN(w / graph->layout->width, h / graph->layout->height), .03, 1.0);
    graph->offset_x = (w + 24 - graph->layout->width * graph->zoom) / 2;
    graph->offset_y = 56 + (h - graph->layout->height * graph->zoom) / 2;
    graph->fit_pending = FALSE; gtk_widget_queue_draw(graph->area);
}

static void resized(GtkDrawingArea *area, int width, int height, gpointer data)
{ (void)area; (void)width; (void)height; graph_fit(data); }

void graph_zoom_at(Graph *graph, double factor, double x, double y)
{
    if (graph->fit_pending) graph_fit(graph);
    double next = CLAMP(graph->zoom * factor, .03, 3.0), ratio = next / graph->zoom;
    graph->offset_x = x - (x - graph->offset_x) * ratio; graph->offset_y = y - (y - graph->offset_y) * ratio;
    graph->zoom = next; graph->fit_pending = FALSE; gtk_widget_queue_draw(graph->area);
}
void graph_zoom_by(Graph *graph, double factor)
{ graph_zoom_at(graph, factor, gtk_widget_get_width(graph->area) / 2.0, gtk_widget_get_height(graph->area) / 2.0); }
void graph_pan(Graph *graph, double dx, double dy)
{
    if (graph->fit_pending) graph_fit(graph);
    graph->offset_x += dx; graph->offset_y += dy; gtk_widget_queue_draw(graph->area);
}
double graph_get_zoom(Graph *graph) { return graph->zoom; }
guint graph_visible_count(Graph *graph) { return graph->nodes->len; }

static gint hit(Graph *graph, double x, double y)
{
    if (!graph->layout || y < 52) return -1;
    if (graph->fit_pending) graph_fit(graph);
    x = (x - graph->offset_x) / graph->zoom; y = (y - graph->offset_y) / graph->zoom;
    for (guint i = 0; i < graph->nodes->len; i++) {
        LayoutNode node = g_array_index(graph->layout->nodes, LayoutNode, i);
        if (x >= node.x && x <= node.x + 200 && y >= node.y && y <= node.y + 62) return (gint)i;
    }
    return -1;
}
gboolean graph_node_center(Graph *graph, guint index, double *x, double *y)
{
    if (!graph->layout) return FALSE;
    if (graph->fit_pending) graph_fit(graph);
    for (guint i = 0; i < graph->nodes->len; i++) {
        Node *node = g_ptr_array_index(graph->nodes, i);
        if (node->id != (gint)index) continue;
        LayoutNode pos = g_array_index(graph->layout->nodes, LayoutNode, i);
        *x = graph->offset_x + (pos.x + 100) * graph->zoom; *y = graph->offset_y + (pos.y + 31) * graph->zoom;
        return TRUE;
    }
    return FALSE;
}
gboolean graph_activate_at(Graph *graph, double x, double y)
{
    gint found = hit(graph, x, y); if (found < 0) return FALSE;
    Node *node = g_ptr_array_index(graph->nodes, (guint)found);
    if (node->id < 0) { g_hash_table_add(graph->expanded, g_strdup(node->group)); rebuild(graph); }
    else if (graph->target_mode) graph->select_target((guint)node->id, graph->user_data);
    else graph->open_file((guint)node->id, graph->user_data);
    return TRUE;
}

static void text(cairo_t *cr, const char *value, double x, double y, double width,
                 int size, gboolean bold, ThemeColor color)
{
    PangoLayout *label = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_new();
    pango_font_description_set_family(font, "Sans"); pango_font_description_set_size(font, size * PANGO_SCALE);
    pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(label, font); pango_layout_set_text(label, value, -1);
    pango_layout_set_width(label, (int)(MAX(1, width) * PANGO_SCALE)); pango_layout_set_ellipsize(label, PANGO_ELLIPSIZE_MIDDLE);
    theme_cairo(cr, color, 1); cairo_move_to(cr, x, y); pango_cairo_show_layout(cr, label);
    pango_font_description_free(font); g_object_unref(label);
}
static void rounded(cairo_t *cr, double x, double y, double w, double h)
{
    const double r = 9;
    cairo_new_sub_path(cr); cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2); cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, G_PI * 1.5); cairo_close_path(cr);
}
static gboolean adjacent(Graph *graph, guint node, gint focus)
{
    if (focus < 0 || (gint)node == focus) return TRUE;
    for (guint i = 0; i < graph->edges->len; i++) {
        LayoutEdge e = g_array_index(graph->edges, LayoutEdge, i);
        if (((gint)e.from == focus && e.to == node) || ((gint)e.to == focus && e.from == node)) return TRUE;
    }
    return FALSE;
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    (void)area; Graph *graph = data;
    theme_cairo(cr, THEME_BACKGROUND, 1); cairo_paint(cr);
    if (graph->fit_pending) graph_fit(graph);
    theme_cairo(cr, THEME_MUTED, .13);
    for (int x = 12; x < width; x += 24) for (int y = 60; y < height; y += 24) {
        cairo_arc(cr, x, y, .7, 0, 2 * G_PI); cairo_fill(cr);
    }
    if (!graph->layout || !graph->nodes->len) {
        text(cr, "Карта зависимостей", 24, 76, width - 48, 17, TRUE, THEME_TEXT);
        text(cr, graph->target_mode ? "Нужен готовый CMake File API reply." : "Выберите проект или файл.",
            24, 116, width - 48, 11, FALSE, THEME_MUTED); return;
    }
    gint focus = graph->hovered;
    if (focus < 0 && selected(graph) >= 0) for (guint i = 0; i < graph->nodes->len; i++)
        if (((Node *)g_ptr_array_index(graph->nodes, i))->id == selected(graph)) { focus = (gint)i; break; }
    cairo_save(cr); cairo_rectangle(cr, 0, 52, width, MAX(0,height - 52)); cairo_clip(cr);
    cairo_translate(cr, graph->offset_x, graph->offset_y); cairo_scale(cr, graph->zoom, graph->zoom);
    for (guint i = 0; i < graph->nodes->len; i++) {
        LayoutNode pos = g_array_index(graph->layout->nodes, LayoutNode, i);
        if (!pos.cycle) continue;
        gboolean first = TRUE; double top = pos.y, bottom = pos.y + 62;
        for (guint j = 0; j < graph->nodes->len; j++) {
            LayoutNode other = g_array_index(graph->layout->nodes, LayoutNode, j);
            if (other.component != pos.component) continue;
            if (j < i) first = FALSE;
            top = MIN(top, other.y); bottom = MAX(bottom, other.y + 62);
        }
        if (!first) continue;
        rounded(cr, pos.x - 14, top - 14, 228, bottom - top + 28);
        double dash[] = {5,4}; cairo_set_dash(cr, dash, 2, 0);
        theme_cairo(cr, THEME_ACCENT, .6); cairo_set_line_width(cr, 1); cairo_stroke(cr); cairo_set_dash(cr, NULL, 0, 0);
    }
    for (guint i = 0; i < graph->edges->len; i++) {
        LayoutEdge e = g_array_index(graph->edges, LayoutEdge, i);
        LayoutNode a = g_array_index(graph->layout->nodes, LayoutNode, e.from), b = g_array_index(graph->layout->nodes, LayoutNode, e.to);
        gboolean active = focus >= 0 && ((gint)e.from == focus || (gint)e.to == focus);
        theme_cairo(cr, active ? THEME_ACCENT : THEME_MUTED, active ? .95 : focus >= 0 ? .13 : .4);
        cairo_set_line_width(cr, active ? 2.2 : 1.2); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        double end_x = b.x, end_y = b.y + 31;
        if (a.layer == b.layer) {
            double channel = a.x - 20 - (i % 4) * 5;
            cairo_move_to(cr, a.x, a.y + 31); cairo_line_to(cr, channel, a.y + 31);
            if (e.from == e.to) {
                cairo_line_to(cr, channel, a.y - 8); cairo_line_to(cr, a.x + 30, a.y - 8);
                end_x = a.x + 30; end_y = a.y; cairo_line_to(cr, end_x, end_y);
            } else { cairo_line_to(cr, channel, end_y); cairo_line_to(cr, end_x, end_y); }
        } else {
            double start_x = a.x + 200, start_y = a.y + 31; cairo_move_to(cr, start_x, start_y);
            if (b.layer == a.layer + 1) {
                double middle = (start_x + end_x) / 2 + ((gint)(i % 5) - 2) * 5;
                cairo_curve_to(cr, middle, start_y, middle, end_y, end_x, end_y);
            } else {
                /* Skip-level links use a corridor above the cards. */
                double lane = 8 + (i % 7) * 6;
                cairo_line_to(cr, start_x + 18, start_y); cairo_line_to(cr, start_x + 18, lane);
                cairo_line_to(cr, end_x - 18, lane); cairo_line_to(cr, end_x - 18, end_y); cairo_line_to(cr, end_x, end_y);
            }
        }
        cairo_stroke(cr); cairo_move_to(cr, end_x, end_y);
        if (e.from == e.to) { cairo_line_to(cr, end_x - 4, end_y - 6); cairo_line_to(cr, end_x + 4, end_y - 6); }
        else { cairo_line_to(cr, end_x - 6, end_y - 4); cairo_line_to(cr, end_x - 6, end_y + 4); }
        cairo_close_path(cr); cairo_fill(cr);
    }
    for (guint i = 0; i < graph->nodes->len; i++) {
        Node *node = g_ptr_array_index(graph->nodes, i); LayoutNode pos = g_array_index(graph->layout->nodes, LayoutNode, i);
        gboolean active = node->id >= 0 && node->id == selected(graph), dim = !adjacent(graph, i, focus);
        cairo_save(cr); if (dim) cairo_push_group(cr);
        rounded(cr, pos.x, pos.y, 200, 62); theme_cairo(cr, active ? THEME_SELECTION : THEME_SURFACE, 1); cairo_fill_preserve(cr);
        theme_cairo(cr, active || (gint)i == graph->hovered ? THEME_ACCENT : THEME_BORDER, 1);
        cairo_set_line_width(cr, active ? 2 : 1); cairo_stroke(cr);
        text(cr, node->title, pos.x + 12, pos.y + 9, 176, 11, TRUE, THEME_TEXT);
        g_autofree char *subtitle = pos.cycle ? g_strdup_printf("↻ цикл · %s", node->subtitle) : g_strdup(node->subtitle);
        text(cr, subtitle, pos.x + 12, pos.y + 35, 176, 9, FALSE, THEME_MUTED);
        if (dim) { cairo_pop_group_to_source(cr); cairo_paint_with_alpha(cr, .4); } cairo_restore(cr);
    }
    cairo_restore(cr);
    theme_cairo(cr, THEME_BACKGROUND, 1); cairo_rectangle(cr, 0, 0, width, 52); cairo_fill(cr);
    g_autofree char *title = g_strdup_printf("%s · %u узлов · %u связей · %.0f%%", graph->target_mode ? "Цели CMake" : "Включения C/C++",
        graph->nodes->len, graph->edges->len, graph->zoom * 100);
    text(cr, title, 12, 7, width - 24, 11, TRUE, THEME_TEXT);
    g_autofree char *hint = graph->omitted ? g_strdup_printf("%u узлов скрыто · сузьте выбор или включите группы", graph->omitted) :
        g_strdup("Колесо: масштаб · перетаскивание: карта · ПКМ: свернуть группу");
    text(cr, hint, 12, 30, width - 24, 9, FALSE, THEME_MUTED);
}

static void released(GtkGestureClick *gesture, int presses, double x, double y, gpointer data)
{
    (void)presses; Graph *graph = data;
    if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) == GDK_BUTTON_SECONDARY) {
        gint found = hit(graph, x, y);
        if (found >= 0) { Node *node = g_ptr_array_index(graph->nodes, (guint)found); g_hash_table_remove(graph->expanded, node->group); rebuild(graph); }
    } else if (!graph->drag_moved) graph_activate_at(graph, x, y);
}
static void drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data)
{
    (void)gesture; (void)x; (void)y; Graph *graph = data;
    graph->drag_x = graph->offset_x; graph->drag_y = graph->offset_y; graph->drag_moved = FALSE;
}
static void drag_update(GtkGestureDrag *gesture, double x, double y, gpointer data)
{
    (void)gesture; Graph *graph = data;
    if (fabs(x) + fabs(y) > 4) graph->drag_moved = TRUE;
    graph->offset_x = graph->drag_x + x; graph->offset_y = graph->drag_y + y; gtk_widget_queue_draw(graph->area);
}
static gboolean scroll_event(GtkEventControllerScroll *controller, double dx, double dy, gpointer data)
{
    (void)controller; Graph *graph = data;
    graph_zoom_at(graph, pow(1.15, -(dy ? dy : dx)), graph->mouse_x, graph->mouse_y); return TRUE;
}
static void motion(GtkEventControllerMotion *controller, double x, double y, gpointer data)
{
    (void)controller; Graph *graph = data; graph->mouse_x = x; graph->mouse_y = y;
    gint found = hit(graph, x, y);
    if (found != graph->hovered) { graph->hovered = found; gtk_widget_queue_draw(graph->area); }
    const char *tooltip = found >= 0 ? ((Node *)g_ptr_array_index(graph->nodes, (guint)found))->subtitle : NULL;
    gtk_widget_set_tooltip_text(graph->area, tooltip); gtk_widget_set_cursor_from_name(graph->area, found >= 0 ? "pointer" : "grab");
}
static void leave(GtkEventControllerMotion *controller, gpointer data)
{ (void)controller; Graph *graph = data; graph->hovered = -1; gtk_widget_queue_draw(graph->area); }

Graph *graph_new(GraphOpenFile open_file, GraphSelectTarget select_target, gpointer user_data)
{
    Graph *graph = g_new0(Graph, 1); graph->selected = graph->selected_target = graph->hovered = -1; graph->zoom = 1;
    graph->nodes = g_ptr_array_new_with_free_func(node_free); graph->edges = g_array_new(FALSE, FALSE, sizeof(LayoutEdge));
    graph->expanded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    graph->open_file = open_file; graph->select_target = select_target; graph->user_data = user_data; graph->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(graph->area, 240, 180); gtk_widget_set_hexpand(graph->area, TRUE); gtk_widget_set_vexpand(graph->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(graph->area), draw, graph, NULL); g_signal_connect(graph->area, "resize", G_CALLBACK(resized), graph);
    GtkGesture *click = gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "released", G_CALLBACK(released), graph); gtk_widget_add_controller(graph->area, GTK_EVENT_CONTROLLER(click));
    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), graph); g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), graph);
    gtk_widget_add_controller(graph->area, GTK_EVENT_CONTROLLER(drag)); gtk_gesture_group(click, drag);
    GtkEventController *hover = gtk_event_controller_motion_new();
    g_signal_connect(hover, "motion", G_CALLBACK(motion), graph); g_signal_connect(hover, "leave", G_CALLBACK(leave), graph);
    gtk_widget_add_controller(graph->area, hover);
    GtkEventController *wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(wheel, "scroll", G_CALLBACK(scroll_event), graph); gtk_widget_add_controller(graph->area, wheel);
    return graph;
}
GtkWidget *graph_widget(Graph *graph) { return graph->area; }
void graph_set_project(Graph *graph, const Project *project, gint index)
{
    if (graph->project != project) { graph->selected_target = -1; g_hash_table_remove_all(graph->expanded); }
    graph->project = project; graph->selected = index; rebuild(graph);
}
void graph_set_target_mode(Graph *graph, gboolean active) { graph->target_mode = active; rebuild(graph); }
void graph_select_target(Graph *graph, gint index) { graph->selected_target = index; rebuild(graph); }
void graph_set_overview(Graph *graph, gboolean overview) { graph->overview = overview; rebuild(graph); }
void graph_set_grouped(Graph *graph, gboolean grouped)
{ graph->grouped = grouped; g_hash_table_remove_all(graph->expanded); rebuild(graph); }
void graph_free(Graph *graph)
{
    graph_layout_free(graph->layout); g_ptr_array_unref(graph->nodes); g_array_unref(graph->edges);
    g_hash_table_unref(graph->expanded); g_free(graph);
}
