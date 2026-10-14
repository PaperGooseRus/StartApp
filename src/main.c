#include <gtk/gtk.h>
#include <locale.h>
#include <stdio.h>
#include "app.h"
#include "project.h"
#ifdef G_OS_WIN32
#include <glib/gwin32.h>
#endif

static void prepare_runtime(void)
{
#ifdef G_OS_WIN32
    /* Bundled DLLs and data sit beside the executable, independent of cwd. */
    g_autofree char *prefix = g_win32_get_package_installation_directory_of_module(NULL);
    g_autofree char *schemas = g_build_filename(prefix, "share", "glib-2.0", "schemas", NULL);
    g_autofree char *data = g_build_filename(prefix, "share", NULL);
    g_autofree char *fonts = g_build_filename(prefix, "etc", "fonts", "fonts.conf", NULL);
    if (!g_getenv("GSETTINGS_SCHEMA_DIR") && g_file_test(schemas, G_FILE_TEST_IS_DIR))
        g_setenv("GSETTINGS_SCHEMA_DIR", schemas, FALSE);
    if (g_file_test(data, G_FILE_TEST_IS_DIR)) {
        const char *existing = g_getenv("XDG_DATA_DIRS");
        g_autofree char *directories = existing ? g_strconcat(data, G_SEARCHPATH_SEPARATOR_S, existing, NULL) : g_strdup(data);
        g_setenv("XDG_DATA_DIRS", directories, TRUE);
    }
    if (!g_getenv("FONTCONFIG_FILE") && g_file_test(fonts, G_FILE_TEST_IS_REGULAR))
        g_setenv("FONTCONFIG_FILE", fonts, FALSE);
    if (!g_getenv("GSK_RENDERER")) g_setenv("GSK_RENDERER", "cairo", FALSE);
#endif
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
#ifdef G_OS_WIN32
    g_auto(GStrv) windows_args = g_win32_get_command_line();
    argv = windows_args;
    argc = (int)g_strv_length(windows_args);
#endif
    prepare_runtime();
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
