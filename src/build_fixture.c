#include "build_fixture.h"
#include <gio/gio.h>
#include <stdio.h>

/* Owned smoke-test child: portable GUI tests also run with no compiler in PATH. */
int build_fixture_main(const char *executable, const char *mode)
{
    if (g_str_equal(mode, "success")) {
        g_print("[1/2] Compiling МОДУЛЬ.c\n[2/2] Linking fixture\n"); fflush(stdout); return 0;
    }
    if (g_str_equal(mode, "failure")) {
        g_printerr("main.c:101:3: error: fixture compile failure\n"); return 2;
    }
    if (g_str_equal(mode, "tree")) {
        g_autoptr(GError) error = NULL;
        g_autoptr(GSubprocess) child = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error, executable, "--test-build-child", "wait", NULL);
        if (!child) { g_printerr("%s\n", error->message); return 3; }
        g_print("CHILD_PID=%s\n", g_subprocess_get_identifier(child)); fflush(stdout);
    } else if (!g_str_equal(mode, "wait")) return 2;
    g_print("waiting for cancellation\n"); fflush(stdout);
    for (guint i = 0; i < 120; i++) g_usleep(500000);
    return 0;
}
