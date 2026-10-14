#include <gtk/gtk.h>
#include <locale.h>
#include <stdio.h>
#include "app.h"
#include "project.h"

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    if (argc == 2 && (g_str_equal(argv[1], "--help") || g_str_equal(argv[1], "-h"))) {
        g_print("StartApp - C / GTK4 project map and editor\n"
                "Usage: startapp [PROJECT_DIRECTORY]\n"
                "       startapp --inspect PROJECT_DIRECTORY   (JSON, no display)\n"
                "       startapp --smoke-test                 (temporary fixture)\n");
        return 0;
    }
    if (argc == 3 && g_str_equal(argv[1], "--inspect")) {
        g_autoptr(GError) error = NULL;
        Project *project = project_scan(argv[2], &error);
        if (!project) { g_printerr("%s\n", error->message); return 1; }
        g_autofree char *json = project_to_json(project);
        /* JSON is UTF-8 regardless of the user's terminal locale. */
        fputs(json, stdout);
        project_free(project);
        return 0;
    }
    gboolean smoke = argc == 2 && g_str_equal(argv[1], "--smoke-test");
    if (argc > 2 || (argc == 2 && argv[1][0] == '-' && !smoke)) {
        g_printerr("Invalid arguments. Use --help.\n");
        return 2;
    }
    return app_run(smoke || argc == 1 ? NULL : argv[1], smoke);
}
