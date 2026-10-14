#ifndef STARTAPP_PROJECT_H
#define STARTAPP_PROJECT_H

#include <gio/gio.h>

/* Analysis is static: no compiler or project script is executed. */
typedef struct {
    char *path; /* UTF-8 relative to Project.root, with '/' separators on all OSes. */
    gboolean source;
    gboolean build_file;
    gboolean compile_settings;
    char *settings_source;
    GPtrArray *quote_dirs;
    GPtrArray *include_dirs;
    GPtrArray *system_dirs;
    GPtrArray *after_dirs;
    guint include_start, include_count;
} ProjectFile;

typedef struct {
    guint from;
    gint to; /* -1 means missing, external or ambiguous. */
    char *include;
    guint line;
    gboolean system;
    gboolean ambiguous;
    gboolean metadata;
    gboolean outside_project;
    char *settings_source;
} ProjectEdge;

typedef struct {
    char *id, *name, *type, *artifact;
    char *definition;
    guint line;
    GArray *sources; /* Project.files indices. */
} ProjectTarget;

typedef struct {
    guint from, to;
    char *definition;
    guint line;
} ProjectTargetEdge;

typedef struct { char *language, *id, *version, *path, *source; } ProjectCompiler;
typedef struct {
    char *name, *display_name, *kind, *generator, *binary_directory, *configuration, *configure_preset, *source;
    gboolean hidden;
} ProjectPreset;

typedef struct {
    char *root;
    GPtrArray *files;
    GPtrArray *edges;
    GPtrArray *notes;
    guint skipped;
    char *build_directory;
    char *configuration;
    GPtrArray *configurations;
    GPtrArray *targets;
    GPtrArray *target_edges;
    guint compile_units;
    GHashTable *file_index;
    char *cmake_version, *generator;
    gboolean multi_config;
    GPtrArray *compilers, *presets;
} Project;

Project *project_scan(const char *directory, GError **error);
Project *project_scan_build(const char *directory, const char *build_directory,
                            const char *configuration, GError **error);
void project_free(Project *project);
gint project_find_file(const Project *project, const char *relative_path);
gint project_find_absolute(const Project *project, const char *path);
gboolean project_path_matches(const char *relative_path, const char *query);
char *project_summary(const Project *project);
char *project_to_json(const Project *project);
char *project_build_summary(const Project *project, gint target);

#endif
