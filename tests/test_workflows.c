#include "workspace_tools.h"
#include <glib/gstdio.h>
#include <string.h>

typedef struct { GMainLoop *loop; BuildRun *run; GString *output; gint code; gboolean expired; } Execution;
static void output(const char *line, gpointer data)
{ Execution *e = data; if (e->output->len < 262144) g_string_append_printf(e->output, "%s\n", line); }
static void finished(gint code, gboolean stopped, gpointer data)
{ Execution *e = data; e->code = code; if (stopped) e->code = -99; g_main_loop_quit(e->loop); }
static gboolean deadline(gpointer data)
{ Execution *e = data; e->expired = TRUE; build_run_stop(e->run); return G_SOURCE_REMOVE; }
static char *execute(BuildPlan *plan)
{
    g_assert_nonnull(plan); g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    Execution e = {.loop = loop, .output = g_string_new(NULL), .code = -999};
    e.run = build_run_start((const char *const *)plan->argv, plan->directory, output, finished, &e);
    guint timeout = g_timeout_add_seconds(30, deadline, &e); g_main_loop_run(loop); if (!e.expired) g_source_remove(timeout);
    if (e.code) g_test_message("Command %s\n%s", plan->display, e.output->str);
    g_assert_false(e.expired); g_assert_cmpint(e.code, ==, 0); build_run_unref(e.run); return g_string_free(e.output, FALSE);
}
static void run(BuildPlan *plan) { g_autofree char *log = execute(plan); build_plan_free(plan); }
static void put(const char *root, const char *name, const char *text)
{ g_autofree char *path = g_build_filename(root, name, NULL); g_assert_true(g_file_set_contents(path, text, -1, NULL)); }
static void remove_tree(const char *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        GDir *dir = g_dir_open(path, 0, NULL); g_assert_nonnull(dir); const char *name;
        while ((name = g_dir_read_name(dir))) { g_autofree char *child = g_build_filename(path, name, NULL); remove_tree(child); }
        g_dir_close(dir); g_assert_cmpint(g_rmdir(path), ==, 0);
    } else g_assert_cmpint(g_unlink(path), ==, 0);
}
static char *fixture(void)
{
    char *root = g_dir_make_tmp("startapp-workflow-spaces-XXXXXX", NULL); g_assert_nonnull(root);
    put(root, "main.c", "#include <stdio.h>\nint main(int argc, char **argv) { int result = 42; printf(\"fixture:%d:%s:%d\\n\", argc, argc>1?argv[1]:\"none\", result); return 0; }\n"); return root;
}
static void test_cmake_run_debug(void)
{
    g_autofree char *cmake = g_find_program_in_path("cmake"); g_assert_nonnull(cmake);
    g_autofree char *root = fixture(); put(root, "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(fixture C)\nadd_executable(fixture main.c)\n");
    Project *initial = project_scan(root, NULL); g_autoptr(GError) error = NULL;
    run(workspace_configure(initial, "CMake", cmake, "build debug", "Ninja", "Debug", NULL, &error)); g_assert_no_error(error);
    Project *p = project_scan_build(root, "build debug", "Debug", &error); g_assert_no_error(error); g_assert_cmpuint(p->targets->len, ==, 1);
    run(workspace_build(p, "CMake", cmake, "fixture", &error)); g_assert_no_error(error);
    BuildPlan *launch = workspace_launch(p, 0, "'argument with spaces'", root, NULL, NULL, &error); g_assert_no_error(error);
    g_autofree char *log = execute(launch); g_assert_nonnull(strstr(log, "fixture:2:argument with spaces:42")); build_plan_free(launch);
    g_autofree char *gdb = g_find_program_in_path("gdb");
    if (gdb) {
        BuildPlan *debug = workspace_launch(p, 0, "", root, gdb, "main", &error); g_assert_no_error(error);
        g_autofree char *trace = execute(debug); g_assert_nonnull(strstr(trace, "main")); g_assert_nonnull(strstr(trace, "#0")); build_plan_free(debug);
    } else g_test_message("Optional debugger unavailable; run/configure remain tested");
    run(workspace_configure(initial, "CMake", cmake, "build release", "Ninja", "Release", NULL, &error)); g_assert_no_error(error);
    Project *release = project_scan_build(root, "build release", "Release", &error); g_assert_no_error(error);
    g_autofree char *comparison = workspace_compare(p, release); g_assert_nonnull(strstr(comparison, "Параметры main.c")); g_assert_nonnull(strstr(comparison, "-O"));
    project_free(initial); project_free(p); project_free(release); remove_tree(root);
}
static void test_meson(void)
{
    g_autofree char *meson = g_find_program_in_path("meson"); if (!meson) { g_test_skip("Optional Meson unavailable"); return; }
    g_autofree char *root = fixture(); put(root, "meson.build", "project('fixture', 'c')\nexecutable('fixture', 'main.c')\n");
    Project *initial = project_scan(root, NULL); g_autoptr(GError) error = NULL;
    run(workspace_configure(initial, "Meson", meson, "build meson", "", "Debug", NULL, &error)); g_assert_no_error(error);
    Project *p = project_scan_build(root, "build meson", NULL, &error); g_assert_no_error(error); g_assert_cmpuint(p->targets->len, ==, 1);
    g_assert_nonnull(strstr(p->generator, "Meson")); g_assert_cmpuint(p->compile_units, >, 0);
    run(workspace_build(p, "Meson", meson, "fixture", &error)); g_assert_no_error(error);
    BuildPlan *launch = workspace_launch(p, 0, "", root, NULL, NULL, &error); g_assert_no_error(error); run(launch);
    project_free(initial); project_free(p); remove_tree(root);
}
static void test_make(void)
{
    g_autofree char *make = g_find_program_in_path("make"); if (!make) make = g_find_program_in_path("mingw32-make");
    if (!make) { g_test_skip("Optional Make unavailable"); return; }
    g_autofree char *root = fixture();
    put(root, "Makefile", "all: fixture\nfixture: main.c\n\tgcc main.c -o fixture\n");
    Project *p = project_scan(root, NULL); g_assert_cmpuint(p->targets->len, ==, 2); g_autoptr(GError) error = NULL;
    run(workspace_build(p, "Make", make, "all", &error)); g_assert_no_error(error);
#ifdef G_OS_WIN32
    g_autofree char *exe = g_build_filename(root, "fixture.exe", NULL);
#else
    g_autofree char *exe = g_build_filename(root, "fixture", NULL);
#endif
    g_assert_true(g_file_test(exe, G_FILE_TEST_IS_REGULAR)); project_free(p); remove_tree(root);
}
static void test_git_snapshot(void)
{
    g_autofree char *git = g_find_program_in_path("git"); if (!git) { g_test_skip("Optional Git unavailable"); return; }
    g_autofree char *root = fixture();
    const char *init[] = {git, "init", NULL}; run(workspace_command(root, init));
    const char *add[] = {git, "add", "main.c", NULL}; run(workspace_command(root, add));
    const char *commit[] = {git, "-c", "user.name=StartApp fixture", "-c", "user.email=fixture@example.invalid", "-c", "commit.gpgsign=false", "commit", "-m", "Owned fixture", NULL}; run(workspace_command(root, commit));
    g_autofree char *archive = g_build_filename(root, "snapshot.tar", NULL), *option = g_strconcat("--output=", archive, NULL);
    const char *args[] = {git, "archive", "--format=tar", option, "HEAD", NULL}; run(workspace_command(root, args));
    g_autofree char *destination = g_build_filename(root, "build", "snapshot", NULL); g_autoptr(GError) error = NULL;
    g_assert_true(workspace_extract_snapshot(archive, destination, &error)); g_assert_no_error(error);
    Project *before = project_scan(destination, &error); g_assert_no_error(error); put(root, "api.h", "#pragma once\n"); put(root, "main.c", "#include \"api.h\"\nint main(void) { return 0; }\n");
    Project *after = project_scan(root, &error); g_assert_no_error(error); g_autofree char *comparison = workspace_compare(before, after);
    g_assert_nonnull(strstr(comparison, "+ Include main.c → api.h")); g_assert_nonnull(strstr(comparison, "+ Файл api.h"));
    project_free(before); project_free(after); remove_tree(root);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); g_test_add_func("/workflow/cmake-configure-compare-run-gdb", test_cmake_run_debug);
    g_test_add_func("/workflow/meson-configure-import-build-run", test_meson); g_test_add_func("/workflow/make-build", test_make); g_test_add_func("/workflow/git-archive-graph-compare", test_git_snapshot); return g_test_run();
}
