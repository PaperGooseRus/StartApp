#include "tools_ui.h"
#include "workspace_tools.h"
#include "build_panel.h"
#include "document.h"
#include <glib/gstdio.h>

typedef enum { OP_SCAN, OP_SEARCH, OP_IMPACT, OP_RULES, OP_COMPARE, OP_ENV, OP_EXPORT, OP_GIT_GRAPH } Operation;
typedef struct {
    Operation operation;
    char *root, *build, *config, *query, *other_build, *other_config, *rules, *file;
    gboolean targets, html;
    Project *project;
    GPtrArray *matches;
    char *text;
} Request;
struct ToolsUI {
    GtkApplication *application;
    GtkWindow *window;
    GtkDropDown *page_picker, *system, *presets, *target, *export_kind;
    GtkStack *pages;
    GtkEntry *catalog_query, *search, *impact_file, *other_build, *other_config, *tool, *build_dir, *generator, *config;
    GtkEntry *git_ref, *git_file, *arguments, *cwd, *debugger, *breakpoint, *export_path;
    GtkTextView *report, *rules;
    GtkListBox *results;
    GtkLabel *state;
    GtkWidget *actions;
    BuildPanel *process;
    Project *project;
    GCancellable *cancel;
    gboolean busy, configuring;
    char *pending_build, *root, *archive_root, *initial_build, *initial_config;
    gboolean git_graph;
    char *catalog_file;
    ToolsGit git;
    GHashTable *changed;
    gboolean collecting_git;
    gint selected_target;
    ToolsNavigate navigate;
    ToolsConfigured configured;
    gpointer data;
};
static const char *entry(GtkEntry *e) { return gtk_editable_get_text(GTK_EDITABLE(e)); }
static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *w = gtk_scrolled_window_new(); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(w), child); gtk_widget_set_vexpand(w, TRUE); return w;
}
static void report(ToolsUI *t, const char *text)
{ gtk_text_buffer_set_text(gtk_text_view_get_buffer(t->report), text ? text : "", -1); }
static void remove_snapshot(const char *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        GDir *dir = g_dir_open(path, 0, NULL); if (!dir) return; const char *name;
        while ((name = g_dir_read_name(dir))) { g_autofree char *child = g_build_filename(path, name, NULL); remove_snapshot(child); }
        g_dir_close(dir); g_rmdir(path);
    } else g_unlink(path);
}
static void request_free(gpointer data)
{
    Request *r = data; g_free(r->root); g_free(r->build); g_free(r->config); g_free(r->query); g_free(r->other_build); g_free(r->other_config); g_free(r->rules); g_free(r->file); g_free(r->text);
    project_free(r->project); g_clear_pointer(&r->matches, g_ptr_array_unref); g_free(r);
}
static void work(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; Request *r = data; g_autoptr(GError) error = NULL;
    r->project = project_scan_incremental(r->root, r->build, r->config, NULL, cancel, &error);
    if (!r->project) { g_task_return_error(task, g_steal_pointer(&error)); return; }
    if (r->operation == OP_SEARCH) r->matches = workspace_search(r->project, r->query, cancel, &error);
    else if (r->operation == OP_ENV) r->text = workspace_environment(r->project);
    else if (r->operation == OP_RULES) r->text = workspace_architecture(r->project, r->rules, &error);
    else if (r->operation == OP_COMPARE) {
        Project *other = project_scan_incremental(r->root, r->other_build, r->other_config, NULL, cancel, &error);
        if (other) { r->text = workspace_compare(r->project, other); project_free(other); }
    } else if (r->operation == OP_GIT_GRAPH) {
        g_autofree char *snapshot = g_build_filename(r->file, "source", NULL), *archive = g_build_filename(r->file, "snapshot.tar", NULL);
        if (workspace_extract_snapshot(archive, snapshot, &error)) {
            Project *old = project_scan_incremental(snapshot, "build-no-metadata", NULL, NULL, cancel, &error);
            if (old) {
                Project *current = project_scan_incremental(r->root, "build-no-metadata", NULL, NULL, cancel, &error);
                if (current) { g_autofree char *comparison = workspace_compare(old, current); r->text = g_strconcat("Git-снимок → рабочие файлы. Статический граф включений; без конфигурации и запуска исторических скриптов.\n", comparison, NULL); project_free(current); }
                project_free(old);
            }
        }
    } else if (r->operation == OP_EXPORT) {
        r->text = workspace_export(r->project, r->html, r->targets);
        g_autofree char *parent = g_path_get_dirname(r->file);
        g_mkdir_with_parents(parent, 0755);
        if (!g_file_set_contents_full(r->file, r->text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0644, &error)) g_clear_pointer(&r->text, g_free);
        else { g_free(r->text); r->text = g_strdup_printf("Карта экспортирована: %s\nSVG/HTML содержат до 200 узлов. Для полного графа используйте --inspect.", r->file); }
    } else if (r->operation == OP_IMPACT) {
        gint focus = project_find_file(r->project, r->query);
        g_autoptr(GArray) reachable = workspace_reachable(r->project, FALSE, focus, 2, 0);
        r->matches = g_ptr_array_new_with_free_func(text_match_free);
        for (guint i = 0; i < reachable->len; i++) {
            guint at = g_array_index(reachable, guint, i); ProjectFile *f = g_ptr_array_index(r->project->files, at);
            TextMatch *m = g_new0(TextMatch, 1); m->path = g_strdup(f->path); m->line = 1; m->text = g_strdup(i ? "транзитивно зависит от выбранного файла" : "выбранный файл"); g_ptr_array_add(r->matches, m);
        }
        GString *s = g_string_new("Потенциально затронутые файлы (статические include, без вычисления #if):\n");
        g_string_append_printf(s, "%u\nЦели с затронутыми исходниками:\n", reachable->len);
        g_autoptr(GHashTable) affected = g_hash_table_new(g_direct_hash, g_direct_equal);
        for (guint i = 0; i < reachable->len; i++) g_hash_table_add(affected, GUINT_TO_POINTER(g_array_index(reachable, guint, i)+1));
        for (guint i = 0; i < r->project->targets->len; i++) {
            ProjectTarget *target = g_ptr_array_index(r->project->targets, i);
            for (guint j = 0; j < target->sources->len; j++) if (g_hash_table_contains(affected, GUINT_TO_POINTER(g_array_index(target->sources, guint, j)+1))) { g_string_append_printf(s, "• %s\n", target->name); break; }
        }
        r->text = g_string_free(s, FALSE);
    }
    if (error) g_task_return_error(task, g_steal_pointer(&error)); else g_task_return_boolean(task, TRUE);
}
static void clear_results(ToolsUI *t)
{ GtkWidget *c; while ((c = gtk_widget_get_first_child(GTK_WIDGET(t->results)))) gtk_list_box_remove(t->results, c); }
static void result_add(ToolsUI *t, const char *text, const char *root, const char *path, guint line)
{
    GtkWidget *label = gtk_label_new(text); gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR); gtk_label_set_max_width_chars(GTK_LABEL(label), 100);
    gtk_list_box_append(t->results, label); GtkWidget *row = gtk_widget_get_parent(label);
    g_object_set_data_full(G_OBJECT(row), "root", g_strdup(root), g_free);
    g_object_set_data_full(G_OBJECT(row), "path", g_strdup(path), g_free);
    g_object_set_data(G_OBJECT(row), "line", GUINT_TO_POINTER(line));
}
static void populate_metadata(ToolsUI *t)
{
    Project *p = t->project;
    gtk_editable_set_text(GTK_EDITABLE(t->build_dir), p->build_directory ? p->build_directory : "build");
    gtk_editable_set_text(GTK_EDITABLE(t->other_build), p->build_directory ? p->build_directory : "build");
    gtk_editable_set_text(GTK_EDITABLE(t->cwd), p->root);
    const char *system = p->generator && g_str_has_prefix(p->generator, "Meson") ? "Meson" : p->generator && g_str_has_prefix(p->generator, "Make (") ? "Make" : "CMake";
    gtk_drop_down_set_selected(t->system, g_str_equal(system, "Meson") ? 1 : g_str_equal(system, "Make") ? 2 : 0);
    GtkStringList *presets = gtk_string_list_new(NULL); gtk_string_list_append(presets, "Без пресета");
    for (guint i = 0; i < p->presets->len; i++) { ProjectPreset *preset = g_ptr_array_index(p->presets, i); if (!preset->hidden && g_str_equal(preset->kind, "configure")) gtk_string_list_append(presets, preset->name); }
    gtk_drop_down_set_model(t->presets, G_LIST_MODEL(presets)); gtk_drop_down_set_selected(t->presets, 0); g_object_unref(presets);
    GtkStringList *targets = gtk_string_list_new(NULL); gtk_string_list_append(targets, "Все цели");
    for (guint i = 0; i < p->targets->len; i++) gtk_string_list_append(targets, ((ProjectTarget *)g_ptr_array_index(p->targets, i))->name);
    gtk_drop_down_set_model(t->target, G_LIST_MODEL(targets)); gtk_drop_down_set_selected(t->target, 0); g_object_unref(targets);
}
static void done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; ToolsUI *t = data; Request *r = g_task_get_task_data(G_TASK(result));
    g_autoptr(GError) error = NULL; gboolean ok = g_task_propagate_boolean(G_TASK(result), &error);
    t->busy = FALSE; gtk_widget_set_sensitive(t->actions, TRUE); g_clear_object(&t->cancel);
    gtk_label_set_text(t->state, ok ? "Готово" : error->message);
    clear_results(t);
    if (ok) {
        project_free(t->project); t->project = g_steal_pointer(&r->project);
        if (r->operation == OP_SCAN) { populate_metadata(t); if (t->selected_target >= 0 && (guint)t->selected_target < t->project->targets->len) gtk_drop_down_set_selected(t->target, (guint)t->selected_target+1); report(t, "Выберите инструмент. Открытие окна читает метаданные; внешние команды запускаются кнопками пользователя."); }
        else report(t, r->text ? r->text : "Результаты поиска по сохранённым файлам · не более 2000 совпадений");
        for (guint i = 0; r->matches && i < r->matches->len; i++) {
            TextMatch *m = g_ptr_array_index(r->matches, i); g_autofree char *label = g_strdup_printf("%s:%u · %s", m->path, m->line, m->text); result_add(t, label, r->root, m->path, m->line);
        }
    }
    if (r->operation == OP_GIT_GRAPH && t->archive_root) { remove_snapshot(t->archive_root); g_clear_pointer(&t->archive_root, g_free); }
    g_application_release(G_APPLICATION(t->application));
}
static void begin(ToolsUI *t, Operation op)
{
    if (tools_ui_busy(t) || !t->root) return;
    Request *r = g_new0(Request, 1); r->operation = op; r->root = g_strdup(t->root);
    r->build = g_strdup(t->project ? t->project->build_directory : t->initial_build); r->config = g_strdup(t->project ? t->project->configuration : t->initial_config);
    r->query = g_strdup(entry(op == OP_IMPACT ? t->impact_file : t->search));
    r->other_build = g_strdup(entry(t->other_build)); r->other_config = g_strdup(entry(t->other_config));
    GtkTextIter a, b; GtkTextBuffer *buffer = gtk_text_view_get_buffer(t->rules); gtk_text_buffer_get_bounds(buffer, &a, &b); r->rules = gtk_text_buffer_get_text(buffer, &a, &b, FALSE);
    r->html = gtk_drop_down_get_selected(t->export_kind) == 1; r->targets = gtk_drop_down_get_selected(t->export_kind) >= 2;
    if (gtk_drop_down_get_selected(t->export_kind) == 3) r->html = TRUE;
    r->file = op == OP_GIT_GRAPH ? g_strdup(t->archive_root) : g_canonicalize_filename(entry(t->export_path), t->root);
    t->cancel = g_cancellable_new(); t->busy = TRUE; gtk_widget_set_sensitive(t->actions, FALSE); gtk_label_set_text(t->state, "Чтение и анализ…");
    g_application_hold(G_APPLICATION(t->application)); GTask *task = g_task_new(NULL, t->cancel, done, t);
    g_task_set_task_data(task, r, request_free); g_task_run_in_thread(task, work); g_object_unref(task);
}
static void worker_clicked(GtkButton *button, gpointer data)
{ begin(data, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "operation"))); }
static void navigate(const char *path, guint line, gpointer data)
{ ToolsUI *t = data; if (t->navigate) t->navigate(t->root, path, line, t->data); }
static void process_done(gint code, gboolean stopped, gpointer data)
{
    ToolsUI *t = data; gtk_label_set_text(t->state, stopped ? "Процесс остановлен" : code ? "Процесс завершился с ошибкой" : "Процесс завершён");
    gtk_widget_set_sensitive(t->actions, TRUE);
    if (!code && !stopped && t->configuring && t->configured) t->configured(t->root, t->pending_build, t->data);
    if (!code && !stopped && t->git_graph) begin(t, OP_GIT_GRAPH);
    if ((code || stopped) && t->git_graph && t->archive_root) { remove_snapshot(t->archive_root); g_clear_pointer(&t->archive_root, g_free); }
    t->git_graph = FALSE;
    if (!code && !stopped && t->collecting_git && t->git) t->git(t->root, t->changed, t->data);
    t->collecting_git = FALSE;
    if (!code && !stopped && t->configuring) {
        g_free(t->project->build_directory); t->project->build_directory = g_strdup(t->pending_build);
        begin(t, OP_SCAN);
    }
    t->configuring = FALSE; g_clear_pointer(&t->pending_build, g_free);
    g_application_release(G_APPLICATION(t->application));
}
static void execute(ToolsUI *t, BuildPlan *plan, GError *error)
{
    if (!plan) { gtk_label_set_text(t->state, error ? error->message : "Команда недоступна"); return; }
    report(t, plan->display);
    if (!build_panel_start(t->process, t->project, plan)) { build_plan_free(plan); return; }
    gtk_widget_set_sensitive(t->actions, FALSE); g_application_hold(G_APPLICATION(t->application));
}
static const char *picked(GtkDropDown *picker)
{ GtkStringObject *o = gtk_drop_down_get_selected_item(picker); return o ? gtk_string_object_get_string(o) : ""; }
static void external_clicked(GtkButton *button, gpointer data)
{
    ToolsUI *t = data; if (tools_ui_busy(t) || !t->project) return;
    t->collecting_git = FALSE; t->git_graph = FALSE;
    const char *op = g_object_get_data(G_OBJECT(button), "external");
    g_autoptr(GError) error = NULL; BuildPlan *plan = NULL;
    const char *system = picked(t->system);
    gint target = (gint)gtk_drop_down_get_selected(t->target) - 1;
    if (g_str_equal(op, "configure")) {
        const char *preset = gtk_drop_down_get_selected(t->presets) ? picked(t->presets) : NULL;
        plan = workspace_configure(t->project, system, entry(t->tool), entry(t->build_dir), entry(t->generator), entry(t->config), preset, &error);
        if (plan) { t->configuring = TRUE; t->pending_build = g_canonicalize_filename(entry(t->build_dir), t->root); }
    } else if (g_str_equal(op, "build")) {
        /* The explicit build directory is also useful for Meson, without CMake metadata. */
        Project copy = *t->project; g_autofree char *dir = g_canonicalize_filename(entry(t->build_dir), t->root); copy.build_directory = dir;
        plan = workspace_build(&copy, system, entry(t->tool), target >= 0 ? picked(t->target) : NULL, &error);
    } else if (g_str_equal(op, "run") || g_str_equal(op, "debug")) {
        plan = workspace_launch(t->project, target, entry(t->arguments), entry(t->cwd), g_str_equal(op, "debug") ? entry(t->debugger) : NULL, entry(t->breakpoint), &error);
    } else {
        const char *ref = entry(t->git_ref), *file = entry(t->git_file);
        if (ref[0] == '-' || strchr(ref, '\n') || file[0] == '-') { gtk_label_set_text(t->state, "Ссылка/путь не должны начинаться с '-'"); return; }
        /* No shell, external diff, pagers or optional index writes. */
        const char *args[20] = {"git", "--no-pager", "--no-optional-locks", "-c", "core.quotePath=false", NULL}; guint n = 5;
        g_autofree char *archive_option = NULL;
        if (g_str_equal(op, "git-graph")) {
            if (t->archive_root) { gtk_label_set_text(t->state, "Для нового снимка закройте и заново откройте инструменты"); return; }
            t->archive_root = g_dir_make_tmp("startapp-git-snapshot-XXXXXX", &error);
            if (!t->archive_root) { gtk_label_set_text(t->state, error->message); return; }
            g_autofree char *archive = g_build_filename(t->archive_root, "snapshot.tar", NULL); archive_option = g_strconcat("--output=", archive, NULL);
            args[n++] = "archive"; args[n++] = "--format=tar"; args[n++] = archive_option; args[n++] = *ref ? ref : "HEAD"; t->git_graph = TRUE;
        } else if (g_str_equal(op, "git-status")) { t->collecting_git = TRUE; g_hash_table_remove_all(t->changed); args[n++] = "status"; args[n++] = "--no-renames"; args[n++] = "--short"; args[n++] = "--untracked-files=all"; }
        else { args[n++] = "diff"; args[n++] = "--no-ext-diff"; args[n++] = "--no-textconv";
            if (g_str_equal(op, "git-compare")) args[n++] = "--name-status";
            if (*ref) args[n++] = ref;
            args[n++] = "--";
            if (*file) args[n++] = file;
        }
        args[n] = NULL; plan = workspace_command(t->root, args);
    }
    execute(t, plan, error);
}
static void git_output(const char *line, gpointer data)
{
    ToolsUI *t = data;
    if (!t->collecting_git || strlen(line) < 4 || line[2] != ' ' || g_hash_table_size(t->changed) >= 10000) return;
    const char *raw = line + 3; g_autofree char *path = NULL;
    if (*raw == '"' && raw[strlen(raw)-1] == '"') { g_autofree char *inside = g_strndup(raw+1, strlen(raw)-2); path = g_strcompress(inside); }
    else path = g_strdup(raw);
    if (g_utf8_validate(path, -1, NULL)) g_hash_table_add(t->changed, g_steal_pointer(&path));
}
void tools_ui_set_git(ToolsUI *t, ToolsGit callback) { t->git = callback; }
static void rules_file(GtkButton *b, gpointer data)
{
    ToolsUI *t = data; if (!t->root || tools_ui_busy(t)) return;
    g_autofree char *path = g_build_filename(t->root, ".startapp-rules.ini", NULL); g_autoptr(GError) error = NULL;
    if (g_object_get_data(G_OBJECT(b), "save")) {
        GtkTextIter a, z; GtkTextBuffer *buffer = gtk_text_view_get_buffer(t->rules); gtk_text_buffer_get_bounds(buffer, &a, &z); g_autofree char *text = gtk_text_buffer_get_text(buffer, &a, &z, FALSE);
        if (strlen(text) > 1024*1024) { gtk_label_set_text(t->state, "Правила превышают 1 MiB"); return; }
        if (g_file_set_contents_full(path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0644, &error)) gtk_label_set_text(t->state, "Правила сохранены в .startapp-rules.ini");
    } else {
        Document *doc = document_open(path, &error);
        if (doc) { gtk_text_buffer_set_text(gtk_text_view_get_buffer(t->rules), doc->text, -1); document_free(doc); gtk_label_set_text(t->state, "Правила загружены"); }
    }
    if (error) gtk_label_set_text(t->state, error->message);
}

static void system_changed(GObject *o, GParamSpec *s, gpointer data)
{
    (void)o; (void)s; ToolsUI *t = data;
    const char *system = picked(t->system); gtk_editable_set_text(GTK_EDITABLE(t->tool), g_str_equal(system, "CMake") ? "cmake" : g_str_equal(system, "Meson") ? "meson" : "make");
}
static void page_changed(GObject *o, GParamSpec *s, gpointer data)
{
    (void)o; (void)s; ToolsUI *t = data; g_autofree char *name = g_strdup_printf("%u", gtk_drop_down_get_selected(t->page_picker)); gtk_stack_set_visible_child_name(t->pages, name);
}
static void catalog_filter(GtkEditable *editable, gpointer data)
{
    ToolsUI *t = data;
    for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(t->results)); row; row = gtk_widget_get_next_sibling(row)) {
        const char *root = g_object_get_data(G_OBJECT(row), "root");
        GtkWidget *label = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
        gtk_widget_set_visible(row, project_path_matches(GTK_IS_LABEL(label) ? gtk_label_get_text(GTK_LABEL(label)) : root ? root : "", gtk_editable_get_text(editable)));
    }
}
static void catalog_clicked(GtkButton *button, gpointer data)
{
    (void)button; ToolsUI *t = data; if (tools_ui_busy(t)) return;
    clear_results(t); g_autofree char *text = workspace_catalog(t->catalog_file, t->project, entry(t->impact_file), NULL); report(t, text);
    /* Catalogue data is inert; selecting a row goes through the application's save guard. */
    g_autoptr(GKeyFile) keys = g_key_file_new();
    if (!g_key_file_load_from_file(keys, t->catalog_file, G_KEY_FILE_NONE, NULL)) return;
    g_auto(GStrv) groups = g_key_file_get_groups(keys, NULL);
    for (guint i = 0; groups[i] && i < 100; i++) {
        g_autofree char *root = g_key_file_get_string(keys, groups[i], "root", NULL), *summary = g_key_file_get_string(keys, groups[i], "summary", NULL);
        g_autofree char *build = g_key_file_get_string(keys, groups[i], "build", NULL), *file = g_key_file_get_string(keys, groups[i], "file", NULL);
        if (!root) continue;
        g_autofree char *label = g_strdup_printf("%s\n%s · %s\nПоследний файл: %s", root, summary ? summary : "", build ? build : "", file ? file : ""); result_add(t, label, root, NULL, 0);
    }
    catalog_filter(GTK_EDITABLE(t->catalog_query), t);
}
static void row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box; ToolsUI *t = data;
    if (t->navigate) t->navigate(g_object_get_data(G_OBJECT(row), "root"), g_object_get_data(G_OBJECT(row), "path"), GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "line")), t->data);
}
void tools_ui_stop(ToolsUI *t)
{ if (t) { if (t->cancel) g_cancellable_cancel(t->cancel); build_panel_stop(t->process); } }
static void stop_clicked(GtkButton *button, gpointer data) { (void)button; tools_ui_stop(data); }
gboolean tools_ui_busy(ToolsUI *t) { return t && (t->busy || build_panel_running(t->process)); }
static gboolean close_requested(GtkWindow *w, gpointer data)
{ ToolsUI *t = data; if (tools_ui_busy(t)) { tools_ui_stop(t); return TRUE; } gtk_widget_set_visible(GTK_WIDGET(w), FALSE); return TRUE; }
static GtkEntry *field(GtkWidget *box, const char *label, const char *value)
{
    GtkWidget *name = gtk_label_new(label); gtk_label_set_xalign(GTK_LABEL(name), 0); gtk_box_append(GTK_BOX(box), name);
    GtkEntry *e = GTK_ENTRY(gtk_entry_new()); gtk_editable_set_text(GTK_EDITABLE(e), value ? value : ""); gtk_box_append(GTK_BOX(box), GTK_WIDGET(e)); return e;
}
static GtkWidget *button(GtkWidget *box, const char *label, GCallback cb, ToolsUI *t)
{ GtkWidget *b = gtk_button_new_with_label(label); g_signal_connect(b, "clicked", cb, t); gtk_box_append(GTK_BOX(box), b); return b; }
static void action(GtkWidget *box, const char *label, Operation op, ToolsUI *t)
{ GtkWidget *b = button(box, label, G_CALLBACK(worker_clicked), t); g_object_set_data(G_OBJECT(b), "operation", GUINT_TO_POINTER(op)); }
static void external(GtkWidget *box, const char *label, const char *op, ToolsUI *t)
{ GtkWidget *b = button(box, label, G_CALLBACK(external_clicked), t); g_object_set_data(G_OBJECT(b), "external", (gpointer)op); }
static GtkWidget *page(ToolsUI *t, guint i, const char *help)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); g_autofree char *name = g_strdup_printf("%u", i);
    gtk_stack_add_named(t->pages, scrolled(box), name);
    GtkWidget *label = gtk_label_new(help); gtk_label_set_wrap(GTK_LABEL(label), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(label), 34); gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_box_append(GTK_BOX(box), label); return box;
}
ToolsUI *tools_ui_new(GtkApplication *application, GtkWindow *parent, ToolsNavigate nav, ToolsConfigured configured, gpointer data)
{
    ToolsUI *t = g_new0(ToolsUI, 1); t->changed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL); t->application = application; t->navigate = nav; t->configured = configured; t->data = data;
    t->catalog_file = g_build_filename(g_get_user_config_dir(), "startapp", "catalog.ini", NULL);
    t->window = GTK_WINDOW(gtk_application_window_new(application)); gtk_window_set_transient_for(t->window, parent);
    gtk_window_set_title(t->window, "StartApp · инструменты проекта"); gtk_window_set_default_size(t->window, 1120, 820); g_signal_connect(t->window, "close-request", G_CALLBACK(close_requested), t);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_window_set_child(t->window, body);
    const char *names[] = {"Каталог проектов", "Поиск текста", "Последствия изменений", "Архитектура", "Сравнение конфигураций", "Git", "Окружение", "Конфигурация и сборка", "Запуск и отладка", "Экспорт карты", NULL};
    t->page_picker = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(names)); gtk_box_append(GTK_BOX(body), GTK_WIDGET(t->page_picker));
    g_signal_connect(t->page_picker, "notify::selected", G_CALLBACK(page_changed), t);
    GtkWidget *pane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL); gtk_widget_set_vexpand(pane, TRUE); gtk_box_append(GTK_BOX(body), pane);
    t->actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_widget_set_size_request(t->actions, 300, -1); gtk_paned_set_start_child(GTK_PANED(pane), t->actions); gtk_paned_set_position(GTK_PANED(pane), 330);
    t->pages = GTK_STACK(gtk_stack_new()); gtk_stack_set_vhomogeneous(t->pages, FALSE); gtk_widget_set_vexpand(GTK_WIDGET(t->pages), TRUE); gtk_box_append(GTK_BOX(t->actions), GTK_WIDGET(t->pages));
    GtkWidget *p = page(t, 0, "Сохранённые карточки проектов. Нажатие на результат открывает проект."); t->catalog_query = field(p, "Поиск по карточкам", ""); g_signal_connect(t->catalog_query, "changed", G_CALLBACK(catalog_filter), t); button(p, "Обновить каталог", G_CALLBACK(catalog_clicked), t);
    p = page(t, 1, "Поиск подстроки без учёта регистра в сохранённых UTF-8 файлах."); t->search = field(p, "Текст", ""); action(p, "Найти в проекте", OP_SEARCH, t);
    p = page(t, 2, "Все прямые и косвенные пользователи заголовка и затронутые цели."); t->impact_file = field(p, "Относительный путь файла", ""); action(p, "Рассчитать последствия", OP_IMPACT, t);
    p = page(t, 3, "Каждая секция запрещает включения from → to. Пути относительно корня, шаблоны * и ?. Проверка не изменяет проект.");
    t->rules = GTK_TEXT_VIEW(gtk_text_view_new()); gtk_text_view_set_monospace(t->rules, TRUE); gtk_text_buffer_set_text(gtk_text_view_get_buffer(t->rules), "[UI must not include platform]\nfrom=src/ui/*\nto=src/platform/*\n", -1); GtkWidget *rules_scroll = scrolled(GTK_WIDGET(t->rules)); gtk_widget_set_size_request(rules_scroll, -1, 160); gtk_box_append(GTK_BOX(p), rules_scroll); action(p, "Проверить правила и циклы", OP_RULES, t);
    button(p, "Прочитать .startapp-rules.ini", G_CALLBACK(rules_file), t); GtkWidget *save_rules = button(p, "Сохранить .startapp-rules.ini", G_CALLBACK(rules_file), t); g_object_set_data(G_OBJECT(save_rules), "save", GINT_TO_POINTER(1));
    p = page(t, 4, "A — текущие метаданные. B — другой каталог/конфигурация того же проекта. Сравниваются цели, исходники, связи и параметры компиляции."); t->other_build = field(p, "Каталог сборки B", "build"); t->other_config = field(p, "Конфигурация B", "Release"); action(p, "Сравнить A и B", OP_COMPARE, t);
    p = page(t, 5, "Git выполняется только по нажатию. Доступны статус и сравнение без изменения индекса и коммитов."); t->git_ref = field(p, "Базовый коммит или диапазон A..B", "HEAD"); t->git_file = field(p, "Путь (пусто — весь проект)", ""); external(p, "Статус файлов", "git-status", t); external(p, "Показать diff", "git-diff", t); external(p, "Сравнить файлы ревизий", "git-compare", t); external(p, "Сравнить граф коммита с рабочим", "git-graph", t);
    p = page(t, 6, "Проверка PATH, путей компиляторов и каталогов заголовков. Не устанавливает инструменты."); action(p, "Проверить окружение", OP_ENV, t);
    p = page(t, 7, "Конфигурация и сборка выполняют код проекта. Укажите инструмент и параметры; команда отображается в журнале. Для CMake пресет задаёт генератор и конфигурацию, каталог ниже переопределяет binaryDir.");
    const char *systems[] = {"CMake", "Meson", "Make", NULL}; t->system = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(systems)); gtk_box_append(GTK_BOX(p), GTK_WIDGET(t->system)); g_signal_connect(t->system, "notify::selected", G_CALLBACK(system_changed), t);
    t->tool = field(p, "Инструмент / полный путь", "cmake"); t->build_dir = field(p, "Каталог сборки", "build"); t->generator = field(p, "Генератор CMake (пусто — системный)", ""); t->config = field(p, "Тип сборки", "Debug");
    const char *preset_names[] = {"Без пресета", NULL}; t->presets = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(preset_names)); gtk_box_append(GTK_BOX(p), GTK_WIDGET(t->presets)); external(p, "Выполнить конфигурацию", "configure", t);
    const char *target_names[] = {"Все цели", NULL}; t->target = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(target_names)); gtk_box_append(GTK_BOX(p), GTK_WIDGET(t->target)); external(p, "Собрать", "build", t);
    p = page(t, 8, "Выберите исполняемую цель во вкладке конфигурации. Отладка останавливается в заданной точке, выводит стеки и локальные переменные и завершает сеанс. Для интерактивного ввода программе нужен внешний терминал."); t->arguments = field(p, "Аргументы (кавычки как в POSIX, без shell)", ""); t->cwd = field(p, "Рабочий каталог", ""); t->debugger = field(p, "GDB / LLDB (имя или путь)", "gdb"); t->breakpoint = field(p, "Точка: функция или файл:строка", "main"); external(p, "Запустить цель", "run", t); external(p, "Отладить до точки останова", "debug", t);
    p = page(t, 9, "Самостоятельный SVG или интерактивный HTML с поиском и выделением соседей. Экспортируется до 200 узлов; полный JSON доступен через --inspect.");
    const char *kinds[] = {"Файлы · SVG", "Файлы · HTML", "Цели · SVG", "Цели · HTML", NULL}; t->export_kind = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(kinds)); gtk_drop_down_set_selected(t->export_kind, 1); gtk_box_append(GTK_BOX(p), GTK_WIDGET(t->export_kind)); t->export_path = field(p, "Путь назначения", "build/project-map.html"); action(p, "Экспортировать карту", OP_EXPORT, t);
    GtkWidget *right = gtk_paned_new(GTK_ORIENTATION_VERTICAL); gtk_paned_set_end_child(GTK_PANED(pane), right);
    t->report = GTK_TEXT_VIEW(gtk_text_view_new()); gtk_text_view_set_editable(t->report, FALSE); gtk_text_view_set_monospace(t->report, TRUE); gtk_text_view_set_wrap_mode(t->report, GTK_WRAP_WORD_CHAR); gtk_paned_set_start_child(GTK_PANED(right), scrolled(GTK_WIDGET(t->report)));
    t->results = GTK_LIST_BOX(gtk_list_box_new()); gtk_list_box_set_activate_on_single_click(t->results, TRUE); g_signal_connect(t->results, "row-activated", G_CALLBACK(row_activated), t); gtk_paned_set_end_child(GTK_PANED(right), scrolled(GTK_WIDGET(t->results))); gtk_paned_set_position(GTK_PANED(right), 240);
    t->process = build_panel_new(navigate, process_done, t); build_panel_observe(t->process, git_output, t); gtk_widget_set_size_request(build_panel_widget(t->process), -1, 190); gtk_widget_set_vexpand(build_panel_widget(t->process), FALSE); gtk_box_append(GTK_BOX(body), build_panel_widget(t->process));
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); t->state = GTK_LABEL(gtk_label_new("Готово")); gtk_label_set_ellipsize(t->state, PANGO_ELLIPSIZE_END); gtk_widget_set_hexpand(GTK_WIDGET(t->state), TRUE); gtk_box_append(GTK_BOX(footer), GTK_WIDGET(t->state)); button(footer, "Остановить задачу", G_CALLBACK(stop_clicked), t); gtk_box_append(GTK_BOX(body), footer);
    return t;
}
void tools_ui_show(ToolsUI *t, const Project *p, const char *file, gint target)
{
    if (tools_ui_busy(t)) { gtk_window_present(t->window); return; }
    t->selected_target = target;
    g_free(t->root); t->root = g_strdup(p->root);
    g_free(t->initial_build); t->initial_build = g_strdup(p->build_directory);
    g_free(t->initial_config); t->initial_config = g_strdup(p->configuration);
    gtk_editable_set_text(GTK_EDITABLE(t->impact_file), file ? file : ""); gtk_editable_set_text(GTK_EDITABLE(t->git_file), file ? file : "");
    gtk_window_present(t->window);
    project_free(t->project); t->project = NULL; begin(t, OP_SCAN);
}
void tools_ui_free(ToolsUI *t)
{
    if (!t) return;
    g_assert(!tools_ui_busy(t)); gtk_window_destroy(t->window); build_panel_free(t->process); project_free(t->project);
    if (t->archive_root) { remove_snapshot(t->archive_root); g_free(t->archive_root); }
    g_hash_table_unref(t->changed); g_free(t->root); g_free(t->pending_build); g_free(t->catalog_file); g_free(t->initial_build); g_free(t->initial_config); g_free(t);
}

gint tools_ui_smoke_step(ToolsUI *t, guint step)
{
    if (tools_ui_busy(t)) return 0;
    if (!t->project) return -1;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(t->report); GtkTextIter a, z; gtk_text_buffer_get_bounds(buffer, &a, &z);
    g_autofree char *text = gtk_text_buffer_get_text(buffer, &a, &z, FALSE);
    switch (step) {
    case 0:
        g_free(t->catalog_file); t->catalog_file = g_build_filename(t->root, "build", ".startapp-test", "catalog.ini", NULL);
        gtk_drop_down_set_selected(t->page_picker, 1); gtk_editable_set_text(GTK_EDITABLE(t->search), "Context line 50"); begin(t, OP_SEARCH); break;
    case 1:
        if (!gtk_widget_get_first_child(GTK_WIDGET(t->results))) return -1;
        gtk_drop_down_set_selected(t->page_picker, 2); gtk_editable_set_text(GTK_EDITABLE(t->impact_file), "api.h"); begin(t, OP_IMPACT); break;
    case 2:
        if (!strstr(text, "затронутые") || !gtk_widget_get_first_child(GTK_WIDGET(t->results))) return -1;
        gtk_drop_down_set_selected(t->page_picker, 3); begin(t, OP_RULES); break;
    case 3:
        if (!strstr(text, "Нарушений: 0")) return -1;
        gtk_drop_down_set_selected(t->page_picker, 4); gtk_editable_set_text(GTK_EDITABLE(t->other_config), "Debug"); begin(t, OP_COMPARE); break;
    case 4:
        if (!strstr(text, "Изменений:")) return -1;
        gtk_drop_down_set_selected(t->page_picker, 6); begin(t, OP_ENV); break;
    case 5:
        if (!strstr(text, "clangd:") || !strstr(text, "cmake:")) return -1;
        gtk_drop_down_set_selected(t->page_picker, 9); gtk_drop_down_set_selected(t->export_kind, 1); begin(t, OP_EXPORT); break;
    case 6: {
        g_autofree char *path = g_build_filename(t->root, "build", "project-map.html", NULL), *html = NULL;
        if (!g_file_get_contents(path, &html, NULL, NULL) || !strstr(html, "<svg") || !strstr(html, "<script>")) return -1;
        gtk_drop_down_set_selected(t->page_picker, 0); catalog_clicked(NULL, t); break;
    }
    case 7: {
        GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(t->results)); if (!row) return -1;
        gtk_editable_set_text(GTK_EDITABLE(t->catalog_query), "not-a-project-XYZ");
        if (gtk_widget_get_visible(row)) return -1;
        gtk_editable_set_text(GTK_EDITABLE(t->catalog_query), "");
        if (!gtk_widget_get_visible(row)) return -1;
        break;
    }
    default: return -1;
    }
    return 1;
}
