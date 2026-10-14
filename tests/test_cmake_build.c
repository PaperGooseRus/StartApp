#include "build.h"
#include <glib/gstdio.h>
#ifdef G_OS_WIN32
#include <glib/gwin32.h>
#endif

typedef struct { GMainLoop *loop; const Project *project; const char *directory; BuildRun *run; gint code; guint errors; gboolean timeout; } Test;
static void output(const char *line, gpointer data)
{
    Test *test = data;
    if (!test->project) return;
    BuildDiagnostic *diagnostic = build_diagnostic_parse(line, test->directory, test->project);
    if (diagnostic && diagnostic->project_path && g_str_equal(diagnostic->project_path, "main.c")) test->errors++;
    build_diagnostic_free(diagnostic);
}
static void finished(gint code, gboolean stopped, gpointer data)
{ Test *test = data; g_assert_false(stopped); test->code = code; g_main_loop_quit(test->loop); }
static gboolean deadline(gpointer data)
{ Test *test = data; test->timeout = TRUE; build_run_stop(test->run); return G_SOURCE_REMOVE; }
static gint execute(const char *const *args, const char *directory, const Project *project, guint *errors)
{
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    Test test = {.loop = loop, .project = project, .directory = directory, .code = -99};
    test.run = build_run_start(args, directory, output, finished, &test);
    guint timer = g_timeout_add_seconds(30, deadline, &test); g_main_loop_run(loop);
    if (!test.timeout) g_source_remove(timer);
    g_assert_false(test.timeout); if (errors) *errors = test.errors;
    build_run_unref(test.run); return test.code;
}
static void put(const char *root, const char *relative, const char *text)
{
    g_autofree char *path = g_build_filename(root, relative, NULL), *parent = g_path_get_dirname(path);
    g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0); g_assert_true(g_file_set_contents(path, text, -1, NULL));
}
static void remove_tree(const char *path)
{
    if (!g_file_test(path, G_FILE_TEST_IS_SYMLINK) && g_file_test(path, G_FILE_TEST_IS_DIR)) {
        GDir *dir = g_dir_open(path, 0, NULL); g_assert_nonnull(dir); const char *name;
        while ((name = g_dir_read_name(dir))) { g_autofree char *child = g_build_filename(path, name, NULL); remove_tree(child); }
        g_dir_close(dir); g_assert_cmpint(g_rmdir(path), ==, 0);
    } else g_assert_cmpint(g_unlink(path), ==, 0);
}
int main(int argc, char **argv)
{
#ifdef G_OS_WIN32
    g_auto(GStrv) args = g_win32_get_command_line(); argv = args; argc = (gint)g_strv_length(args);
#endif
    g_assert_cmpint(argc, ==, 2);
    g_autofree char *temporary = g_dir_make_tmp("startapp-real-build-XXXXXX", NULL);
    g_autofree char *root = g_build_filename(temporary, "project with spaces", NULL), *build = g_build_filename(root, "build", NULL);
    put(root, "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(owned_fixture C)\nadd_executable(fixture main.c)\n");
    put(root, "main.c", "int main(void) { return missing_token; }\n");
    put(root, "build/.cmake/api/v1/query/codemodel-v2", ""); put(root, "build/.cmake/api/v1/query/toolchains-v1", "");
    const char *configure[] = {argv[1], "-S", root, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", NULL};
    g_assert_cmpint(execute(configure, root, NULL, NULL), ==, 0);
    g_autoptr(GError) error = NULL; Project *project = project_scan_build(root, build, "Debug", &error);
    g_assert_no_error(error); g_assert_cmpuint(project->targets->len, ==, 1);
    BuildPlan *plan = build_plan_new(project, 0, argv[1], &error); g_assert_no_error(error); g_assert_nonnull(plan);
    guint errors = 0; g_assert_cmpint(execute((const char *const *)plan->argv, plan->directory, project, &errors), !=, 0);
    g_assert_cmpuint(errors, >, 0);
    put(root, "main.c", "int main(void) { return 0; }\n");
    g_assert_cmpint(execute((const char *const *)plan->argv, plan->directory, project, NULL), ==, 0);
    ProjectTarget *target = g_ptr_array_index(project->targets, 0); g_assert_nonnull(target->artifact);
    g_assert_true(g_file_test(target->artifact, G_FILE_TEST_IS_REGULAR));
    g_print("Real CMake/Ninja build PASS: failing compile, file diagnostics, fixed source, selected target and artifact\n");
    build_plan_free(plan); project_free(project); remove_tree(temporary); return 0;
}
