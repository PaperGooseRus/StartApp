#ifndef STARTAPP_BUILD_H
#define STARTAPP_BUILD_H
#include "project.h"

typedef struct { char **argv; char *directory, *display; } BuildPlan;
BuildPlan *build_plan_new(const Project *project, gint target, const char *cmake, GError **error);
void build_plan_free(BuildPlan *plan);

typedef struct { char *file, *project_path, *message; guint line, column; gboolean warning; } BuildDiagnostic;
BuildDiagnostic *build_diagnostic_parse(const char *text, const char *directory, const Project *project);
void build_diagnostic_free(gpointer data);

typedef struct BuildRun BuildRun;
typedef void (*BuildOutput)(const char *line, gpointer data);
typedef void (*BuildFinished)(gint exit_code, gboolean stopped, gpointer data);
/* Callbacks run on the caller's main context. Output is bounded and line-oriented. */
BuildRun *build_run_start(const char *const *argv, const char *directory, BuildOutput output,
                         BuildFinished finished, gpointer data);
void build_run_stop(BuildRun *run);
void build_run_unref(BuildRun *run);
gboolean build_run_active(BuildRun *run);

#endif
