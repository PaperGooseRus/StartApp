#include <gtk/gtk.h>
#include <locale.h>
#include <stdio.h>
#include "app.h"
#include "project.h"
#include "build_fixture.h"
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
    if (argc == 3 && g_str_equal(argv[1], "--test-build-child")) return build_fixture_main(argv[0], argv[2]);
    const char *directory = NULL, *build_directory = NULL, *configuration = NULL;
    gboolean inspect = FALSE, smoke = FALSE;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (g_str_equal(arg, "--help") || g_str_equal(arg, "-h")) {
            g_print("StartApp - C / GTK4 project map and editor\n"
                "Usage: startapp [--inspect] [PROJECT_DIRECTORY] [--build-dir DIRECTORY] [--config NAME]\n"
                "       startapp --smoke-test (temporary fixture)\n");
            return 0;
        }
        if (g_str_equal(arg, "--inspect")) inspect = TRUE;
        else if (g_str_equal(arg, "--smoke-test")) smoke = TRUE;
        else if (g_str_equal(arg, "--build-dir") || g_str_equal(arg, "--config")) {
            if (++i >= argc) { g_printerr("Missing option value. Use --help.\n"); return 2; }
            if (g_str_equal(arg, "--build-dir")) build_directory = argv[i];
            else configuration = argv[i];
        } else if (arg[0] != '-' && !directory) directory = arg;
        else { g_printerr("Invalid arguments. Use --help.\n"); return 2; }
    }
    if ((inspect && !directory) || (smoke && (directory || build_directory || configuration || inspect)) ||
        (!directory && (build_directory || configuration))) {
        g_printerr("A project directory is required for build options or --inspect.\n"); return 2;
    }
    if (inspect) {
        g_autoptr(GError) error = NULL;
        Project *project = project_scan_build(directory, build_directory, configuration, &error);
        if (!project) { g_printerr("%s\n", error->message); return 1; }
        g_autofree char *json = project_to_json(project);
        /* JSON is UTF-8 regardless of the user's terminal locale. */
        fputs(json, stdout);
        project_free(project);
        return 0;
    }
    return app_run(directory, build_directory, configuration, smoke);
}
