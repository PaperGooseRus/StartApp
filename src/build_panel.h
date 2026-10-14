#ifndef STARTAPP_BUILD_PANEL_H
#define STARTAPP_BUILD_PANEL_H
#include <gtk/gtk.h>
#include "build.h"

typedef struct BuildPanel BuildPanel;
typedef void (*BuildNavigate)(const char *path, guint line, gpointer data);
BuildPanel *build_panel_new(BuildNavigate navigate, BuildFinished finished, gpointer data);
GtkWidget *build_panel_widget(BuildPanel *panel);
gboolean build_panel_start(BuildPanel *panel, const Project *project, BuildPlan *plan);
gboolean build_panel_running(BuildPanel *panel);
void build_panel_stop(BuildPanel *panel);
gint build_panel_exit_code(BuildPanel *panel);
gboolean build_panel_stopped(BuildPanel *panel);
guint build_panel_diagnostic_count(BuildPanel *panel);
gboolean build_panel_open_diagnostic(BuildPanel *panel, guint index);
void build_panel_free(BuildPanel *panel);
#endif
