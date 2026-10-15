#ifndef STARTAPP_TOOLS_UI_H
#define STARTAPP_TOOLS_UI_H
#include <gtk/gtk.h>
#include "project.h"
typedef struct ToolsUI ToolsUI;
typedef void (*ToolsNavigate)(const char *root, const char *path, guint line, gpointer data);
typedef void (*ToolsConfigured)(const char *root, const char *build_dir, gpointer data);
typedef void (*ToolsGit)(const char *root, GHashTable *changed_paths, gpointer data);
ToolsUI *tools_ui_new(GtkApplication *application, GtkWindow *parent, ToolsNavigate navigate, ToolsConfigured configured, gpointer data);
void tools_ui_show(ToolsUI *tools, const Project *project, const char *file, gint target);
void tools_ui_set_git(ToolsUI *tools, ToolsGit callback);
gint tools_ui_smoke_step(ToolsUI *tools, guint step);
gboolean tools_ui_busy(ToolsUI *tools);
void tools_ui_stop(ToolsUI *tools);
void tools_ui_free(ToolsUI *tools);
#endif
