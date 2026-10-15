#include "workspace_tools.h"
#include "recovery.h"
#include "language_client.h"
#include <glib/gstdio.h>
#include <string.h>

static char *root;
static void put(const char *name, const char *text)
{ g_autofree char *path = g_build_filename(root, name, NULL), *parent = g_path_get_dirname(path); g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0); g_assert_true(g_file_set_contents(path, text, -1, NULL)); }
static void remove_tree(const char *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        GDir *dir = g_dir_open(path, 0, NULL); g_assert_nonnull(dir); const char *name;
        while ((name = g_dir_read_name(dir))) { g_autofree char *child = g_build_filename(path, name, NULL); remove_tree(child); }
        g_dir_close(dir); g_assert_cmpint(g_rmdir(path), ==, 0);
    } else g_assert_cmpint(g_unlink(path), ==, 0);
}
static Project *fixture(void)
{
    root = g_dir_make_tmp("startapp-tools-XXXXXX", NULL); g_assert_nonnull(root);
    put("main.c", "#include \"api.h\"\n// ПрИвЕт Unicode\nint main(void) { return 0; }\n");
    put("api.h", "#include \"deep.h\"\n"); put("deep.h", "#include \"api.h\"\n");
    g_autoptr(GError) error = NULL; Project *p = project_scan(root, &error); g_assert_no_error(error); return p;
}
static void cleanup(Project *p) { project_free(p); remove_tree(root); g_clear_pointer(&root, g_free); }
static void test_analysis(void)
{
    Project *p = fixture(); gint main = project_find_file(p, "main.c"), api = project_find_file(p, "api.h"), deep = project_find_file(p, "deep.h");
    g_autoptr(GArray) a = workspace_reachable(p, FALSE, main, 1, 1), b = workspace_reachable(p, FALSE, main, 1, 0), c = workspace_reachable(p, FALSE, deep, 2, 0), d = workspace_reachable(p, FALSE, main, 2, 0);
    g_assert_cmpuint(a->len, ==, 2); g_assert_cmpuint(b->len, ==, 3); g_assert_cmpuint(c->len, ==, 3); g_assert_cmpuint(d->len, ==, 1);
    g_autofree char *explanation = workspace_explain(p, FALSE, (guint)main, (guint)api); g_assert_nonnull(strstr(explanation, "main.c:1")); g_assert_nonnull(strstr(explanation, "#include"));
    g_autoptr(GError) error = NULL; g_autofree char *rules = workspace_architecture(p, "[deny]\nfrom=main.*\nto=api.*\n", &error); g_assert_no_error(error); g_assert_nonnull(strstr(rules, "deny: main.c:1")); g_assert_nonnull(strstr(rules, "Цикл:"));
    g_autofree char *invalid = workspace_architecture(p, "[deny]\nfrom=x\n", &error); g_assert_null(invalid); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    cleanup(p);
}
static void test_search_cancel(void)
{
    Project *p = fixture(); g_autoptr(GError) error = NULL; g_autoptr(GPtrArray) matches = workspace_search(p, "привет", NULL, &error);
    g_assert_no_error(error); g_assert_cmpuint(matches->len, ==, 1); TextMatch *m = g_ptr_array_index(matches, 0); g_assert_cmpstr(m->path, ==, "main.c"); g_assert_cmpuint(m->line, ==, 2);
    g_autoptr(GCancellable) cancel = g_cancellable_new(); g_cancellable_cancel(cancel);
    g_autoptr(GPtrArray) stopped = workspace_search(p, "x", cancel, &error); g_assert_null(stopped); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&error);
    Project *aborted = project_scan_incremental(root, NULL, NULL, p, cancel, &error); g_assert_null(aborted); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    cleanup(p);
}
static void test_incremental(void)
{
    Project *p = fixture(); g_autoptr(GError) error = NULL;
    Project *next = project_scan_incremental(root, NULL, NULL, p, NULL, &error); g_assert_no_error(error); g_assert_cmpuint(next->reused_files, ==, 3);
    g_autofree char *old_json = project_to_json(p), *next_json = project_to_json(next); g_assert_cmpstr(old_json, ==, next_json); project_free(next);
    put("api.h", "#pragma once\n"); next = project_scan_incremental(root, NULL, NULL, p, NULL, &error); g_assert_no_error(error); g_assert_cmpuint(next->reused_files, ==, 2); g_assert_cmpuint(next->edges->len, ==, 2);
    g_autofree char *diff = workspace_compare(p, next); g_assert_nonnull(strstr(diff, "Include api.h → deep.h")); project_free(next); cleanup(p);
}
static void test_export_catalog(void)
{
    Project *p = fixture(); put("x&'.c", ""); Project *next = project_scan(root, NULL);
    g_autofree char *svg = workspace_export(next, FALSE, FALSE), *html = workspace_export(next, TRUE, FALSE);
    g_assert_nonnull(strstr(svg, "xmlns=")); g_assert_nonnull(strstr(svg, "x&amp;&apos;.c")); g_assert_nonnull(strstr(html, "toLocaleLowerCase")); g_assert_null(strstr(html, "<title>x&'"));
    g_autofree char *path = g_build_filename(root, "build", "catalog.ini", NULL); g_autoptr(GError) error = NULL;
    g_autofree char *card = workspace_catalog(path, p, "main.c", &error); g_assert_no_error(error); g_assert_nonnull(strstr(card, "Последний файл: main.c"));
    g_autofree char *loaded = workspace_catalog(path, NULL, NULL, &error); g_assert_no_error(error); g_assert_nonnull(strstr(loaded, root));
    project_free(next); cleanup(p);
}
static void test_recovery(void)
{
    Project *p = fixture(); g_autofree char *path = g_build_filename(root, "main.c", NULL), *dir = g_build_filename(root, "build", "recovery", NULL);
    Document *doc = document_open(path, NULL); g_autoptr(GError) error = NULL;
    g_assert_true(recovery_save(dir, doc, "unsaved 😀\n", &error)); g_assert_no_error(error);
    g_autofree char *text = recovery_load(dir, doc, &error); g_assert_no_error(error); g_assert_cmpstr(text, ==, "unsaved 😀\n");
    put("main.c", "external\n"); Document *external = document_open(path, NULL);
    g_autofree char *conflict = recovery_load(dir, external, &error); g_assert_null(conflict); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG); g_clear_error(&error);
    recovery_remove(dir, path); g_autofree char *gone = recovery_load(dir, doc, &error); g_assert_null(gone); g_assert_no_error(error);
    document_free(doc); document_free(external); cleanup(p);
}
static void test_make_meson(void)
{
    Project *p = fixture(); put("Makefile", "all: app\napp: main.o\nmain.o: main.c\n%.o: %.c\nVAR := value\n\techo not executed\n");
    Project *make = project_scan(root, NULL); g_assert_cmpuint(make->targets->len, ==, 3); g_assert_cmpuint(make->target_edges->len, ==, 2); g_assert_nonnull(strstr(make->generator, "approximation")); project_free(make);
    g_autofree char *source = g_build_filename(root, "main.c", NULL); g_autofree char *escaped_source = g_strescape(source, NULL);
    g_autofree char *json = g_strdup_printf("[{\"name\":\"app\",\"id\":\"app@exe\",\"type\":\"executable\",\"filename\":[\"app\"],\"target_sources\":[{\"sources\":[\"%s\"]}]}]", escaped_source);
    put("build/meson-info/intro-targets.json", json); Project *meson = project_scan_build(root, "build", NULL, NULL);
    g_assert_cmpuint(meson->targets->len, ==, 1); g_assert_nonnull(strstr(meson->generator, "Meson")); ProjectTarget *t = g_ptr_array_index(meson->targets, 0); g_assert_cmpuint(t->sources->len, ==, 1); g_assert_cmpstr(t->type, ==, "EXECUTABLE"); project_free(meson); cleanup(p);
}
static void test_plans(void)
{
    Project *p = fixture(); g_autoptr(GError) error = NULL;
    BuildPlan *plan = workspace_configure(p, "CMake", "cmake", "build with spaces", "Ninja", "Debug", NULL, &error); g_assert_no_error(error); g_assert_cmpstr(plan->argv[2], ==, root); g_assert_nonnull(strstr(plan->display, "build with spaces"));
    g_autofree char *query = g_build_filename(root, "build with spaces", ".cmake", "api", "v1", "query", "codemodel-v2", NULL); g_assert_true(g_file_test(query, G_FILE_TEST_IS_REGULAR)); build_plan_free(plan);
    plan = workspace_build(p, "Make", "make", "-bad", &error); g_assert_null(plan); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    plan = workspace_configure(p, "Make", "make", "build", "", "", NULL, &error); g_assert_null(plan); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    cleanup(p);
}
typedef struct { GMainLoop *loop; JsonNode *result; GError *error; } LspTest;
static void lsp_result(GObject *source, GAsyncResult *result, gpointer data)
{ (void)source; LspTest *t = data; t->result = language_client_finish(result, &t->error); g_main_loop_quit(t->loop); }
static JsonNode *query(LanguageClient *client, const char *path, const char *text, guint row, guint col, const char *method)
{
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE); LspTest t = {.loop = loop};
    g_assert_true(language_client_request(client, path, text, row, col, method, lsp_result, &t)); g_main_loop_run(loop); g_assert_no_error(t.error); g_assert_nonnull(t.result); return t.result;
}
static void test_clangd(void)
{
    g_autofree char *tool = g_find_program_in_path("clangd"); if (!tool) { g_test_skip("Optional clangd is not installed"); return; }
    Project *p = fixture(); put("api.h", "int answer(void);\n");
    const char *text = "#include \"api.h\"\nint answer(void) { return 42; }\nint main(void) { return answer(); }\n"; put("main.c", text);
    g_autofree char *path = g_build_filename(root, "main.c", NULL); LanguageClient *client = language_client_new(tool, root, NULL);
    g_autoptr(JsonNode) definition = query(client, path, text, 2, 25, "textDocument/definition");
    g_autofree char *def_json = json_to_string(definition, FALSE); g_assert_nonnull(strstr(def_json, "uri")); g_assert_nonnull(strstr(def_json, "range"));
    g_autoptr(JsonNode) completion = query(client, path, "#include \"api.h\"\nint answer(void) { return 42; }\nint main(void) { return ans }\n", 2, 27, "textDocument/completion");
    g_autofree char *comp_json = json_to_string(completion, FALSE); g_assert_nonnull(strstr(comp_json, "answer"));
    g_autoptr(JsonNode) diagnostics = query(client, path, "int main(void) { return missing_token; }\n", 0, 25, "diagnostics");
    g_autofree char *diag_json = json_to_string(diagnostics, FALSE); g_assert_nonnull(strstr(diag_json, "missing_token"));
    language_client_stop(client); language_client_unref(client); cleanup(p);
}
static void test_utf16(void)
{ g_assert_cmpuint(language_utf16_offset("a😀б", 2), ==, 3); g_assert_cmpuint(language_char_offset("a😀б", 3), ==, 2); g_assert_cmpuint(language_char_offset("a😀б", 2), ==, 1); }
static void test_snapshot_bounds(void)
{
    Project *p = fixture(); g_autofree char *archive = g_build_filename(root, "unsafe.tar", NULL), *destination = g_build_filename(root, "build", "snapshot", NULL);
    char tar[1536] = {0}; memcpy(tar, "../escape.c", 11); memcpy(tar+124, "00000000000", 11); tar[156] = '0';
    g_assert_true(g_file_set_contents(archive, tar, sizeof(tar), NULL)); g_autoptr(GError) error = NULL;
    g_assert_false(workspace_extract_snapshot(archive, destination, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    memset(tar, 0, sizeof(tar)); memcpy(tar, "pax", 3); memcpy(tar+124, "00000000007", 11); tar[156] = 'x'; memcpy(tar+512, "7 path=", 7);
    g_assert_true(g_file_set_contents(archive, tar, sizeof(tar), NULL));
    g_assert_false(workspace_extract_snapshot(archive, destination, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    cleanup(p);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/tools/impact-explain-rules-cycles", test_analysis); g_test_add_func("/tools/search-and-cancel", test_search_cancel);
    g_test_add_func("/tools/incremental-and-compare", test_incremental); g_test_add_func("/tools/export-and-catalog", test_export_catalog);
    g_test_add_func("/tools/recovery-conflict", test_recovery); g_test_add_func("/tools/make-meson-import", test_make_meson);
    g_test_add_func("/tools/configure-plans", test_plans); g_test_add_func("/language/utf16", test_utf16); g_test_add_func("/language/real-clangd", test_clangd);
    g_test_add_func("/tools/snapshot-traversal-and-malformed-pax", test_snapshot_bounds);
    return g_test_run();
}
