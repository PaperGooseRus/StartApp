#ifndef STARTAPP_GRAPH_H
#define STARTAPP_GRAPH_H

#include <gtk/gtk.h>
#include "project.h"

typedef struct Graph Graph;
typedef void (*GraphOpenFile)(guint index, gpointer user_data);

Graph *graph_new(GraphOpenFile open_file, gpointer user_data);
GtkWidget *graph_widget(Graph *graph);
void graph_set_project(Graph *graph, const Project *project, gint selected);
void graph_set_overview(Graph *graph, gboolean overview);
gboolean graph_activate_at(Graph *graph, double x, double y);
void graph_free(Graph *graph);

#endif
