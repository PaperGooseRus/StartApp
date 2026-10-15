#include "document.h"
#include "project.h"
#include "build_metadata.h"
#include "graph_layout.h"
#include "build.h"
#include "session.h"
#include <math.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#ifndef G_OS_WIN32
#include <sys/stat.h>
#endif

static gboolean make_symlink(const char *target, const char *path)
{
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GError) error = NULL;
    if (g_file_make_symbolic_link(file, target, NULL, &error)) return TRUE;
#ifdef G_OS_WIN32
    /* Windows may require Developer Mode or elevation to create symbolic links. */
    g_test_skip(error->message);
    return FALSE;
#else
    g_error("Cannot create fixture symlink: %s", error->message);
    return FALSE;
#endif
}

static char *fixture(void)
{
    GError *error = NULL;
    char *root = g_dir_make_tmp("startapp-core-XXXXXX", &error);
    g_assert_no_error(error);
    return root;
}

static char *write_file(const char *root, const char *relative, const char *text)
{
    char *path = g_build_filename(root, relative, NULL);
    g_autofree char *directory = g_path_get_dirname(path);
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    return path;
}

static void remove_tree(const char *path)
{
    if (!g_file_test(path, G_FILE_TEST_IS_SYMLINK) && g_file_test(path, G_FILE_TEST_IS_DIR)) {
        GDir *directory = g_dir_open(path, 0, NULL);
        const char *name;
        while ((name = g_dir_read_name(directory))) {
            g_autofree char *child = g_build_filename(path, name, NULL);
            remove_tree(child);
        }
        g_dir_close(directory);
        g_assert_cmpint(g_rmdir(path), ==, 0);
    } else g_assert_cmpint(g_unlink(path), ==, 0);
}

static Project *scan(const char *root)
{
    GError *error = NULL;
    Project *project = project_scan(root, &error);
    g_assert_no_error(error);
    g_assert_nonnull(project);
    return project;
}

static void test_scan(void)
{
    g_autofree char *root = fixture();
    g_autofree char *main = write_file(root, "src/main.c", "#include \"api.h\"\n#include <stdio.h>\n");
    g_autofree char *header = write_file(root, "include/api.h", "#pragma once\n");
    g_autofree char *make = write_file(root, "Makefile", "all:\n\tcc src/main.c\n");
    g_autofree char *cmake = write_file(root, "CMakeLists.txt", "project(test C)\n");
    Project *project = scan(root);
    g_assert_cmpuint(project->files->len, ==, 4);
    g_assert_cmpuint(project->edges->len, ==, 2);
    ProjectEdge *local = g_ptr_array_index(project->edges, 0);
    g_assert_cmpint(local->to, ==, project_find_file(project, "include/api.h"));
    g_assert_cmpuint(local->line, ==, 1);
    ProjectEdge *system = g_ptr_array_index(project->edges, 1);
    g_assert_true(system->system);
    g_assert_cmpint(system->to, ==, -1);
    ProjectFile *build = g_ptr_array_index(project->files, project_find_file(project, "Makefile"));
    g_assert_true(build->build_file);
    g_autofree char *json = project_to_json(project);
    g_assert_nonnull(strstr(json, "\"line\":1"));
    project_free(project);
    remove_tree(root);
}

static void test_resolution(void)
{
    g_autofree char *root = fixture();
    g_autofree char *main = write_file(root, "src/main.c", "#include \"api.h\"\n#include \"shared.h\"\n#include \"absent.h\"\n");
    g_autofree char *own = write_file(root, "src/api.h", "");
    g_autofree char *other = write_file(root, "other/api.h", "");
    g_autofree char *a = write_file(root, "a/shared.h", "");
    g_autofree char *b = write_file(root, "b/shared.h", "");
    Project *project = scan(root);
    ProjectEdge *edge = g_ptr_array_index(project->edges, 0);
    g_assert_cmpint(edge->to, ==, project_find_file(project, "src/api.h"));
    edge = g_ptr_array_index(project->edges, 1);
    g_assert_true(edge->ambiguous);
    g_assert_cmpint(edge->to, ==, -1);
    edge = g_ptr_array_index(project->edges, 2);
    g_assert_false(edge->ambiguous);
    g_assert_cmpint(edge->to, ==, -1);
    project_free(project);
    remove_tree(root);
}

static void test_comments(void)
{
    g_autofree char *root = fixture();
    g_autofree char *path = write_file(root, "main.c",
        "/*\n#include \"fake.h\"\n*/\n// #include \"fake.h\"\n"
        "const char *s = \"// not a comment\";\n"
        "# /* allow comment */ include \"real.h\"\n"
        "#include SOME_MACRO\n#include_next \"ignored.h\"\n");
    g_autofree char *header = write_file(root, "real.h", "");
    Project *project = scan(root);
    g_assert_cmpuint(project->edges->len, ==, 1);
    ProjectEdge *edge = g_ptr_array_index(project->edges, 0);
    g_assert_cmpstr(edge->include, ==, "real.h");
    g_assert_cmpuint(edge->line, ==, 6);
    project_free(project);
    remove_tree(root);
}

static void test_exclusions(void)
{
    g_autofree char *root = fixture();
    g_autofree char *main = write_file(root, "main.c", "");
    g_autofree char *git = write_file(root, ".git/internal.c", "");
    g_autofree char *build = write_file(root, "build/generated.c", "");
    g_autofree char *modules = write_file(root, "node_modules/module.c", "");
    g_autofree char *link = g_build_filename(root, "link.c", NULL);
    gboolean linked = make_symlink(main, link);
    Project *project = scan(root);
    g_assert_cmpuint(project->files->len, ==, 1);
    g_assert_cmpuint(project->skipped, ==, linked ? 1 : 0);
    project_free(project);
    remove_tree(root);
}

static void test_empty_and_missing(void)
{
    g_autofree char *root = fixture();
    Project *project = scan(root);
    g_assert_cmpuint(project->files->len, ==, 0);
    project_free(project);
    g_autofree char *missing = g_build_filename(root, "missing", NULL);
    GError *error = NULL;
    g_assert_null(project_scan(missing, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    remove_tree(root);
}

static void test_relative_parent(void)
{
    g_autofree char *root = fixture();
    g_autofree char *main = write_file(root, "src/main.c", "#include \"../include/api.h\"\n");
    g_autofree char *header = write_file(root, "include/api.h", "");
    Project *project = scan(root);
    ProjectEdge *edge = g_ptr_array_index(project->edges, 0);
    g_assert_cmpint(edge->to, ==, project_find_file(project, "include/api.h"));
    project_free(project);
    remove_tree(root);
}

static Document *open_document(const char *path)
{
    GError *error = NULL;
    Document *document = document_open(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(document);
    return document;
}

static void test_save_and_rescan(void)
{
    g_autofree char *root = fixture();
    g_autofree char *path = write_file(root, "main.c", "int main(void) { return 0; }\n");
    g_autofree char *header = write_file(root, "api.h", "");
#ifndef G_OS_WIN32
    g_assert_cmpint(g_chmod(path, 0640), ==, 0);
#endif
    Document *document = open_document(path);
    GError *error = NULL;
    g_assert_true(document_save(document, "#include \"api.h\"\n", &error));
    g_assert_no_error(error);
    Project *project = scan(root);
    g_assert_cmpuint(project->edges->len, ==, 1);
    ProjectEdge *edge = g_ptr_array_index(project->edges, 0);
    g_assert_cmpint(edge->to, >=, 0);
#ifndef G_OS_WIN32
    struct stat st;
    g_assert_cmpint(stat(path, &st), ==, 0);
    g_assert_cmpuint(st.st_mode & 0777, ==, 0640);
#endif
    g_assert_true(document_save(document, "// second save\n", &error));
    g_assert_no_error(error);
    document_free(document);
    project_free(project);
    remove_tree(root);
}

static void test_save_conflict(void)
{
    g_autofree char *root = fixture();
    g_autofree char *path = write_file(root, "main.c", "original\n");
    Document *document = open_document(path);
    g_assert_true(g_file_set_contents(path, "external edit\n", -1, NULL));
    GError *error = NULL;
    g_assert_false(document_save(document, "my edit\n", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG);
    g_clear_error(&error);
    g_autofree char *disk = NULL;
    g_assert_true(g_file_get_contents(path, &disk, NULL, NULL));
    g_assert_cmpstr(disk, ==, "external edit\n");
    document_free(document);
    remove_tree(root);
}

static void test_crlf(void)
{
    g_autofree char *root = fixture();
    g_autofree char *path = write_file(root, "main.c", "a\r\nb\r\n");
    Document *document = open_document(path);
    g_assert_cmpstr(document->text, ==, "a\nb\n");
    GError *error = NULL;
    g_assert_true(document_save(document, "a\nc\n", &error));
    g_assert_no_error(error);
    g_autofree char *disk = NULL;
    g_assert_true(g_file_get_contents(path, &disk, NULL, NULL));
    g_assert_cmpstr(disk, ==, "a\r\nc\r\n");
    document_free(document);
    remove_tree(root);
}

static void test_binary_rejected(void)
{
    g_autofree char *root = fixture();
    g_autofree char *path = write_file(root, "binary.c", "");
    const char bytes[] = {'x', '\0', 'y'};
    g_assert_true(g_file_set_contents(path, bytes, sizeof bytes, NULL));
    GError *error = NULL;
    g_assert_null(document_open(path, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    Project *project = scan(root);
    g_assert_cmpuint(project->notes->len, ==, 1);
    project_free(project);
    remove_tree(root);
}

static void test_symlink_replaced(void)
{
    g_autofree char *root = fixture();
    g_autofree char *path = write_file(root, "main.c", "original\n");
    g_autofree char *target = write_file(root, "other.c", "leave me alone\n");
    Document *document = open_document(path);
    g_assert_cmpint(g_unlink(path), ==, 0);
    if (!make_symlink(target, path)) {
        document_free(document);
        remove_tree(root);
        return;
    }
    GError *error = NULL;
    g_assert_false(document_save(document, "replacement\n", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_clear_error(&error);
    g_autofree char *disk = NULL;
    g_assert_true(g_file_get_contents(target, &disk, NULL, NULL));
    g_assert_cmpstr(disk, ==, "leave me alone\n");
    document_free(document);
    remove_tree(root);
}

static void test_unicode(void)
{
    g_autofree char *root = fixture();
    g_autofree char *path = write_file(root, "основной.c", "#include \"данные.h\"\n");
    g_autofree char *header = write_file(root, "данные.h", "// Привет\n");
    Project *project = scan(root);
    ProjectEdge *edge = g_ptr_array_index(project->edges, 0);
    g_assert_cmpint(edge->to, >=, 0);
    Document *document = open_document(header);
    g_assert_cmpstr(document->text, ==, "// Привет\n");
    document_free(document);
    project_free(project);
    remove_tree(root);
}

static void test_search_paths(void)
{
    g_autofree char *root = fixture();
    g_autofree char *a = write_file(root, "src/Api.h", "");
    g_autofree char *b = write_file(root, "tests/api.h", "");
    g_autofree char *c = write_file(root, "src/main.c", "");
    Project *project = scan(root);
    guint basename_matches = 0, path_matches = 0;
    for (guint i = 0; i < project->files->len; i++) {
        ProjectFile *file = g_ptr_array_index(project->files, i);
        basename_matches += project_path_matches(file->path, "API.H");
        path_matches += project_path_matches(file->path, "SRC/API");
    }
    g_assert_cmpuint(basename_matches, ==, 2);
    g_assert_cmpuint(path_matches, ==, 1);
    g_assert_false(project_path_matches("src/main.c", "src/api"));
    project_free(project);
    remove_tree(root);
}

static void test_search_unicode(void)
{
    g_assert_true(project_path_matches("src/ДАННЫЕ.h", "данные"));
    g_assert_true(project_path_matches("src/Café.c", "CAFE\314\201"));
    g_assert_true(project_path_matches("src/Straße.c", "STRASSE"));
    g_assert_false(project_path_matches("src/данные.h", "модуль"));
}

static void test_search_empty(void)
{
    g_assert_true(project_path_matches("src/main.c", ""));
    g_assert_true(project_path_matches("src/main.c", "  \t "));
    g_assert_true(project_path_matches("src/main.c", " main.c "));
    g_assert_false(project_path_matches("src/main.c", "does-not-exist"));
}

static void put(const char *root, const char *path, const char *text)
{
    g_autofree char *written = write_file(root, path, text);
}

static ProjectEdge *include_edge(Project *project, const char *source, const char *name)
{
    gint index = project_find_file(project, source);
    g_assert_cmpint(index, >=, 0);
    for (guint i = 0; i < project->edges->len; i++) {
        ProjectEdge *edge = g_ptr_array_index(project->edges, i);
        if ((gint)edge->from == index && g_str_equal(edge->include, name)) return edge;
    }
    g_error("Missing fixture include %s -> %s", source, name);
    return NULL;
}

static void resolved(Project *project, const char *source, const char *name, const char *destination)
{
    ProjectEdge *edge = include_edge(project, source, name);
    g_assert_true(edge->metadata);
    g_assert_false(edge->ambiguous);
    g_assert_cmpint(edge->to, ==, destination ? project_find_file(project, destination) : -1);
}

static void test_windows_commands(void)
{
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) args = build_metadata_split_command(
        "cl.exe /I\"C:\\SDK files\\include\" /external:I C:\\SDK\\system /c \"src\\МОДУЛЬ.c\" \"\"", TRUE, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(g_strv_length(args), ==, 7);
    g_assert_cmpstr(args[1], ==, "/IC:\\SDK files\\include");
    g_assert_cmpstr(args[3], ==, "C:\\SDK\\system");
    g_assert_cmpstr(args[5], ==, "src\\МОДУЛЬ.c");
    g_assert_cmpstr(args[6], ==, "");
    g_auto(GStrv) doubled = build_metadata_split_command("cl \"say\"\"hello\"", TRUE, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(doubled[1], ==, "say\"hello");
    g_auto(GStrv) broken = build_metadata_split_command("cl /I\"broken", TRUE, &error);
    g_assert_null(broken);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void test_compile_order(void)
{
    g_autofree char *root = fixture();
    put(root, "src/main.c", "#include <api.h>\n#include \"quote.h\"\n#include <quote.h>\n#include <system.h>\n");
    put(root, "первый каталог/api.h", "");
    put(root, "second/api.h", "");
    put(root, "quoted/quote.h", "");
    put(root, "second/quote.h", "");
    put(root, "system/system.h", "");
    put(root, "after/system.h", "");
    put(root, "build/compile_commands.json", "[{\"directory\":\"..\",\"file\":\"src/main.c\","
        "\"arguments\":[\"cc\",\"-Iпервый каталог\",\"-I\",\"second\",\"-iquote\",\"quoted\","
        "\"-idirafterafter\",\"-isystem\",\"system\"]}]");
    Project *project = scan(root);
    g_assert_cmpuint(project->compile_units, ==, 1);
    resolved(project, "src/main.c", "api.h", "первый каталог/api.h");
    ProjectFile *source = g_ptr_array_index(project->files, project_find_file(project, "src/main.c"));
    ProjectEdge *quote = g_ptr_array_index(project->edges, source->include_start + 1);
    ProjectEdge *angle = g_ptr_array_index(project->edges, source->include_start + 2);
    g_assert_cmpint(quote->to, ==, project_find_file(project, "quoted/quote.h"));
    g_assert_cmpint(angle->to, ==, project_find_file(project, "second/quote.h"));
    resolved(project, "src/main.c", "system.h", "system/system.h");
    g_assert_cmpuint(project->notes->len, ==, 0);
    g_autofree char *json = project_to_json(project);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
    project_free(project);
    remove_tree(root);
}

static void test_compile_inheritance(void)
{
    g_autofree char *root = fixture();
    put(root, "main.c", "#include \"common/a.h\"\n");
    put(root, "common/a.h", "#include <nested.h>\n");
    put(root, "headers/nested.h", "#include \"../common/a.h\"\n");
    put(root, "build/compile_commands.json", "[{\"directory\":\"..\",\"file\":\"main.c\","
        "\"arguments\":[\"cc\",\"/Iheaders\"]}]");
    Project *project = scan(root);
    resolved(project, "common/a.h", "nested.h", "headers/nested.h");
    resolved(project, "headers/nested.h", "../common/a.h", "common/a.h");
    g_assert_cmpuint(project->edges->len, ==, 3);
    project_free(project);
    remove_tree(root);
}

static void test_compile_conflict(void)
{
    g_autofree char *root = fixture();
    put(root, "a.c", "#include \"shared.h\"\n");
    put(root, "b.c", "#include \"shared.h\"\n");
    put(root, "shared.h", "#include <choice.h>\n");
    put(root, "one/choice.h", "");
    put(root, "two/choice.h", "");
    put(root, "build/compile_commands.json", "["
        "{\"directory\":\"..\",\"file\":\"a.c\",\"arguments\":[\"cc\",\"-Ione\"]},"
        "{\"directory\":\"..\",\"file\":\"b.c\",\"arguments\":[\"cc\",\"-Itwo\"]}]");
    Project *project = scan(root);
    ProjectEdge *edge = include_edge(project, "shared.h", "choice.h");
    g_assert_true(edge->metadata);
    g_assert_true(edge->ambiguous);
    g_assert_cmpint(edge->to, ==, -1);
    project_free(project);
    remove_tree(root);
}

static void test_compile_no_guessing(void)
{
    g_autofree char *root = fixture();
    put(root, "main.c", "#include <guess.h>\n#include <ignored.h>\n");
    put(root, "unrelated/guess.h", "");
    put(root, "build/generated/ignored.h", "");
    put(root, "build/compile_commands.json", "[{\"directory\":\"..\",\"file\":\"main.c\","
        "\"arguments\":[\"cl\",\"/external:Ibuild/generated\",\"@flags.rsp\"]}]");
    Project *project = scan(root);
    resolved(project, "main.c", "guess.h", NULL);
    resolved(project, "main.c", "ignored.h", NULL);
    g_assert_true(include_edge(project, "main.c", "ignored.h")->outside_project);
    g_assert_cmpuint(project->notes->len, >, 0);
    project_free(project);
    remove_tree(root);
}

static void test_compile_invalid(void)
{
    g_autofree char *root = fixture();
    put(root, "main.c", "#include \"api.h\"\n");
    put(root, "lib/api.h", "");
    const char *cases[] = {"{", "{}", "[null,1,{}]",
        "[{\"directory\":\"..\",\"file\":\"main.c\",\"arguments\":[\"cc\",false]}]",
        "[{\"directory\":\"..\",\"file\":\"main.c\",\"arguments\":[]}]",
        "[{\"directory\":\"..\",\"file\":\"main.c\",\"command\":\"\"}]"};
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        put(root, "build/compile_commands.json", cases[i]);
        Project *project = scan(root);
        g_assert_cmpuint(project->notes->len, >, 0);
        g_assert_cmpuint(project->compile_units, ==, 0);
        g_assert_false(include_edge(project, "main.c", "api.h")->metadata);
        g_assert_cmpint(include_edge(project, "main.c", "api.h")->to, ==, project_find_file(project, "lib/api.h"));
        project_free(project);
    }
    remove_tree(root);
}

static void cmake_fixture(const char *root)
{
    put(root, "main.c", "#include <choice.h>\n");
    put(root, "debug/choice.h", "");
    put(root, "release/choice.h", "");
    put(root, "CMakeLists.txt", "project(fixture C)\nadd_library(core STATIC main.c)\nadd_executable(app main.c)\ntarget_link_libraries(app PRIVATE core)\n");
    put(root, "build/.cmake/api/v1/reply/index-2026.json",
        "{\"objects\":[{\"kind\":\"codemodel\",\"version\":{\"major\":2},\"jsonFile\":\"model.json\"}]}");
    put(root, "build/.cmake/api/v1/reply/model.json",
        "{\"paths\":{\"source\":\"..\"},\"configurations\":["
        "{\"name\":\"Debug\",\"targets\":[{\"jsonFile\":\"app-debug.json\"},{\"jsonFile\":\"core.json\"}]},"
        "{\"name\":\"Release\",\"targets\":[{\"jsonFile\":\"app-release.json\"},{\"jsonFile\":\"core.json\"}]}]}");
    put(root, "build/.cmake/api/v1/reply/core.json", "{\"id\":\"core-id\",\"name\":\"core\",\"type\":\"STATIC_LIBRARY\"}");
    for (guint i = 0; i < 2; i++) {
        g_autofree char *name = g_strdup_printf("build/.cmake/api/v1/reply/app-%s.json", i ? "release" : "debug");
        g_autofree char *json = g_strdup_printf("{\"id\":\"app-id\",\"name\":\"app\",\"type\":\"EXECUTABLE\","
            "\"artifacts\":[{\"path\":\"%s/app.exe\"}],\"backtrace\":0,"
            "\"backtraceGraph\":{\"files\":[\"CMakeLists.txt\"],\"nodes\":[{\"file\":0,\"line\":3},{\"file\":0,\"line\":4}]},"
            "\"dependencies\":[{\"id\":\"core-id\",\"backtrace\":1}],"
            "\"sources\":[{\"path\":\"main.c\",\"compileGroupIndex\":0}],"
            "\"compileGroups\":[{\"includes\":[{\"path\":\"%s\"}]}]}", i ? "Release" : "Debug", i ? "release" : "debug");
        put(root, name, json);
    }
}

static Project *scan_config(const char *root, const char *config)
{
    g_autoptr(GError) error = NULL;
    Project *project = project_scan_build(root, "build", config, &error);
    g_assert_no_error(error);
    g_assert_nonnull(project);
    return project;
}

static void test_cmake_configurations(void)
{
    g_autofree char *root = fixture();
    cmake_fixture(root);
    /* An unlabelled database entry must not overwrite a selected multi-config group. */
    put(root, "build/compile_commands.json", "[{\"directory\":\"..\",\"file\":\"main.c\","
        "\"arguments\":[\"cc\",\"-Iunrelated\"]}]");
    Project *project = scan_config(root, "Release");
    g_assert_cmpstr(project->configuration, ==, "Release");
    g_assert_cmpuint(project->configurations->len, ==, 2);
    g_assert_cmpuint(project->targets->len, ==, 2);
    g_assert_cmpuint(project->target_edges->len, ==, 1);
    resolved(project, "main.c", "choice.h", "release/choice.h");
    ProjectTarget *app = g_ptr_array_index(project->targets, 0);
    ProjectTargetEdge *edge = g_ptr_array_index(project->target_edges, 0);
    g_assert_cmpstr(app->definition, ==, "CMakeLists.txt");
    g_assert_cmpuint(app->line, ==, 3);
    g_assert_cmpuint(app->sources->len, ==, 1);
    g_assert_cmpuint(edge->from, ==, 0);
    g_assert_cmpuint(edge->to, ==, 1);
    g_assert_cmpuint(edge->line, ==, 4);
    project_free(project);
    project = scan_config(root, "Debug");
    resolved(project, "main.c", "choice.h", "debug/choice.h");
    project_free(project);
    project = scan_config(root, "Missing");
    g_assert_cmpuint(project->targets->len, ==, 0);
    g_assert_cmpuint(project->notes->len, >, 0);
    project_free(project);
    remove_tree(root);
}

static void test_cmake_invalid_reply(void)
{
    g_autofree char *root = fixture();
    cmake_fixture(root);
    put(root, "build/.cmake/api/v1/reply/index-2027.json",
        "{\"objects\":[{\"kind\":\"codemodel\",\"version\":{\"major\":2},\"jsonFile\":\"../outside.json\"}]}");
    Project *project = scan(root);
    g_assert_cmpuint(project->targets->len, ==, 0);
    g_assert_cmpuint(project->notes->len, >, 0);
    g_assert_nonnull(strstr(g_ptr_array_index(project->notes, 0), "traversal rejected"));
    project_free(project);
    remove_tree(root);
}

static void test_compiler_configurations(void)
{
    g_autofree char *root = fixture();
    put(root, "main.c", "#include <choice.h>\n");
    put(root, "debug/choice.h", "");
    put(root, "release/choice.h", "");
    put(root, "build/compile_commands.json", "["
        "{\"directory\":\"..\",\"file\":\"main.c\",\"arguments\":[\"cc\",\"-DCMAKE_INTDIR=\\\"Debug\\\"\",\"-Idebug\"]},"
        "{\"directory\":\"..\",\"file\":\"main.c\",\"arguments\":[\"cc\",\"-DCMAKE_INTDIR=\\\"Release\\\"\",\"-Irelease\"]}]");
    Project *project = scan_config(root, "Release");
    resolved(project, "main.c", "choice.h", "release/choice.h");
    project_free(project);
    remove_tree(root);
}

static void test_explicit_build_folder(void)
{
    g_autofree char *root = fixture();
    put(root, "src/main.c", "#include <api.h>\n");
    put(root, "include/api.h", "");
    put(root, "build/Windows с пробелом/compile_commands.json",
        "[{\"directory\":\"../..\",\"file\":\"src/main.c\",\"command\":\"cc -Iinclude src/main.c\"}]");
    Project *project = scan(root);
    g_assert_cmpuint(project->compile_units, ==, 0);
    project_free(project);
    g_autoptr(GError) error = NULL;
    project = project_scan_build(root, "build/Windows с пробелом", NULL, &error);
    g_assert_no_error(error);
    resolved(project, "src/main.c", "api.h", "include/api.h");
#ifdef G_OS_WIN32
    g_assert_cmpint(project_find_file(project, "SRC\\MAIN.C"), ==, project_find_file(project, "src/main.c"));
#endif
    project_free(project);
    remove_tree(root);
}

static void test_layout(void)
{
    g_autoptr(GArray) edges = g_array_new(FALSE, FALSE, sizeof(LayoutEdge));
    LayoutEdge links[] = {{0,1},{0,2},{1,3},{2,3},{3,4},{4,3},{4,5},{0,5},{6,6},{90,1},{1,90}};
    g_array_append_vals(edges, links, G_N_ELEMENTS(links));
    GraphLayout *layout = graph_layout_build(7, edges), *again = graph_layout_build(7, edges);
    g_assert_cmpuint(layout->cycles, ==, 2);
    LayoutNode a = g_array_index(layout->nodes, LayoutNode, 3), b = g_array_index(layout->nodes, LayoutNode, 4);
    g_assert_true(a.cycle); g_assert_true(b.cycle);
    g_assert_cmpuint(a.component, ==, b.component);
    g_assert_cmpuint(a.layer, ==, b.layer);
    for (guint i = 0; i < 7; i++) {
        LayoutNode node = g_array_index(layout->nodes, LayoutNode, i), other = g_array_index(again->nodes, LayoutNode, i);
        g_assert_cmpfloat(node.x, ==, other.x); g_assert_cmpfloat(node.y, ==, other.y);
        g_assert_cmpfloat(node.x + 200, <=, layout->width); g_assert_cmpfloat(node.y + 62, <=, layout->height);
        for (guint j = i + 1; j < 7; j++) {
            LayoutNode next = g_array_index(layout->nodes, LayoutNode, j);
            g_assert_true(node.x + 200 <= next.x || next.x + 200 <= node.x || node.y + 62 <= next.y || next.y + 62 <= node.y);
        }
    }
    for (guint i = 0; i < G_N_ELEMENTS(links); i++) {
        LayoutEdge edge = links[i]; if (edge.from >= 7 || edge.to >= 7) continue;
        LayoutNode from = g_array_index(layout->nodes, LayoutNode, edge.from), to = g_array_index(layout->nodes, LayoutNode, edge.to);
        if (from.component != to.component) g_assert_cmpuint(from.layer, <, to.layer);
    }
    graph_layout_free(layout); graph_layout_free(again);
    layout = graph_layout_build(0, edges); g_assert_cmpuint(layout->nodes->len, ==, 0); graph_layout_free(layout);
}

static void test_layout_bounds(void)
{
    g_autoptr(GArray) edges = g_array_new(FALSE, FALSE, sizeof(LayoutEdge));
    for (guint i = 0; i < 299; i++) { LayoutEdge e = {i, i+1}; g_array_append_val(edges, e); }
    GraphLayout *layout = graph_layout_build(300, edges);
    g_assert_cmpuint(layout->nodes->len, ==, GRAPH_LAYOUT_LIMIT);
    g_assert_cmpuint(g_array_index(layout->nodes, LayoutNode, GRAPH_LAYOUT_LIMIT-1).layer, ==, GRAPH_LAYOUT_LIMIT-1);
    graph_layout_free(layout);
}

static void test_group_layout(void)
{
    g_autoptr(GArray) edges = g_array_new(FALSE, FALSE, sizeof(LayoutEdge));
    LayoutEdge links[] = {{0,1},{1,2},{2,3},{3,2},{4,1},{4,5},{5,6},{0,6}};
    g_array_append_vals(edges, links, G_N_ELEMENTS(links));
    const char *groups[] = {"src", "include", "src", "include", "tests", "tests", "src"};
    GraphLayout *layout = graph_layout_build(7, edges), *again = graph_layout_build(7, edges);
    graph_layout_group(layout, groups); graph_layout_group(again, groups);
    for (guint i = 0; i < 7; i++) {
        LayoutNode node = g_array_index(layout->nodes, LayoutNode, i);
        LayoutNode repeat = g_array_index(again->nodes, LayoutNode, i);
        g_assert_cmpfloat(node.y, ==, repeat.y);
        g_assert_cmpfloat(node.y + 62, <=, layout->height);
        for (guint j = i + 1; j < 7; j++) {
            LayoutNode other = g_array_index(layout->nodes, LayoutNode, j);
            if (g_str_equal(groups[i], groups[j]))
                g_assert_true(node.x + 200 <= other.x || other.x + 200 <= node.x || node.y + 62 <= other.y || other.y + 62 <= node.y);
        }
    }
    /* Entire directory containers, including their title/margins, stay disjoint. */
    const char *unique[] = {"src", "include", "tests"}; double previous = 0;
    for (guint g = 0; g < G_N_ELEMENTS(unique); g++) {
        double top = G_MAXDOUBLE, bottom = 0;
        for (guint i = 0; i < 7; i++) if (g_str_equal(groups[i], unique[g])) {
            LayoutNode node = g_array_index(layout->nodes, LayoutNode, i);
            top = MIN(top, node.y - 35); bottom = MAX(bottom, node.y + 80);
        }
        g_assert_cmpfloat(top, >, previous); previous = bottom;
    }
    for (guint i = 0; i < G_N_ELEMENTS(links); i++) {
        LayoutNode from = g_array_index(layout->nodes, LayoutNode, links[i].from);
        LayoutNode to = g_array_index(layout->nodes, LayoutNode, links[i].to);
        if (from.component != to.component) g_assert_cmpuint(from.layer, <, to.layer);
    }
    graph_layout_free(layout); graph_layout_free(again);
}

static void test_build_profile(void)
{
    g_autofree char *root = fixture(); cmake_fixture(root);
    put(root, "build/.cmake/api/v1/reply/index-2027.json", "{\"cmake\":{\"version\":{\"string\":\"3.31\"},"
        "\"generator\":{\"name\":\"Ninja Multi-Config\",\"multiConfig\":true}},\"objects\":["
        "{\"kind\":\"codemodel\",\"version\":{\"major\":2},\"jsonFile\":\"model.json\"},"
        "{\"kind\":\"toolchains\",\"version\":{\"major\":1},\"jsonFile\":\"toolchains.json\"}]}");
    put(root, "build/.cmake/api/v1/reply/toolchains.json", "{\"toolchains\":[{\"language\":\"C\","
        "\"compiler\":{\"id\":\"GNU\",\"version\":\"14.2\",\"path\":\"C:/toolchain/bin/gcc.exe\"}},{\"language\":\"CXX\",\"compiler\":{}}]}");
    Project *project = scan_config(root, "Release");
    g_assert_cmpstr(project->generator, ==, "Ninja Multi-Config"); g_assert_true(project->multi_config);
    g_assert_cmpstr(project->cmake_version, ==, "3.31"); g_assert_cmpuint(project->compilers->len, ==, 2);
    ProjectCompiler *compiler = g_ptr_array_index(project->compilers, 0);
    g_assert_cmpstr(compiler->version, ==, "14.2");
    g_autofree char *text = project_build_summary(project, 0);
    g_assert_nonnull(strstr(text, "Release")); g_assert_nonnull(strstr(text, "app")); g_assert_nonnull(strstr(text, "GNU"));
    g_autofree char *json = project_to_json(project); g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
    g_assert_nonnull(strstr(json, "\"version\":\"14.2\""));
    project_free(project); remove_tree(root);
}

static void test_cache_profile(void)
{
    g_autofree char *root = fixture(); put(root, "CMakeLists.txt", "project(example C)\n");
    put(root, "build/CMakeCache.txt", "# generated\nCMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_BUILD_TYPE:STRING=Debug\n"
        "CMAKE_C_COMPILER:FILEPATH=/toolchain/cc\nCMAKE_CXX_COMPILER:FILEPATH=/toolchain/c++\n");
    Project *project = scan(root);
    g_assert_cmpstr(project->generator, ==, "Ninja"); g_assert_cmpstr(project->configuration, ==, "Debug");
    g_assert_cmpuint(project->compilers->len, ==, 2); g_assert_null(project->cmake_version);
    g_assert_cmpuint(project->targets->len, ==, 0);
    project_free(project); remove_tree(root);
}

static void test_presets(void)
{
    g_autofree char *root = fixture();
    put(root, "CMakePresets.json", "{\"version\":6,\"include\":[\"config/base.json\"],"
        "\"configurePresets\":[{\"name\":\"debug\",\"inherits\":\"base\",\"cacheVariables\":{\"CMAKE_BUILD_TYPE\":{\"type\":\"STRING\",\"value\":\"Debug\"}}}],"
        "\"buildPresets\":[{\"name\":\"build-release\",\"configurePreset\":\"debug\",\"configuration\":\"Release\"}]}");
    put(root, "config/base.json", "{\"configurePresets\":[{\"name\":\"base\",\"hidden\":true,\"generator\":\"Ninja Multi-Config\",\"binaryDir\":\"${sourceDir}/build\"}]}");
    Project *project = scan(root); g_assert_cmpuint(project->presets->len, ==, 3);
    ProjectPreset *preset = g_ptr_array_index(project->presets, 0);
    g_assert_cmpstr(preset->generator, ==, "Ninja Multi-Config"); g_assert_cmpstr(preset->configuration, ==, "Debug");
    preset = g_ptr_array_index(project->presets, 1);
    g_assert_cmpstr(preset->configuration, ==, "Release"); g_assert_cmpstr(preset->generator, ==, "Ninja Multi-Config");
    g_assert_null(project->configuration); /* Declared presets are not active metadata. */
    project_free(project);
    put(root, "config/base.json", "{\"configurePresets\":[{\"name\":\"base\",\"inherits\":[\"debug\",\"base\"]}]}");
    project = scan(root); g_assert_cmpuint(project->notes->len, >, 0); project_free(project); remove_tree(root);
}

static void test_build_plan(void)
{
    g_autofree char *root = fixture(); cmake_fixture(root);
    put(root, "build/CMakeCache.txt", "CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config\n");
    Project *project = scan_config(root, "Release");
    g_autoptr(GError) error = NULL;
    BuildPlan *plan = build_plan_new(project, 0, "C:/tools with spaces/cmake.exe", &error);
    g_assert_no_error(error); g_assert_nonnull(plan);
    g_assert_cmpstr(plan->argv[0], ==, "C:/tools with spaces/cmake.exe");
    g_assert_cmpstr(plan->argv[1], ==, "--build"); g_assert_cmpstr(plan->argv[3], ==, "--config");
    g_assert_cmpstr(plan->argv[4], ==, "Release"); g_assert_cmpstr(plan->argv[6], ==, "app");
    build_plan_free(plan);
    ProjectTarget *target = g_ptr_array_index(project->targets, 0);
    g_free(target->name); target->name = g_strdup("--parallel");
    plan = build_plan_new(project, 0, "cmake", &error);
    g_assert_null(plan); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    project_free(project); remove_tree(root);
}
static void test_diagnostics(void)
{
    g_autofree char *root = fixture(); put(root, "src/МОДУЛЬ.c", "int main(void) {}\n");
    Project *project = scan(root); g_autofree char *build = g_build_filename(root, "build", NULL);
    BuildDiagnostic *diagnostic = build_diagnostic_parse("\033[31m../src/МОДУЛЬ.c:1:4: error: missing token\033[0m", build, project);
    g_assert_nonnull(diagnostic); g_assert_cmpstr(diagnostic->project_path, ==, "src/МОДУЛЬ.c");
    g_assert_cmpuint(diagnostic->line, ==, 1); g_assert_cmpuint(diagnostic->column, ==, 4); g_assert_false(diagnostic->warning);
    build_diagnostic_free(diagnostic);
    diagnostic = build_diagnostic_parse("C:\\Projects with spaces\\main.c(101,3): warning C4100: unused", build, NULL);
    g_assert_nonnull(diagnostic); g_assert_cmpstr(diagnostic->file, ==, "C:\\Projects with spaces\\main.c");
    g_assert_cmpuint(diagnostic->column, ==, 3); g_assert_true(diagnostic->warning); build_diagnostic_free(diagnostic);
    diagnostic = build_diagnostic_parse("src/МОДУЛЬ.c:1: warning: fallback root", build, project);
    g_assert_cmpstr(diagnostic->project_path, ==, "src/МОДУЛЬ.c"); build_diagnostic_free(diagnostic);
    g_assert_null(build_diagnostic_parse("[2/2] Linking", build, project));
    g_assert_null(build_diagnostic_parse("main.c:4294967296: error: too large", build, project));
    project_free(project); remove_tree(root);
}
static void test_session(void)
{
    g_autofree char *root = fixture(), *path = g_build_filename(root, "state", "workspace.ini", NULL);
    SessionStore *store = session_store_new(path);
    char *expanded[] = {"src", "include", NULL};
    SessionState state = {.file = "src/МОДУЛЬ.c", .target = "core", .page = "build", .cmake = "C:/Tools/cmake.exe",
        .line = 101, .column = 3, .width = 1080, .height = 720, .sidebar = 260, .editor = 470, .log = 520,
        .target_mode = TRUE, .overview = TRUE, .groups = TRUE, .log_visible = TRUE, .zoom = .75,
        .offset_x = 100.125, .offset_y = -50, .expanded = expanded, .configuration = "Release",
        .view = 2, .split = TRUE, .inspector = TRUE};
    g_autoptr(GError) error = NULL;
    g_assert_true(session_save(store, root, &state, &error)); g_assert_no_error(error); session_store_free(store);
    store = session_store_new(path); SessionState *loaded = session_load(store, root);
    g_assert_nonnull(loaded); g_assert_cmpstr(loaded->file, ==, state.file); g_assert_cmpstr(loaded->target, ==, "core");
    g_assert_cmpstr(loaded->cmake, ==, state.cmake); g_assert_cmpuint(loaded->line, ==, 101);
    g_assert_cmpuint(loaded->view, ==, 2); g_assert_true(loaded->split); g_assert_true(loaded->inspector);
    g_assert_cmpfloat(loaded->zoom, ==, .75); g_assert_cmpfloat(loaded->offset_x, ==, 100.125);
    g_assert_true(loaded->overview); g_assert_cmpstr(loaded->expanded[1], ==, "include"); session_state_free(loaded);
    state.width = 20; state.line = -1; state.zoom = NAN; state.offset_y = INFINITY;
    g_assert_true(session_save(store, root, &state, &error)); loaded = session_load(store, root);
    g_assert_cmpint(loaded->width, ==, 1420); g_assert_cmpint(loaded->line, ==, 1);
    g_assert_cmpfloat(loaded->zoom, ==, 1); g_assert_cmpfloat(loaded->offset_y, ==, 0); session_state_free(loaded);
    for (guint i = 0; i < 21; i++) {
        g_autofree char *name = g_strdup_printf("проект %u", i), *project_root = g_build_filename(root, name, NULL);
        g_assert_true(session_save(store, project_root, &state, &error));
    }
    g_auto(GStrv) recent = session_recent(store); g_assert_cmpuint(g_strv_length(recent), ==, 20);
    g_assert_null(session_load(store, root));
    g_assert_true(session_save(store, recent[0], &state, &error));
    g_auto(GStrv) updated = session_recent(store); g_assert_cmpuint(g_strv_length(updated), ==, 20);
    session_store_free(store); remove_tree(root);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/project/scan-and-build-detection", test_scan);
    g_test_add_func("/project/resolution-and-ambiguity", test_resolution);
    g_test_add_func("/project/comments-and-line-numbers", test_comments);
    g_test_add_func("/project/ignored-directories-and-symlinks", test_exclusions);
    g_test_add_func("/project/empty-and-missing", test_empty_and_missing);
    g_test_add_func("/project/relative-parent-include", test_relative_parent);
    g_test_add_func("/document/save-rescan-and-permissions", test_save_and_rescan);
    g_test_add_func("/document/external-change-conflict", test_save_conflict);
    g_test_add_func("/document/crlf-roundtrip", test_crlf);
    g_test_add_func("/document/binary-rejected", test_binary_rejected);
    g_test_add_func("/document/symlink-replacement", test_symlink_replaced);
    g_test_add_func("/project/unicode-paths", test_unicode);
    g_test_add_func("/project/search-path-and-basename", test_search_paths);
    g_test_add_func("/project/search-unicode-normalization", test_search_unicode);
    g_test_add_func("/project/search-empty-and-no-results", test_search_empty);
    g_test_add_func("/metadata/windows-command-quoting", test_windows_commands);
    g_test_add_func("/metadata/include-search-order", test_compile_order);
    g_test_add_func("/metadata/nested-headers-and-cycles", test_compile_inheritance);
    g_test_add_func("/metadata/conflicting-header-contexts", test_compile_conflict);
    g_test_add_func("/metadata/no-guessing-and-generated-headers", test_compile_no_guessing);
    g_test_add_func("/metadata/malformed-compile-database", test_compile_invalid);
    g_test_add_func("/metadata/cmake-configurations-and-origins", test_cmake_configurations);
    g_test_add_func("/metadata/reply-path-traversal", test_cmake_invalid_reply);
    g_test_add_func("/metadata/compiler-configurations", test_compiler_configurations);
    g_test_add_func("/metadata/explicit-unicode-build-folder", test_explicit_build_folder);
    g_test_add_func("/graph/layers-cycles-no-overlap-and-stability", test_layout);
    g_test_add_func("/graph/bounded-long-chain", test_layout_bounds);
    g_test_add_func("/graph/directory-containers-no-overlap", test_group_layout);
    g_test_add_func("/metadata/build-profile", test_build_profile);
    g_test_add_func("/metadata/cache-profile", test_cache_profile);
    g_test_add_func("/metadata/presets-includes-inheritance-and-cycles", test_presets);
    g_test_add_func("/build/plan-options-and-selected-target", test_build_plan);
    g_test_add_func("/build/gcc-msvc-diagnostics-and-relative-paths", test_diagnostics);
    g_test_add_func("/session/roundtrip-mru-and-invalid-values", test_session);
    return g_test_run();
}
