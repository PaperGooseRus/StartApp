#include "project.h"

static gint target_named(Project *project, const char *name)
{
    for (guint i = 0; i < project->targets->len; i++) {
        ProjectTarget *target = g_ptr_array_index(project->targets, i);
        if (g_str_equal(target->name, name)) return (gint)i;
    }
    return -1;
}

static void dependency(Project *project, const char *from, const char *to)
{
    gint source = target_named(project, from), destination = target_named(project, to);
    g_assert_cmpint(source, >=, 0);
    g_assert_cmpint(destination, >=, 0);
    for (guint i = 0; i < project->target_edges->len; i++) {
        ProjectTargetEdge *edge = g_ptr_array_index(project->target_edges, i);
        if ((gint)edge->from == source && (gint)edge->to == destination) return;
    }
    g_error("Missing real CMake dependency: %s -> %s", from, to);
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, ==, 3);
    g_autoptr(GError) error = NULL;
    Project *project = project_scan_build(argv[1], argv[2], NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(project);
    g_assert_cmpuint(project->compile_units, >=, 8);
    g_assert_nonnull(project->generator); g_assert_nonnull(project->cmake_version);
    g_assert_cmpuint(project->compilers->len, >=, 1);
    ProjectCompiler *compiler = g_ptr_array_index(project->compilers, 0);
    g_assert_nonnull(compiler->id); g_assert_nonnull(compiler->version); g_assert_nonnull(compiler->path);
    dependency(project, "startapp", "startapp_ui");
    dependency(project, "startapp_ui", "startapp_core");
    ProjectTarget *app = g_ptr_array_index(project->targets, target_named(project, "startapp"));
    g_assert_cmpstr(app->definition, ==, "CMakeLists.txt");
    g_assert_cmpuint(app->line, >, 0);
    g_assert_nonnull(app->artifact);
    ProjectFile *file = g_ptr_array_index(project->files, project_find_file(project, "src/app.c"));
    g_assert_true(file->compile_settings);
    gboolean resolved = FALSE;
    for (guint i = file->include_start; i < file->include_start + file->include_count; i++) {
        ProjectEdge *edge = g_ptr_array_index(project->edges, i);
        if (g_str_equal(edge->include, "theme.h")) {
            g_assert_true(edge->metadata);
            g_assert_cmpint(edge->to, ==, project_find_file(project, "src/theme.h"));
            resolved = TRUE;
        }
    }
    g_assert_true(resolved);
    g_print("Real CMake File API + compilation database PASS (%u targets, %u compile units)\n",
            project->targets->len, project->compile_units);
    project_free(project);
    return 0;
}
