#ifndef STARTAPP_WORKSPACE_TOOLS_H
#define STARTAPP_WORKSPACE_TOOLS_H
#include "project.h"
#include "build.h"

typedef struct { char *path, *text; guint line; } TextMatch;
void text_match_free(gpointer data);
GPtrArray *workspace_search(const Project *project, const char *query, GCancellable *cancel, GError **error);
/* direction: 0 both, 1 dependencies, 2 dependants. depth 0 = unbounded. */
GArray *workspace_reachable(const Project *project, gboolean targets, gint focus, guint direction, guint depth);
char *workspace_explain(const Project *project, gboolean targets, guint from, guint to);
char *workspace_architecture(const Project *project, const char *rules, GError **error);
char *workspace_compare(const Project *left, const Project *right);
char *workspace_export(const Project *project, gboolean html, gboolean targets);
char *workspace_environment(const Project *project);
gboolean workspace_extract_snapshot(const char *archive, const char *destination, GError **error);
char *workspace_catalog(const char *filename, const Project *project, const char *last_file, GError **error);
BuildPlan *workspace_command(const char *directory, const char *const *argv);
BuildPlan *workspace_configure(const Project *project, const char *system, const char *tool,
    const char *build_dir, const char *generator, const char *config, const char *preset, GError **error);
BuildPlan *workspace_build(const Project *project, const char *system, const char *tool, const char *target, GError **error);
BuildPlan *workspace_launch(const Project *project, gint target, const char *arguments, const char *directory,
    const char *debugger, const char *breakpoint, GError **error);
#endif
