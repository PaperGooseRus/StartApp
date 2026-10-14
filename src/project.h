#ifndef STARTAPP_PROJECT_H
#define STARTAPP_PROJECT_H

#include <gio/gio.h>

/* Analysis is static: no compiler or project script is executed. */
typedef struct {
    char *path; /* Relative to Project.root. */
    gboolean source;
    gboolean build_file;
} ProjectFile;

typedef struct {
    guint from;
    gint to; /* -1 means missing, external or ambiguous. */
    char *include;
    guint line;
    gboolean system;
    gboolean ambiguous;
} ProjectEdge;

typedef struct {
    char *root;
    GPtrArray *files;
    GPtrArray *edges;
    GPtrArray *notes;
    guint skipped;
} Project;

Project *project_scan(const char *directory, GError **error);
void project_free(Project *project);
gint project_find_file(const Project *project, const char *relative_path);
gboolean project_path_matches(const char *relative_path, const char *query);
char *project_summary(const Project *project);
char *project_to_json(const Project *project);

#endif
