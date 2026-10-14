#include "build.h"
#include "build_fixture.h"
#include <glib/gstdio.h>
#ifdef G_OS_WIN32
#include <glib/gwin32.h>
#include <windows.h>
#else
#include <signal.h>
#include <errno.h>
#endif

typedef struct { GMainLoop *loop; BuildRun *run; gint code; gboolean stopped, timed_out; guint lines; guint64 child; } RunTest;
static char *executable;
static void output(const char *line, gpointer data)
{
    RunTest *test = data; g_assert_true(g_utf8_validate(line, -1, NULL)); test->lines++;
    if (g_str_has_prefix(line, "CHILD_PID=")) { test->child = g_ascii_strtoull(line + 10, NULL, 10); build_run_stop(test->run); }
}
static void finished(gint code, gboolean stopped, gpointer data)
{ RunTest *test = data; test->code = code; test->stopped = stopped; g_main_loop_quit(test->loop); }
static gboolean timeout(gpointer data)
{ RunTest *test = data; test->timed_out = TRUE; build_run_stop(test->run); return G_SOURCE_REMOVE; }
static void run_mode(const char *mode, gint expected_code, gboolean expected_stop)
{
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    RunTest test = {.loop = loop, .code = -99};
    const char *args[] = {executable, "--test-build-child", mode, NULL};
    g_autofree char *directory = g_get_current_dir();
    test.run = build_run_start(args, directory, output, finished, &test);
    if (g_str_equal(mode, "immediate")) build_run_stop(test.run);
    guint timer = g_timeout_add_seconds(10, timeout, &test); g_main_loop_run(loop);
    if (!test.timed_out) g_source_remove(timer);
    g_assert_false(test.timed_out); g_assert_false(build_run_active(test.run));
    g_assert_cmpint(test.stopped, ==, expected_stop);
    if (!expected_stop) { g_assert_cmpint(test.code, ==, expected_code); g_assert_cmpuint(test.lines, >, 0); }
    if (g_str_equal(mode, "tree")) {
        g_assert_cmpuint(test.child, >, 0);
#ifdef G_OS_WIN32
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)test.child);
        if (process) { g_assert_cmpuint(WaitForSingleObject(process, 5000), ==, WAIT_OBJECT_0); CloseHandle(process); }
#else
        /* A killed grandchild can remain a zombie until the container's init reaps it. */
        g_autofree char *stat_path = g_strdup_printf("/proc/%" G_GUINT64_FORMAT "/stat", test.child);
        g_autofree char *stat = NULL;
        if (g_file_get_contents(stat_path, &stat, NULL, NULL)) {
            const char *end = strrchr(stat, ')'); g_assert_nonnull(end); g_assert_true(end[2] == 'Z' || end[2] == 'X');
        } else { g_assert_cmpint(kill((pid_t)test.child, 0), ==, -1); g_assert_cmpint(errno, ==, ESRCH); }
#endif
    }
    build_run_unref(test.run);
}
static void success(void) { run_mode("success", 0, FALSE); }
static void failure(void) { run_mode("failure", 2, FALSE); }
static void stop_tree(void) { run_mode("tree", -1, TRUE); }
static void stop_immediately(void) { run_mode("immediate", -1, TRUE); }
static void spawn_error(void)
{
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE); RunTest test = {.loop = loop};
    const char *args[] = {"startapp-missing-build-tool-71f923", NULL};
    g_autofree char *cwd = g_get_current_dir(); test.run = build_run_start(args, cwd, output, finished, &test);
    guint timer = g_timeout_add_seconds(10, timeout, &test); g_main_loop_run(loop);
    if (!test.timed_out) g_source_remove(timer);
    g_assert_false(test.timed_out); g_assert_cmpint(test.code, ==, -1); g_assert_cmpuint(test.lines, >, 0); build_run_unref(test.run);
}
int main(int argc, char **argv)
{
#ifdef G_OS_WIN32
    g_auto(GStrv) wide_args = g_win32_get_command_line(); argv = wide_args; argc = (gint)g_strv_length(wide_args);
#endif
    if (argc == 3 && g_str_equal(argv[1], "--test-build-child")) return build_fixture_main(argv[0], argv[2]);
    executable = g_canonicalize_filename(argv[0], NULL);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/build/success-streaming", success); g_test_add_func("/build/nonzero-exit-and-stderr", failure);
    g_test_add_func("/build/stop-process-tree", stop_tree); g_test_add_func("/build/stop-before-spawn", stop_immediately);
    g_test_add_func("/build/spawn-failure", spawn_error);
    gint result = g_test_run(); g_free(executable); return result;
}
