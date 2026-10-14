#include "graph.h"
#include "theme.h"
#include <math.h>

typedef struct {
    guint file;
    double x, y, width, height;
} Node;

struct Graph {
    GtkWidget *area;
    const Project *project;
    gint selected;
    gint selected_target;
    gboolean target_mode;
    gboolean overview;
    GArray *nodes;
    GraphOpenFile open_file;
    GraphSelectTarget select_target;
    gpointer user_data;
    guint omitted;
};

static void update_height(Graph *graph);

static void resized(GtkDrawingArea *area, int width, int height, gpointer data)
{
    (void)area; (void)width; (void)height;
    update_height(data);
}

static Node *find_node(Graph *graph, guint file)
{
    for (guint i = 0; i < graph->nodes->len; i++) {
        Node *node = &g_array_index(graph->nodes, Node, i);
        if (node->file == file) return node;
    }
    return NULL;
}

static void add_node(Graph *graph, guint file, double x, double y, double width)
{
    if (find_node(graph, file)) return;
    Node node = {file, x, y, width, 66};
    g_array_append_val(graph->nodes, node);
}

static void layout(Graph *graph, int width)
{
    g_array_set_size(graph->nodes, 0);
    graph->omitted = 0;
    if (!graph->project) return;
    guint columns = width >= 620 ? 3 : width >= 380 ? 2 : 1;
    double column_width = MAX(100.0, (width - 48.0 - 24.0 * (columns - 1)) / columns);
    gint selection = graph->target_mode ? graph->selected_target : graph->selected;
    if (width < 620 && !graph->overview && selection >= 0) {
        add_node(graph, (guint)selection, 24, 88, MAX(100.0, width - 48.0));
        guint incoming = 0, outgoing = 0;
        guint count = graph->target_mode ? graph->project->target_edges->len : graph->project->edges->len;
        for (guint i = 0; i < count; i++) {
            guint from;
            gint to;
            if (graph->target_mode) {
                ProjectTargetEdge *edge = g_ptr_array_index(graph->project->target_edges, i);
                from = edge->from; to = (gint)edge->to;
            } else {
                ProjectEdge *edge = g_ptr_array_index(graph->project->edges, i);
                from = edge->from; to = edge->to;
            }
            if (to < 0 || from == (guint)to) continue;
            if ((gint)from == selection && !find_node(graph, (guint)to)) {
                if (outgoing >= 20) { graph->omitted++; continue; }
                add_node(graph, (guint)to, 24 + (columns - 1) * (column_width + 24),
                    218 + (columns == 1 ? incoming + outgoing : outgoing) * 90, column_width);
                outgoing++;
            } else if (to == selection && !find_node(graph, from)) {
                if (incoming >= 20) { graph->omitted++; continue; }
                add_node(graph, from, 24, 218 + (columns == 1 ? incoming + outgoing : incoming) * 90, column_width);
                incoming++;
            }
        }
        return;
    }
    if (graph->target_mode) {
        if (graph->overview || graph->selected_target < 0) {
            for (guint i = 0; i < graph->project->targets->len; i++) {
                if (i >= 60) { graph->omitted++; continue; }
                add_node(graph, i, 24 + (i % columns) * (column_width + 24),
                         88 + (i / columns) * 100, column_width);
            }
        } else {
            add_node(graph, (guint)graph->selected_target, 48 + column_width, 132, column_width);
            guint incoming = 0, outgoing = 0;
            for (guint i = 0; i < graph->project->target_edges->len; i++) {
                ProjectTargetEdge *edge = g_ptr_array_index(graph->project->target_edges, i);
                if (edge->from == edge->to) continue;
                if ((gint)edge->from == graph->selected_target && !find_node(graph, edge->to)) {
                    if (outgoing >= 20) { graph->omitted++; continue; }
                    add_node(graph, edge->to, 72 + 2 * column_width, 88 + outgoing++ * 90, column_width);
                } else if ((gint)edge->to == graph->selected_target && !find_node(graph, edge->from)) {
                    if (incoming >= 20) { graph->omitted++; continue; }
                    add_node(graph, edge->from, 24, 88 + incoming++ * 90, column_width);
                }
            }
        }
        return;
    }
    if (graph->overview || graph->selected < 0) {
        guint count = 0;
        for (guint i = 0; i < graph->project->files->len; i++) {
            ProjectFile *file = g_ptr_array_index(graph->project->files, i);
            if (!file->source) continue;
            if (count >= 60) { graph->omitted++; continue; }
            add_node(graph, i, 24 + (count % columns) * (column_width + 24),
                     88 + (count / columns) * 100, column_width);
            count++;
        }
    } else {
        add_node(graph, (guint)graph->selected, 24 + column_width + 24, 132, column_width);
        guint incoming = 0, outgoing = 0;
        for (guint i = 0; i < graph->project->edges->len; i++) {
            ProjectEdge *edge = g_ptr_array_index(graph->project->edges, i);
            if (edge->to < 0 || edge->from == (guint)edge->to) continue;
            if ((gint)edge->from == graph->selected && !find_node(graph, (guint)edge->to)) {
                if (outgoing >= 20) { graph->omitted++; continue; }
                add_node(graph, (guint)edge->to, 24 + 2 * (column_width + 24),
                         88 + outgoing++ * 90, column_width);
            } else if (edge->to == graph->selected && !find_node(graph, edge->from)) {
                if (incoming >= 20) { graph->omitted++; continue; }
                add_node(graph, edge->from, 24, 88 + incoming++ * 90, column_width);
            }
        }
    }
}

static void text(cairo_t *cr, const char *value, double x, double y,
                 double width, int size, gboolean bold,
                 ThemeColor color)
{
    PangoLayout *label = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_new();
    pango_font_description_set_family(font, "Sans");
    pango_font_description_set_size(font, size * PANGO_SCALE);
    pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(label, font);
    pango_layout_set_text(label, value, -1);
    pango_layout_set_width(label, (int)(width * PANGO_SCALE));
    pango_layout_set_ellipsize(label, PANGO_ELLIPSIZE_MIDDLE);
    theme_cairo(cr, color, 1);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, label);
    pango_font_description_free(font);
    g_object_unref(label);
}

static void rounded(cairo_t *cr, double x, double y, double w, double h)
{
    const double radius = 10;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius, radius, -G_PI / 2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, G_PI / 2);
    cairo_arc(cr, x + radius, y + h - radius, radius, G_PI / 2, G_PI);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, G_PI * 1.5);
    cairo_close_path(cr);
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                 gpointer user_data)
{
    (void)area;
    Graph *graph = user_data;
    theme_cairo(cr, THEME_BACKGROUND, 1);
    cairo_paint(cr);
    theme_cairo(cr, THEME_MUTED, .16);
    for (int x = 16; x < width; x += 24)
        for (int y = 16; y < height; y += 24) {
            cairo_arc(cr, x, y, .8, 0, 2 * G_PI);
            cairo_fill(cr);
        }
    layout(graph, width);
    if (!graph->project || !graph->nodes->len) {
        text(cr, "Карта зависимостей", 32, 60, width - 64, 20, TRUE, THEME_TEXT);
        text(cr, graph->target_mode ? "Нет целей CMake · выберите каталог с готовым File API reply." :
             graph->project ? "В проекте пока нет файлов C/C++." :
             "Откройте папку проекта, чтобы увидеть связи между файлами.",
             32, 104, width - 64, 11, FALSE, THEME_MUTED);
        return;
    }
    gint selected_index = graph->target_mode ? graph->selected_target : graph->selected;
    if (graph->overview || selected_index < 0) {
        g_autofree char *title = graph->target_mode ?
            g_strdup_printf("CMake · цели сборки · %s", graph->project->configuration ? graph->project->configuration : "") :
            g_strdup("C / C++ · локальные включения");
        text(cr, title, 24, 24, width - 48, 12, TRUE, THEME_TEXT);
    } else if (width < 620) {
        text(cr, graph->target_mode ? "ВЫБРАННАЯ ЦЕЛЬ · СВЯЗИ НИЖЕ" : "ВЫБРАННЫЙ ФАЙЛ · СВЯЗИ НИЖЕ",
            24, 26, width - 48, 9, TRUE, THEME_ACCENT);
    } else {
        double col = MAX(150.0, ((double)width - 100.0) / 3.0);
        text(cr, graph->target_mode ? "ЗАВИСЯТ ОТ ЦЕЛИ" : "ИСПОЛЬЗУЮТ ФАЙЛ", 24, 26, col, 9, TRUE, THEME_MUTED);
        text(cr, graph->target_mode ? "ВЫБРАННАЯ ЦЕЛЬ" : "ВЫБРАННЫЙ ФАЙЛ", 48 + col, 26, col, 9, TRUE, THEME_ACCENT);
        text(cr, graph->target_mode ? "ЗАВИСИМОСТИ" : "ВКЛЮЧАЕТ", 72 + 2 * col, 26, col, 9, TRUE, THEME_MUTED);
    }
    guint edge_count = graph->target_mode ? graph->project->target_edges->len : graph->project->edges->len;
    for (guint i = 0; i < edge_count; i++) {
        guint from_index;
        gint to_index;
        if (graph->target_mode) {
            ProjectTargetEdge *edge = g_ptr_array_index(graph->project->target_edges, i);
            from_index = edge->from; to_index = (gint)edge->to;
        } else {
            ProjectEdge *edge = g_ptr_array_index(graph->project->edges, i);
            from_index = edge->from; to_index = edge->to;
        }
        if (to_index < 0) continue;
        Node *from = find_node(graph, from_index);
        Node *to = find_node(graph, (guint)to_index);
        if (!from || !to || from == to) continue;
        double start_x = from->x + from->width, start_y = from->y + from->height / 2;
        double end_x = to->x, end_y = to->y + to->height / 2;
        if (to->x <= from->x) {
            start_x = from->x + from->width / 2;
            start_y = from->y + from->height;
            end_x = to->x + to->width / 2;
            end_y = to->y;
        }
        gboolean active = (gint)from_index == selected_index || to_index == selected_index;
        theme_cairo(cr, active ? THEME_ACCENT : THEME_MUTED, active ? .85 : .4);
        cairo_set_line_width(cr, active ? 2 : 1.3);
        cairo_move_to(cr, start_x, start_y);
        if (to->x > from->x) {
            double middle = (start_x + end_x) / 2;
            cairo_curve_to(cr, middle, start_y, middle, end_y, end_x, end_y);
        } else cairo_curve_to(cr, start_x, start_y + 28, end_x, end_y - 28, end_x, end_y);
        cairo_stroke(cr);
        cairo_move_to(cr, end_x, end_y);
        if (to->x > from->x) {
            cairo_line_to(cr, end_x - 6, end_y - 4);
            cairo_line_to(cr, end_x - 6, end_y + 4);
        } else {
            cairo_line_to(cr, end_x - 4, end_y - 6);
            cairo_line_to(cr, end_x + 4, end_y - 6);
        }
        cairo_close_path(cr);
        cairo_fill(cr);
    }
    for (guint i = 0; i < graph->nodes->len; i++) {
        Node *node = &g_array_index(graph->nodes, Node, i);
        gboolean selected = (gint)node->file == selected_index;
        rounded(cr, node->x, node->y, node->width, node->height);
        theme_cairo(cr, selected ? THEME_SELECTION : THEME_SURFACE, 1);
        cairo_fill_preserve(cr);
        theme_cairo(cr, selected ? THEME_ACCENT : THEME_BORDER, 1);
        cairo_set_line_width(cr, selected ? 2 : 1);
        cairo_stroke(cr);
        g_autofree char *basename = NULL, *directory = NULL;
        if (graph->target_mode) {
            ProjectTarget *target = g_ptr_array_index(graph->project->targets, node->file);
            basename = g_strdup(target->name); directory = g_strdup(target->type);
        } else {
            ProjectFile *file = g_ptr_array_index(graph->project->files, node->file);
            basename = g_path_get_basename(file->path); directory = g_path_get_dirname(file->path);
        }
        text(cr, basename, node->x + 12, node->y + 10, node->width - 24, 11, TRUE, THEME_TEXT);
        text(cr, directory, node->x + 12, node->y + 37, node->width - 24, 9, FALSE, THEME_MUTED);
    }
    if (graph->omitted) {
        g_autofree char *notice = g_strdup_printf("Ещё %u узлов · %s", graph->omitted,
            graph->target_mode ? "выберите цель из списка" : "выберите файл в дереве для детализации");
        text(cr, notice, 24, 54, width - 48, 9, FALSE, THEME_ACCENT);
    }
}

gboolean graph_activate_at(Graph *graph, double x, double y)
{
    for (guint i = 0; i < graph->nodes->len; i++) {
        Node node = g_array_index(graph->nodes, Node, i);
        if (x >= node.x && x <= node.x + node.width &&
            y >= node.y && y <= node.y + node.height) {
            if (graph->target_mode) graph->select_target(node.file, graph->user_data);
            else graph->open_file(node.file, graph->user_data);
            return TRUE;
        }
    }
    return FALSE;
}

static void clicked(GtkGestureClick *gesture, int presses, double x, double y,
                    gpointer user_data)
{
    (void)gesture;
    (void)presses;
    graph_activate_at(user_data, x, y);
}

static void motion(GtkEventControllerMotion *controller, double x, double y,
                   gpointer user_data)
{
    (void)controller;
    Graph *graph = user_data;
    const char *tooltip = NULL;
    for (guint i = 0; i < graph->nodes->len; i++) {
        Node *node = &g_array_index(graph->nodes, Node, i);
        if (x >= node->x && x <= node->x + node->width &&
            y >= node->y && y <= node->y + node->height) {
            if (graph->target_mode) {
                ProjectTarget *target = g_ptr_array_index(graph->project->targets, node->file);
                tooltip = target->artifact ? target->artifact : target->name;
            } else {
                ProjectFile *file = g_ptr_array_index(graph->project->files, node->file);
                tooltip = file->path;
            }
            break;
        }
    }
    gtk_widget_set_tooltip_text(graph->area, tooltip);
    gtk_widget_set_cursor_from_name(graph->area, tooltip ? "pointer" : "default");
}

Graph *graph_new(GraphOpenFile open_file, GraphSelectTarget select_target, gpointer user_data)
{
    Graph *graph = g_new0(Graph, 1);
    graph->nodes = g_array_new(FALSE, FALSE, sizeof(Node));
    graph->selected = -1;
    graph->selected_target = -1;
    graph->open_file = open_file;
    graph->select_target = select_target;
    graph->user_data = user_data;
    graph->area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(graph->area), 280);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(graph->area), 500);
    gtk_widget_set_hexpand(graph->area, TRUE);
    gtk_widget_set_vexpand(graph->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(graph->area), draw, graph, NULL);
    g_signal_connect(graph->area, "resize", G_CALLBACK(resized), graph);
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(clicked), graph);
    gtk_widget_add_controller(graph->area, GTK_EVENT_CONTROLLER(click));
    GtkEventController *hover = gtk_event_controller_motion_new();
    g_signal_connect(hover, "motion", G_CALLBACK(motion), graph);
    gtk_widget_add_controller(graph->area, hover);
    return graph;
}

GtkWidget *graph_widget(Graph *graph) { return graph->area; }

static void update_height(Graph *graph)
{
    int width = gtk_widget_get_width(graph->area);
    layout(graph, width > 0 ? width : 660);
    double height = 500;
    for (guint i = 0; i < graph->nodes->len; i++) {
        Node *node = &g_array_index(graph->nodes, Node, i);
        height = MAX(height, node->y + node->height + 32);
    }
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(graph->area), (int)height);
    /* Drawing will rebuild hit boxes using the actual allocation. */
    g_array_set_size(graph->nodes, 0);
    gtk_widget_queue_draw(graph->area);
}

void graph_set_project(Graph *graph, const Project *project, gint selected)
{
    if (graph->project != project) graph->selected_target = -1;
    graph->project = project;
    graph->selected = selected;
    update_height(graph);
}

void graph_set_target_mode(Graph *graph, gboolean active)
{
    graph->target_mode = active;
    update_height(graph);
}

void graph_select_target(Graph *graph, gint index)
{
    graph->selected_target = index;
    update_height(graph);
}

void graph_set_overview(Graph *graph, gboolean overview)
{
    graph->overview = overview;
    update_height(graph);
}

void graph_free(Graph *graph)
{
    g_array_unref(graph->nodes);
    g_free(graph);
}
