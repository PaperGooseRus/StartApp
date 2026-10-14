#ifndef STARTAPP_GRAPH_H
#define STARTAPP_GRAPH_H

#include <gtk/gtk.h>
#include "project.h"

typedef struct Graph Graph;
typedef void (*GraphOpenFile)(guint index, gpointer user_data);
typedef void (*GraphSelectTarget)(guint index, gpointer user_data);

Graph *graph_new(GraphOpenFile open_file, GraphSelectTarget select_target, gpointer user_data);
GtkWidget *graph_widget(Graph *graph);
void graph_set_project(Graph *graph, const Project *project, gint selected);
void graph_set_overview(Graph *graph, gboolean overview);
void graph_set_target_mode(Graph *graph, gboolean active);
void graph_select_target(Graph *graph, gint index);
void graph_set_grouped(Graph *graph, gboolean grouped);
void graph_fit(Graph *graph);
void graph_zoom_at(Graph *graph, double factor, double x, double y);
void graph_zoom_by(Graph *graph, double factor);
void graph_pan(Graph *graph, double dx, double dy);
double graph_get_zoom(Graph *graph);
guint graph_visible_count(Graph *graph);
void graph_get_transform(Graph *graph, double *zoom, double *x, double *y);
void graph_restore_transform(Graph *graph, double zoom, double x, double y);
char **graph_get_expanded(Graph *graph);
void graph_set_expanded(Graph *graph, const char *const *groups);
gboolean graph_activate_at(Graph *graph, double x, double y);
gboolean graph_node_center(Graph *graph, guint index, double *x, double *y);
void graph_free(Graph *graph);

#endif
