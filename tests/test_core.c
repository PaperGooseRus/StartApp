#define _XOPEN_SOURCE 700
#include "document.h"
#include "project.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    g_assert_cmpint(symlink(main, link), ==, 0);
    Project *project = scan(root);
    g_assert_cmpuint(project->files->len, ==, 1);
    g_assert_cmpuint(project->skipped, ==, 1);
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
    g_assert_cmpint(g_chmod(path, 0640), ==, 0);
    Document *document = open_document(path);
    GError *error = NULL;
    g_assert_true(document_save(document, "#include \"api.h\"\n", &error));
    g_assert_no_error(error);
    Project *project = scan(root);
    g_assert_cmpuint(project->edges->len, ==, 1);
    ProjectEdge *edge = g_ptr_array_index(project->edges, 0);
    g_assert_cmpint(edge->to, >=, 0);
    struct stat st;
    g_assert_cmpint(stat(path, &st), ==, 0);
    g_assert_cmpuint(st.st_mode & 0777, ==, 0640);
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
    g_assert_cmpint(symlink(target, path), ==, 0);
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
    return g_test_run();
}
