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
gboolean graph_activate_at(Graph *graph, double x, double y);
gboolean graph_node_center(Graph *graph, guint index, double *x, double *y);
void graph_free(Graph *graph);

#endif
