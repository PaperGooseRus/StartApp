#include "graph.h"
#include <math.h>

typedef struct {
    guint file;
    double x, y, width, height;
} Node;

struct Graph {
    GtkWidget *area;
    const Project *project;
    gint selected;
    gboolean overview;
    GArray *nodes;
    GraphOpenFile open_file;
    gpointer user_data;
    guint omitted;
};

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
    double column_width = MAX(150.0, ((double)width - 100.0) / 3.0);
    if (graph->overview || graph->selected < 0) {
        guint count = 0;
        for (guint i = 0; i < graph->project->files->len; i++) {
            ProjectFile *file = g_ptr_array_index(graph->project->files, i);
            if (!file->source) continue;
            if (count >= 60) { graph->omitted++; continue; }
            add_node(graph, i, 24 + (count % 3) * (column_width + 24),
                     88 + (count / 3) * 100, column_width);
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
                 double r, double g, double b)
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
    cairo_set_source_rgb(cr, r, g, b);
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
    cairo_set_source_rgb(cr, .055, .075, .105);
    cairo_paint(cr);
    cairo_set_source_rgba(cr, .3, .4, .55, .18);
    for (int x = 16; x < width; x += 24)
        for (int y = 16; y < height; y += 24) {
            cairo_arc(cr, x, y, .8, 0, 2 * G_PI);
            cairo_fill(cr);
        }
    layout(graph, width);
    if (!graph->project || !graph->nodes->len) {
        text(cr, "Карта зависимостей", 32, 60, width - 64, 20, TRUE, .88, .93, 1);
        text(cr, graph->project ? "В проекте пока нет файлов C/C++." :
             "Откройте папку проекта, чтобы увидеть связи между файлами.",
             32, 104, width - 64, 11, FALSE, .55, .65, .76);
        return;
    }
    if (graph->overview || graph->selected < 0) {
        text(cr, "C / C++ · локальные включения", 24, 24, width - 48, 12, TRUE, .65, .78, .94);
    } else {
        double col = MAX(150.0, ((double)width - 100.0) / 3.0);
        text(cr, "ИСПОЛЬЗУЮТ ФАЙЛ", 24, 26, col, 9, TRUE, .55, .65, .76);
        text(cr, "ВЫБРАННЫЙ ФАЙЛ", 48 + col, 26, col, 9, TRUE, .4, .84, .73);
        text(cr, "ВКЛЮЧАЕТ", 72 + 2 * col, 26, col, 9, TRUE, .55, .65, .76);
    }
    for (guint i = 0; i < graph->project->edges->len; i++) {
        ProjectEdge *edge = g_ptr_array_index(graph->project->edges, i);
        if (edge->to < 0) continue;
        Node *from = find_node(graph, edge->from);
        Node *to = find_node(graph, (guint)edge->to);
        if (!from || !to || from == to) continue;
        double start_x = from->x + from->width, start_y = from->y + from->height / 2;
        double end_x = to->x, end_y = to->y + to->height / 2;
        if (to->x <= from->x) {
            start_x = from->x + from->width / 2;
            start_y = from->y + from->height;
            end_x = to->x + to->width / 2;
            end_y = to->y;
        }
        gboolean active = (gint)edge->from == graph->selected || edge->to == graph->selected;
        cairo_set_source_rgba(cr, active ? .35 : .38, active ? .85 : .52,
                             active ? .73 : .72, active ? .8 : .35);
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
        ProjectFile *file = g_ptr_array_index(graph->project->files, node->file);
        gboolean selected = (gint)node->file == graph->selected;
        rounded(cr, node->x, node->y, node->width, node->height);
        cairo_set_source_rgb(cr, selected ? .10 : .09, selected ? .22 : .13, selected ? .24 : .19);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, selected ? .36 : .2, selected ? .85 : .3, selected ? .73 : .42);
        cairo_set_line_width(cr, selected ? 2 : 1);
        cairo_stroke(cr);
        g_autofree char *basename = g_path_get_basename(file->path);
        g_autofree char *directory = g_path_get_dirname(file->path);
        text(cr, basename, node->x + 12, node->y + 10, node->width - 24, 11, TRUE, .88, .93, 1);
        text(cr, directory, node->x + 12, node->y + 37, node->width - 24, 9, FALSE, .53, .65, .78);
    }
    if (graph->omitted) {
        g_autofree char *notice = g_strdup_printf("Ещё %u узлов · выберите файл в дереве для детализации", graph->omitted);
        text(cr, notice, 24, 54, width - 48, 9, FALSE, .95, .73, .4);
    }
}

gboolean graph_activate_at(Graph *graph, double x, double y)
{
    for (guint i = 0; i < graph->nodes->len; i++) {
        Node node = g_array_index(graph->nodes, Node, i);
        if (x >= node.x && x <= node.x + node.width &&
            y >= node.y && y <= node.y + node.height) {
            graph->open_file(node.file, graph->user_data);
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
            ProjectFile *file = g_ptr_array_index(graph->project->files, node->file);
            tooltip = file->path;
            break;
        }
    }
    gtk_widget_set_tooltip_text(graph->area, tooltip);
    gtk_widget_set_cursor_from_name(graph->area, tooltip ? "pointer" : "default");
}

Graph *graph_new(GraphOpenFile open_file, gpointer user_data)
{
    Graph *graph = g_new0(Graph, 1);
    graph->nodes = g_array_new(FALSE, FALSE, sizeof(Node));
    graph->selected = -1;
    graph->open_file = open_file;
    graph->user_data = user_data;
    graph->area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(graph->area), 660);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(graph->area), 500);
    gtk_widget_set_hexpand(graph->area, TRUE);
    gtk_widget_set_vexpand(graph->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(graph->area), draw, graph, NULL);
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
    layout(graph, 660);
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
    graph->project = project;
    graph->selected = selected;
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
