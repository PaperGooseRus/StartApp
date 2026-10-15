#include "app.h"
#include "document.h"
#include "editor.h"
#include "graph.h"
#include "theme.h"
#include "build_panel.h"
#include "session.h"
#include "tools_ui.h"
#include "language_ui.h"
#include "recovery.h"
#include "workspace_tools.h"
#include <glib/gstdio.h>
#include <math.h>
#ifdef G_OS_WIN32
#include <glib/gwin32.h>
#endif

typedef enum { ACTION_NONE, ACTION_FILE, ACTION_LOCATION, ACTION_PROJECT, ACTION_CLOSE, ACTION_RELOAD, ACTION_BUILD, ACTION_TAB_CLOSE } Action;

typedef struct { char *path; Document *document; GtkTextBuffer *buffer; GtkWidget *button; } EditorTab;

typedef struct {
    GPtrArray *tabs;
    EditorTab *current_tab;
    GtkBox *tab_bar;
    guint recovery_timer, monitor_timer;
    gboolean recovery_busy;
    char *recovery_dir;
    GPtrArray *monitors;
    GHashTable *git_changed;
    LanguageUI *language;
    GtkEntry *graph_search;
    GtkDropDown *graph_direction;
    GtkSpinButton *graph_depth;
    ToolsUI *tools;
    GCancellable *scan_cancel;
    gboolean deferred_rescan;
    char *queued_scan;
    gboolean queued_replace;
    GtkApplication *application;
    GtkWindow *window;
    GtkTextView *editor;
    GtkTextBuffer *buffer;
    GtkListView *files;
    GtkSingleSelection *selection;
    GtkSearchEntry *search;
    GtkDropDown *configuration_picker, *target_picker;
    GtkToggleButton *target_mode, *overview, *groups;
    GtkStack *sidebar_pages;
    GtkLabel *build_chain, *build_profile;
    GtkLabel *build_name;
    BuildPanel *build_panel;
    GtkWidget *build_button;
    GtkToggleButton *log_toggle;
    GtkEntry *cmake_entry;
    GtkLabel *build_command;
    GtkDropDown *recent_picker;
    GtkPaned *outer, *workspace, *main_pane;
    SessionStore *sessions;
    SessionState *restore;
    guint restore_tick, restore_frames;
    const char *smoke_build_mode;
    GtkWidget *metadata_button;
    char *build_directory, *configuration;
    gboolean updating_metadata;
    gint selected_target;
    GtkLabel *project_name, *summary, *file_name, *status, *position, *details, *search_result;
    GtkWidget *save_button, *reload_button, *scan_button;
    Graph *graph;
    Project *project;
    Document *document;
    char *selected;
    gboolean busy, closing, confirming;
    Action pending;
    char *pending_path;
    guint pending_line;
    gboolean smoke;
    guint smoke_stage;
    GtkTextBuffer *smoke_tab_buffer;
    gboolean smoke_link_pending;
    gint smoke_result;
    char *smoke_root;
    char *initial_path;
} App;

typedef struct { char *path, *build_directory, *configuration; gboolean replace; SessionState *restore; const Project *previous; } ScanRequest;

static void scan_project(App *app, const char *path, gboolean replace);
static gboolean save_document(App *app, gboolean rescan);
static void request_action(App *app, Action action, const char *path);
static void request_action_at(App *app, Action action, const char *path, guint line);
static void cursor_moved(GtkTextBuffer *buffer, const GtkTextIter *location,
                         GtkTextMark *mark, gpointer user_data);
static void select_current_file(App *app);
static void graph_target_selected(guint index, gpointer data);
static void update_build_controls(App *app);
static void save_session(App *app);
static void refresh_recent(App *app);
static void start_build(App *app);
static gboolean restore_view(GtkWidget *widget, GdkFrameClock *clock, gpointer data);
static void status(App *app, const char *message);
static void clear_tabs(App *app);
static void stash_tab(App *app);
static gboolean recovery_tick(gpointer data);
static void setup_monitors(App *app);
static void tools_clicked(GtkButton *button, gpointer data);
static void tools_configured(const char *root, const char *directory, gpointer data);
static void use_buffer(App *app, GtkTextBuffer *buffer);
static void show_error(App *app, const char *title, const char *message);

static gboolean building(App *app) { return build_panel_running(app->build_panel); }

static char *fixture_executable(void)
{
#ifdef G_OS_WIN32
    g_autofree char *prefix = g_win32_get_package_installation_directory_of_module(NULL);
    return g_build_filename(prefix, "startapp-cli.exe", NULL);
#else
    return g_file_read_link("/proc/self/exe", NULL);
#endif
}
static BuildPlan *current_build_plan(App *app, GError **error)
{
    if (app->smoke && app->project && app->project->build_directory) {
        BuildPlan *plan = g_new0(BuildPlan, 1); plan->argv = g_new0(char *, 4);
        plan->argv[0] = fixture_executable(); plan->argv[1] = g_strdup("--test-build-child");
        plan->argv[2] = g_strdup(app->smoke_build_mode ? app->smoke_build_mode : "success");
        plan->directory = g_strdup(app->project->build_directory); plan->display = g_strjoinv(" ", plan->argv); return plan;
    }
    g_autofree char *exe = g_find_program_in_path(gtk_editable_get_text(GTK_EDITABLE(app->cmake_entry)));
    return build_plan_new(app->project, app->selected_target, exe, error);
}
static void update_build_controls(App *app)
{
    if (!app->build_button) return;
    gboolean running = building(app);
    g_autoptr(GError) error = NULL; BuildPlan *plan = current_build_plan(app, &error);
    gtk_label_set_text(app->build_command, plan ? plan->display : error ? error->message : "Сборка недоступна");
    gtk_widget_set_tooltip_text(GTK_WIDGET(app->build_command), plan ? plan->display : NULL);
    gtk_widget_set_sensitive(app->build_button, plan && !running && !app->busy && !app->confirming);
    gtk_widget_set_sensitive(GTK_WIDGET(app->cmake_entry), !running && !app->busy);
    if (running) {
        gtk_widget_set_sensitive(app->scan_button, FALSE); gtk_widget_set_sensitive(app->metadata_button, FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(app->configuration_picker), FALSE); gtk_widget_set_sensitive(GTK_WIDGET(app->target_picker), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(app->target_mode), FALSE);
    }
    build_plan_free(plan);
}
static void log_toggled(GtkToggleButton *button, gpointer data)
{
    App *app = data; gboolean visible = gtk_toggle_button_get_active(button);
    gtk_widget_set_visible(build_panel_widget(app->build_panel), visible);
    if (visible) gtk_paned_set_position(app->main_pane, MAX(300, gtk_widget_get_height(GTK_WIDGET(app->main_pane)) - 220));
}
static void cmake_changed(GtkEditable *editable, gpointer data) { (void)editable; update_build_controls(data); }
static void build_clicked(GtkButton *button, gpointer data) { (void)button; request_action(data, ACTION_BUILD, NULL); }
static void build_navigate(const char *path, guint line, gpointer data)
{ request_action_at(data, ACTION_LOCATION, path, line); }
static void build_finished(gint code, gboolean stopped, gpointer data)
{
    App *app = data;
    gtk_widget_set_sensitive(app->scan_button, TRUE); gtk_widget_set_sensitive(app->metadata_button, app->project != NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(app->target_mode), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->configuration_picker), app->project && app->project->configurations->len > 1);
    gtk_widget_set_sensitive(GTK_WIDGET(app->target_picker), app->project && app->project->targets->len > 0);
    update_build_controls(app);
    status(app, stopped ? "Сборка остановлена" : code == 0 ? "Сборка завершена успешно" : "Сборка завершилась с ошибкой · откройте диагностику");
    if (app->project && ((!stopped && code == 0) || app->deferred_rescan)) { app->deferred_rescan = FALSE; scan_project(app, app->project->root, FALSE); }
    g_application_release(G_APPLICATION(app->application));
}
static void start_build(App *app)
{
    if (app->busy || building(app) || !app->project) return;
    g_autoptr(GError) error = NULL; BuildPlan *plan = current_build_plan(app, &error);
    if (!plan) { show_error(app, "Сборка недоступна", error ? error->message : "Не найден инструмент сборки"); return; }
    if (!build_panel_start(app->build_panel, app->project, plan)) { build_plan_free(plan); return; }
    g_application_hold(G_APPLICATION(app->application));
    gtk_toggle_button_set_active(app->log_toggle, TRUE); status(app, "Сборка выполняется по сохранённым файлам…"); update_build_controls(app);
}

static void save_session(App *app)
{
    if (!app->project || app->restore) return;
    GtkTextIter cursor; gtk_text_buffer_get_iter_at_mark(app->buffer, &cursor, gtk_text_buffer_get_insert(app->buffer));
    SessionState state = {0}; state.file = app->selected;
    state.build_directory = app->build_directory ? app->build_directory : app->project->build_directory;
    state.configuration = app->configuration ? app->configuration : app->project->configuration;
    if (app->selected_target >= 0 && (guint)app->selected_target < app->project->targets->len)
        state.target = ((ProjectTarget *)g_ptr_array_index(app->project->targets, (guint)app->selected_target))->name;
    state.page = (char *)gtk_stack_get_visible_child_name(app->sidebar_pages);
    state.cmake = (char *)gtk_editable_get_text(GTK_EDITABLE(app->cmake_entry));
    state.line = gtk_text_iter_get_line(&cursor) + 1; state.column = gtk_text_iter_get_line_offset(&cursor);
    state.width = gtk_widget_get_width(GTK_WIDGET(app->window)); state.height = gtk_widget_get_height(GTK_WIDGET(app->window));
    state.sidebar = gtk_paned_get_position(app->outer); state.editor = gtk_paned_get_position(app->workspace); state.log = gtk_paned_get_position(app->main_pane);
    state.target_mode = gtk_toggle_button_get_active(app->target_mode); state.overview = gtk_toggle_button_get_active(app->overview);
    state.groups = gtk_toggle_button_get_active(app->groups); state.log_visible = gtk_toggle_button_get_active(app->log_toggle);
    graph_get_transform(app->graph, &state.zoom, &state.offset_x, &state.offset_y); state.expanded = graph_get_expanded(app->graph);
    g_autoptr(GError) error = NULL;
    if (!session_save(app->sessions, app->project->root, &state, &error)) status(app, error->message);
    g_strfreev(state.expanded); refresh_recent(app);
    if (!app->smoke) { g_autofree char *card = workspace_catalog(NULL, app->project, app->selected, NULL); }
}
static gboolean restore_view(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    (void)widget; (void)clock; App *app = data;
    if (++app->restore_frames < 4) return G_SOURCE_CONTINUE;
    SessionState *state = app->restore;
    if (state) {
        graph_restore_transform(app->graph, state->zoom, state->offset_x, state->offset_y);
        if (app->document) {
            guint line = MIN(state->line, gtk_text_buffer_get_line_count(app->buffer));
            editor_go_to_line(app->editor, line);
            GtkTextIter cursor, end; gtk_text_buffer_get_iter_at_line(app->buffer, &cursor, (gint)line - 1);
            end = cursor; gtk_text_iter_forward_to_line_end(&end);
            gtk_text_iter_forward_chars(&cursor, MIN(state->column, gtk_text_iter_get_line_offset(&end)));
            gtk_text_buffer_place_cursor(app->buffer, &cursor);
        }
        session_state_free(state); app->restore = NULL;
    }
    app->restore_tick = 0; return G_SOURCE_REMOVE;
}

static void append_origin(GString *text, const char *kind, guint index,
                           const char *path, guint line)
{
    if (!path) { g_string_append(text, "источник недоступен"); return; }
    g_autofree char *escaped = g_markup_escape_text(path, -1);
    g_string_append_printf(text, "<a href=\"%s:%u\">%s:%u</a>", kind, index, escaped, line);
}

static void status(App *app, const char *message)
{
    gtk_label_set_text(app->status, message);
}

static void show_error(App *app, const char *title, const char *message)
{
    status(app, message);
    if (app->smoke) return;
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(dialog, message);
    gtk_alert_dialog_show(dialog, app->window);
    g_object_unref(dialog);
}

static void update_document_title(App *app)
{
    gboolean dirty = gtk_text_buffer_get_modified(app->buffer);
    g_autofree char *label = app->selected ?
        g_strdup_printf("%s%s", dirty ? "●  " : "", app->selected) :
        g_strdup("Выберите файл в дереве или на карте");
    gtk_label_set_text(app->file_name, label);
    gtk_widget_set_sensitive(app->save_button, app->document && dirty && !app->busy);
    gtk_widget_set_sensitive(app->reload_button, app->document && !app->busy);
}

static void update_details(App *app)
{
    if (!app->project) return;
    g_autofree char *passport = project_build_summary(app->project, app->selected_target);
    gtk_label_set_text(app->build_profile, passport);
    update_build_controls(app);
    if (gtk_toggle_button_get_active(app->target_mode)) {
        GString *details = g_string_new("<b>Граф целей сборки</b>\n");
        if (app->selected_target >= 0 && (guint)app->selected_target < app->project->targets->len) {
            ProjectTarget *target = g_ptr_array_index(app->project->targets, (guint)app->selected_target);
            g_autofree char *name = g_markup_escape_text(target->name, -1);
            g_autofree char *type = g_markup_escape_text(target->type, -1);
            g_string_append_printf(details, "%s · %s · %u исходников\n", name, type, target->sources->len);
            append_origin(details, "target", (guint)app->selected_target, target->definition, target->line);
            g_string_append_c(details, '\n');
            if (target->artifact) {
                g_autofree char *artifact = g_markup_escape_text(target->artifact, -1);
                g_string_append_printf(details, "Артефакт: %s\n", artifact);
            }
        }
        guint shown = 0;
        for (guint i = 0; i < app->project->target_edges->len; i++) {
            ProjectTargetEdge *edge = g_ptr_array_index(app->project->target_edges, i);
            if (app->selected_target >= 0 && (gint)edge->from != app->selected_target &&
                (gint)edge->to != app->selected_target) continue;
            if (++shown > 25) { g_string_append(details, "… остальные связи доступны в JSON\n"); break; }
            ProjectTarget *from = g_ptr_array_index(app->project->targets, edge->from);
            ProjectTarget *to = g_ptr_array_index(app->project->targets, edge->to);
            g_autofree char *a = g_markup_escape_text(from->name, -1);
            g_autofree char *b = g_markup_escape_text(to->name, -1);
            g_string_append_printf(details, "%s → %s · ", a, b);
            append_origin(details, "targetedge", i, edge->definition, edge->line);
            g_string_append_c(details, '\n');
        }
        if (!app->project->targets->len) g_string_append(details, "Выберите каталог с готовым CMake File API reply.\n");
        for (guint i = 0; i < app->project->notes->len && i < 5; i++) {
            g_autofree char *note = g_markup_escape_text(g_ptr_array_index(app->project->notes, i), -1);
            g_string_append_printf(details, "%s\n", note);
        }
        gtk_label_set_markup(app->details, details->str);
        g_string_free(details, TRUE);
        return;
    }
    GString *details = g_string_new("Нажмите на <b>путь:строку</b>, чтобы перейти к источнику связи.\n"
        "Пути заголовков берутся из метаданных, если они доступны. Условия #if не вычисляются.\n");
    gint selected = app->selected ? project_find_file(app->project, app->selected) : -1;
    guint shown = 0;
    for (guint i = 0; i < app->project->edges->len; i++) {
        ProjectEdge *edge = g_ptr_array_index(app->project->edges, i);
        if (selected >= 0 ? ((gint)edge->from != selected && edge->to != selected) :
            (edge->to >= 0 || edge->system)) continue;
        if (++shown > 25) { g_string_append(details, "… остальные связи доступны через --inspect\n"); break; }
        ProjectFile *from = g_ptr_array_index(app->project->files, edge->from);
        const char *kind = edge->outside_project ? "вне карты проекта" :
                           edge->to >= 0 ? "локальный" : edge->ambiguous ? "неоднозначный" :
                           edge->system ? "внешний / системный" : "не разрешён";
        g_autofree char *path = g_markup_escape_text(from->path, -1);
        g_autofree char *include = g_markup_escape_text(edge->include, -1);
        g_string_append_printf(details, "<a href=\"include:%u\">%s:%u</a> → %s [%s; %s]\n",
                               i, path, edge->line, include, kind, edge->metadata ? "метаданные" : "эвристика");
    }
    for (guint i = 0; i < app->project->notes->len && i < 5; i++) {
        g_autofree char *note = g_markup_escape_text(g_ptr_array_index(app->project->notes, i), -1);
        g_string_append_printf(details, "%s\n", note);
    }
    gtk_label_set_markup(app->details, details->str);
    g_string_free(details, TRUE);
}

static void buffer_changed(GtkTextBuffer *buffer, gpointer user_data)
{
    App *app = user_data;
    if (app->document && gtk_text_buffer_get_modified(buffer)) {
        if (app->recovery_timer) g_source_remove(app->recovery_timer);
        app->recovery_timer = g_timeout_add(1500, recovery_tick, app);
    }
    for (guint i = 0; app->tabs && i < app->tabs->len; i++) {
        EditorTab *tab = g_ptr_array_index(app->tabs, i); g_autofree char *name = g_path_get_basename(tab->path);
        g_autofree char *title = g_strdup_printf("%s%s", gtk_text_buffer_get_modified(tab->buffer) ? "● " : "", name); gtk_button_set_label(GTK_BUTTON(tab->button), title);
    }
    update_document_title(app);
    GtkTextIter cursor;
    GtkTextMark *insert = gtk_text_buffer_get_insert(buffer);
    gtk_text_buffer_get_iter_at_mark(buffer, &cursor, insert);
    cursor_moved(buffer, &cursor, insert, app);
}

static void cursor_moved(GtkTextBuffer *buffer, const GtkTextIter *location,
                         GtkTextMark *mark, gpointer user_data)
{
    if (mark != gtk_text_buffer_get_insert(buffer)) return;
    App *app = user_data;
    g_autofree char *position = g_strdup_printf("Строка %d · столбец %d",
        gtk_text_iter_get_line(location) + 1, gtk_text_iter_get_line_offset(location) + 1);
    gtk_label_set_text(app->position, position);
}

typedef struct { Document document; char *text; GtkTextBuffer *buffer; } RecoveryItem;
typedef struct { char *directory; GPtrArray *items; } RecoveryBatch;
static void recovery_item_free(gpointer data)
{ RecoveryItem *i = data; g_free(i->document.path); g_free(i->document.original); g_free(i->text); g_object_unref(i->buffer); g_free(i); }
static void recovery_batch_free(gpointer data)
{ RecoveryBatch *b = data; g_free(b->directory); g_ptr_array_unref(b->items); g_free(b); }
static void recover_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)cancel; RecoveryBatch *b = data; g_autoptr(GError) error = NULL;
    for (guint i = 0; i < b->items->len; i++) { RecoveryItem *item = g_ptr_array_index(b->items, i); if (!recovery_save(b->directory, &item->document, item->text, &error)) { g_task_return_error(task, g_steal_pointer(&error)); return; } }
    g_task_return_boolean(task, TRUE);
}
static void recover_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; App *app = data; g_autoptr(GError) error = NULL; g_task_propagate_boolean(G_TASK(result), &error);
    RecoveryBatch *batch = g_task_get_task_data(G_TASK(result));
    for (guint i = 0; i < batch->items->len; i++) { RecoveryItem *item = g_ptr_array_index(batch->items, i); if (app->closing || !gtk_text_buffer_get_modified(item->buffer)) recovery_remove(batch->directory, item->document.path); }
    app->recovery_busy = FALSE;
    if (error && !app->closing) status(app, error->message);
    g_application_release(G_APPLICATION(app->application));
}
static void recovery_add(RecoveryBatch *batch, Document *doc, GtkTextBuffer *buffer)
{
    if (!doc || !gtk_text_buffer_get_modified(buffer)) return;
    RecoveryItem *item = g_new0(RecoveryItem, 1); item->document.path = g_strdup(doc->path); item->document.original = g_memdup2(doc->original, doc->original_length); item->document.original_length = doc->original_length; item->buffer = g_object_ref(buffer);
    GtkTextIter a, z; gtk_text_buffer_get_bounds(buffer, &a, &z); item->text = gtk_text_buffer_get_text(buffer, &a, &z, FALSE); g_ptr_array_add(batch->items, item);
}
static gboolean recovery_tick(gpointer data)
{
    App *app = data;
    if (app->closing) { app->recovery_timer = 0; return G_SOURCE_REMOVE; }
    if (app->recovery_busy) return G_SOURCE_CONTINUE;
    app->recovery_timer = 0;
    RecoveryBatch *batch = g_new0(RecoveryBatch, 1); batch->directory = g_strdup(app->recovery_dir); batch->items = g_ptr_array_new_with_free_func(recovery_item_free);
    recovery_add(batch, app->document, app->buffer);
    for (guint i = 0; i < app->tabs->len; i++) { EditorTab *tab = g_ptr_array_index(app->tabs, i); if (tab != app->current_tab) recovery_add(batch, tab->document, tab->buffer); }
    if (!batch->items->len) { recovery_batch_free(batch); return G_SOURCE_REMOVE; }
    app->recovery_busy = TRUE; g_application_hold(G_APPLICATION(app->application)); GTask *task = g_task_new(NULL, NULL, recover_done, app); g_task_set_task_data(task, batch, recovery_batch_free); g_task_run_in_thread(task, recover_worker); g_object_unref(task); return G_SOURCE_REMOVE;
}

static void use_buffer(App *app, GtkTextBuffer *buffer)
{
    g_signal_handlers_disconnect_by_data(app->buffer, app);
    editor_use_buffer(app->editor, buffer); app->buffer = buffer;
    g_signal_connect(buffer, "changed", G_CALLBACK(buffer_changed), app);
    g_signal_connect(buffer, "modified-changed", G_CALLBACK(buffer_changed), app);
    g_signal_connect(buffer, "mark-set", G_CALLBACK(cursor_moved), app);
}
static void stash_tab(App *app)
{
    if (!app->current_tab) return;
    app->current_tab->document = app->document; app->document = NULL; app->current_tab = NULL;
}
static void tab_free(gpointer data)
{
    EditorTab *tab = data; document_free(tab->document); g_object_unref(tab->buffer); g_free(tab->path); g_free(tab);
}
static void clear_tabs(App *app)
{
    stash_tab(app);
    if (app->tabs) {
        for (guint i = 0; i < app->tabs->len; i++) gtk_box_remove(app->tab_bar, ((EditorTab *)g_ptr_array_index(app->tabs, i))->button);
        g_ptr_array_set_size(app->tabs, 0);
    }
}
static void tab_clicked(GtkButton *button, gpointer data)
{ App *app = data; const char *path = g_object_get_data(G_OBJECT(button), "path"); if (g_strcmp0(path, app->selected)) request_action(app, ACTION_FILE, path); }
static void pin_tab(GtkButton *button, gpointer data)
{
    (void)button; App *app = data;
    if (!app->document || app->current_tab || app->tabs->len >= 20) return;
    EditorTab *tab = g_new0(EditorTab, 1); tab->path = g_strdup(app->selected); tab->buffer = g_object_ref(app->buffer);
    g_autofree char *name = g_path_get_basename(tab->path); tab->button = gtk_button_new_with_label(name);
    g_object_set_data_full(G_OBJECT(tab->button), "path", g_strdup(tab->path), g_free); gtk_widget_set_tooltip_text(tab->button, tab->path);
    g_signal_connect(tab->button, "clicked", G_CALLBACK(tab_clicked), app); gtk_box_append(app->tab_bar, tab->button); g_ptr_array_add(app->tabs, tab); app->current_tab = tab;
}
static void close_tab(GtkButton *button, gpointer data) { (void)button; request_action(data, ACTION_TAB_CLOSE, NULL); }

static gboolean open_file(App *app, const char *relative)
{
    if (!app->project) return FALSE;
    g_autofree char *absolute = g_build_filename(app->project->root, relative, NULL);
    g_autoptr(GError) error = NULL;
    EditorTab *tab = NULL;
    for (guint i = 0; app->tabs && i < app->tabs->len; i++) { EditorTab *candidate = g_ptr_array_index(app->tabs, i); if (g_str_equal(candidate->path, relative)) { tab = candidate; break; } }
    if (tab == app->current_tab && tab) { select_current_file(app); return TRUE; }
    if (tab && !gtk_text_buffer_get_modified(tab->buffer)) {
        Document *disk = document_open(absolute, &error);
        if (!disk) { show_error(app, "Файл вкладки недоступен", error->message); return FALSE; }
        if (g_strcmp0(disk->text, tab->document->text)) {
            gtk_text_buffer_set_enable_undo(tab->buffer, FALSE); gtk_text_buffer_set_text(tab->buffer, disk->text, -1); gtk_text_buffer_set_enable_undo(tab->buffer, TRUE); gtk_text_buffer_set_modified(tab->buffer, FALSE);
        }
        document_free(tab->document); tab->document = disk;
    }
    Document *document = tab ? tab->document : document_open(absolute, &error);
    if (!document) { show_error(app, "Не удалось открыть файл", error->message); return FALSE; }
    stash_tab(app); document_free(app->document);
    if (tab) { use_buffer(app, tab->buffer); tab->document = NULL; app->current_tab = tab; }
    else { GtkTextBuffer *next = editor_buffer_new(); use_buffer(app, next); g_object_unref(next); }
    app->document = document;
    g_free(app->selected);
    app->selected = g_strdup(relative);
    select_current_file(app);
    /* Reset undo history when switching files. */
    editor_set_language(app->editor, document->path);
    if (!tab) {
        gtk_text_buffer_set_enable_undo(app->buffer, FALSE); gtk_text_buffer_set_text(app->buffer, document->text, -1); gtk_text_buffer_set_enable_undo(app->buffer, TRUE); gtk_text_buffer_set_modified(app->buffer, FALSE);
        g_autofree char *draft = recovery_load(app->recovery_dir, document, &error);
        if (draft && g_strcmp0(draft, document->text)) { gtk_text_buffer_set_text(app->buffer, draft, -1); gtk_text_buffer_set_modified(app->buffer, TRUE); }
    }
    gtk_text_view_set_editable(app->editor, TRUE);
    graph_set_project(app->graph, app->project, project_find_file(app->project, relative));
    update_details(app);
    update_document_title(app);
    language_ui_context(app->language, app->project->root, app->project->build_directory, app->selected);
    status(app, "Файл открыт · изменения графа появятся после сохранения");
    if (error) status(app, error->message);
    else if (gtk_text_buffer_get_modified(app->buffer) && !tab) status(app, "Восстановлен черновик после прерывания работы · проверьте текст и сохраните");
    return TRUE;
}

static void graph_open(guint index, gpointer user_data)
{
    App *app = user_data;
    if (app->busy || !app->project || index >= app->project->files->len) return;
    ProjectFile *file = g_ptr_array_index(app->project->files, index);
    if (g_strcmp0(file->path, app->selected) != 0)
        request_action(app, ACTION_FILE, file->path);
}

static GListModel *tree_children(gpointer item, gpointer user_data)
{
    (void)user_data;
    GListStore *children = g_object_get_data(G_OBJECT(item), "children");
    return children ? G_LIST_MODEL(g_object_ref(children)) : NULL;
}

static void tree_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void)factory; (void)data;
    GtkWidget *expander = gtk_tree_expander_new();
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_margin_top(label, 5);
    gtk_widget_set_margin_bottom(label, 5);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), label);
    gtk_list_item_set_child(item, expander);
}

static void tree_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void)factory; App *app = data;
    GtkTreeListRow *row = gtk_list_item_get_item(item);
    GFile *file = gtk_tree_list_row_get_item(row);
    GtkTreeExpander *expander = GTK_TREE_EXPANDER(gtk_list_item_get_child(item));
    gtk_tree_expander_set_list_row(expander, row);
    g_autofree char *name = g_file_get_basename(file);
    gboolean directory = g_object_get_data(G_OBJECT(file), "children") != NULL;
    g_autofree char *relative = NULL;
    if (app->project) { g_autoptr(GFile) root = g_file_new_for_path(app->project->root); relative = g_file_get_relative_path(root, file); }
#ifdef G_OS_WIN32
    if (relative) g_strdelimit(relative, "\\", '/');
#endif
    gboolean changed = relative && app->git_changed && g_hash_table_contains(app->git_changed, relative);
    g_autofree char *label = g_strdup_printf("%s %s", directory ? "▸" : changed ? "●" : "·", name);
    gtk_label_set_text(GTK_LABEL(gtk_tree_expander_get_child(expander)), label);
    g_autofree char *path = g_file_get_path(file);
    gtk_widget_set_tooltip_text(GTK_WIDGET(expander), path);
}

static void tree_unbind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void)factory; (void)data;
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(gtk_list_item_get_child(item)), NULL);
}

static void tree_activate(GtkListView *view, guint position, gpointer user_data)
{
    (void)view;
    App *app = user_data;
    if (app->busy) return;
    g_autoptr(GtkTreeListRow) row = g_list_model_get_item(G_LIST_MODEL(app->selection), position);
    if (!row) return;
    GFile *file = gtk_tree_list_row_get_item(row);
    guint encoded = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(file), "file-index"));
    if (encoded) graph_open(encoded - 1, app);
    else gtk_tree_list_row_set_expanded(row, !gtk_tree_list_row_get_expanded(row));
}

static void populate_tree(App *app)
{
    if (!app->project) return;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(app->search));
    guint matches = 0;
    GListStore *root = g_list_store_new(G_TYPE_FILE);
    GHashTable *directories = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    g_hash_table_insert(directories, g_strdup(""), g_object_ref(root));
    for (guint i = 0; i < app->project->files->len; i++) {
        ProjectFile *file = g_ptr_array_index(app->project->files, i);
        if (!project_path_matches(file->path, query)) continue;
        matches++;
        g_auto(GStrv) parts = g_strsplit(file->path, "/", -1);
        GListStore *parent = root;
        GString *prefix = g_string_new(NULL);
        for (guint j = 0; parts[j]; j++) {
            if (j) g_string_append_c(prefix, '/');
            g_string_append(prefix, parts[j]);
            gboolean directory = parts[j + 1] != NULL;
            GListStore *children = directory ? g_hash_table_lookup(directories, prefix->str) : NULL;
            if (children) { parent = children; continue; }
            g_autofree char *path = g_build_filename(app->project->root, prefix->str, NULL);
            g_autoptr(GFile) item = g_file_new_for_path(path);
            if (directory) {
                children = g_list_store_new(G_TYPE_FILE);
                g_object_set_data_full(G_OBJECT(item), "children", children, g_object_unref);
                g_hash_table_insert(directories, g_strdup(prefix->str), g_object_ref(children));
            } else g_object_set_data(G_OBJECT(item), "file-index", GUINT_TO_POINTER(i + 1));
            g_list_store_append(parent, item);
            if (directory) parent = children;
        }
        g_string_free(prefix, TRUE);
    }
    g_hash_table_unref(directories);
    GtkTreeListModel *model = gtk_tree_list_model_new(G_LIST_MODEL(root), FALSE, TRUE,
                                                   tree_children, NULL, NULL);
    gtk_single_selection_set_model(app->selection, G_LIST_MODEL(model));
    g_object_unref(model);
    g_autofree char *result = matches ?
        g_strdup_printf("%u из %u файлов", matches, app->project->files->len) :
        g_strdup("Совпадений нет");
    gtk_label_set_text(app->search_result, result);
    select_current_file(app);
}

static void select_current_file(App *app)
{
    gtk_single_selection_set_selected(app->selection, GTK_INVALID_LIST_POSITION);
    /* Select the current document if it remains visible, without reopening it. */
    if (app->selected) {
        for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(app->selection)); i++) {
            g_autoptr(GtkTreeListRow) row = g_list_model_get_item(G_LIST_MODEL(app->selection), i);
            GFile *item = gtk_tree_list_row_get_item(row);
            guint encoded = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item), "file-index"));
            if (!encoded) continue;
            ProjectFile *file = g_ptr_array_index(app->project->files, encoded - 1);
            if (g_str_equal(file->path, app->selected)) {
                gtk_single_selection_set_selected(app->selection, i);
                break;
            }
        }
    }
}

static void search_changed(GtkSearchEntry *entry, gpointer data)
{
    (void)entry;
    App *app = data;
    if (!app->closing) populate_tree(app);
}

static void search_stopped(GtkSearchEntry *entry, gpointer data)
{
    App *app = data;
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    gtk_widget_grab_focus(GTK_WIDGET(app->editor));
}

static void search_activate(GtkSearchEntry *entry, gpointer data)
{
    (void)entry;
    App *app = data;
    if (!app->project || app->busy) return;
    /* SearchEntry debounces typing; Enter must use the newest query. */
    populate_tree(app);
    /* Enter opens the first file result, skipping parent directory rows. */
    for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(app->selection)); i++) {
        g_autoptr(GtkTreeListRow) row = g_list_model_get_item(G_LIST_MODEL(app->selection), i);
        GFile *file = gtk_tree_list_row_get_item(row);
        if (g_object_get_data(G_OBJECT(file), "file-index")) {
            tree_activate(app->files, i, app);
            gtk_widget_grab_focus(GTK_WIDGET(app->editor));
            break;
        }
    }
}

static void scan_request_free(gpointer data)
{
    ScanRequest *request = data;
    g_free(request->path);
    g_free(request->build_directory);
    g_free(request->configuration);
    session_state_free(request->restore);
    g_free(request);
}

static void scan_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)cancel;
    ScanRequest *request = data;
    g_autoptr(GError) error = NULL;
    Project *project = project_scan_incremental(request->path, request->build_directory, request->configuration, request->previous, cancel, &error);
    if (project) g_task_return_pointer(task, project, (GDestroyNotify)project_free);
    else g_task_return_error(task, g_steal_pointer(&error));
}

static void scan_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)source;
    App *app = user_data;
    GTask *task = G_TASK(result);
    ScanRequest *request = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    Project *project = g_task_propagate_pointer(task, &error);
    app->busy = FALSE;
    g_clear_object(&app->scan_cancel);
    if (app->closing) {
        project_free(project);
        g_application_release(G_APPLICATION(app->application));
        return;
    }
    gtk_text_view_set_editable(app->editor, app->document != NULL);
    gtk_widget_set_sensitive(app->scan_button, TRUE);
    gtk_widget_set_sensitive(app->metadata_button, project != NULL || app->project != NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(app->target_mode), TRUE);
    if (!project) {
        gtk_widget_set_sensitive(GTK_WIDGET(app->configuration_picker), app->project && app->project->configurations->len > 1);
        gtk_widget_set_sensitive(GTK_WIDGET(app->target_picker), app->project && app->project->targets->len > 0);
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) show_error(app, "Не удалось прочитать проект", error->message);
        else status(app, "Анализ отменён");
        update_document_title(app);
        update_build_controls(app);
        if (app->queued_scan) { g_autofree char *queued = g_steal_pointer(&app->queued_scan); scan_project(app, queued, app->queued_replace); }
        g_application_release(G_APPLICATION(app->application));
        return;
    }
    g_autofree char *previous_target = !request->replace && app->selected_target >= 0 && app->project &&
        (guint)app->selected_target < app->project->targets->len ?
        g_strdup(((ProjectTarget *)g_ptr_array_index(app->project->targets, (guint)app->selected_target))->name) : NULL;
    graph_set_project(app->graph, NULL, -1);
    project_free(app->project);
    app->project = project;
    app->selected_target = -1;
    if (request->replace) {
        g_clear_pointer(&app->git_changed, g_hash_table_unref); graph_set_changed(app->graph, NULL);
        g_clear_pointer(&app->build_directory, g_free);
        g_clear_pointer(&app->configuration, g_free);
        app->build_directory = g_strdup(request->build_directory);
        app->configuration = g_strdup(request->configuration);
        clear_tabs(app);
        document_free(app->document);
        app->document = NULL;
        g_clear_pointer(&app->selected, g_free);
        gtk_text_buffer_set_enable_undo(app->buffer, FALSE);
        gtk_text_buffer_set_text(app->buffer, "", -1);
        gtk_text_buffer_set_enable_undo(app->buffer, TRUE);
        gtk_text_buffer_set_modified(app->buffer, FALSE);
        gtk_text_view_set_editable(app->editor, FALSE);
        gtk_editable_set_text(GTK_EDITABLE(app->search), "");
    }
    g_autofree char *name = g_path_get_basename(project->root);
    g_autofree char *summary = project_summary(project);
    gtk_label_set_text(app->project_name, name);
    gtk_widget_set_tooltip_text(GTK_WIDGET(app->project_name), project->root);
    gtk_label_set_text(app->summary, summary);
    const char *compiler = app->project->compilers->len ?
        ((ProjectCompiler *)g_ptr_array_index(app->project->compilers, 0))->id : NULL;
    g_autofree char *chain = project->generator ? g_strdup_printf("%s → %s\n%s", project->generator,
        compiler ? compiler : "компилятор неизвестен", project->configuration && *project->configuration ? project->configuration : "без конфигурации") :
        g_strdup("Сборка ещё не определена");
    gtk_label_set_text(app->build_chain, chain);
    gtk_label_set_text(app->build_name, project->build_directory ? project->build_directory : "Метаданные не найдены");
    gtk_widget_set_tooltip_text(GTK_WIDGET(app->build_name), project->build_directory);
    app->updating_metadata = TRUE;
    GtkStringList *configs = gtk_string_list_new(NULL);
    guint chosen = GTK_INVALID_LIST_POSITION;
    for (guint i = 0; i < project->configurations->len; i++) {
        const char *name = g_ptr_array_index(project->configurations, i);
        gtk_string_list_append(configs, *name ? name : "По умолчанию");
        if (g_strcmp0(name, project->configuration) == 0) chosen = i;
    }
    if (!project->configurations->len) {
        gtk_string_list_append(configs, "Без конфигурации");
        chosen = 0;
    }
    gtk_drop_down_set_model(app->configuration_picker, G_LIST_MODEL(configs));
    gtk_drop_down_set_selected(app->configuration_picker, chosen);
    gtk_widget_set_sensitive(GTK_WIDGET(app->configuration_picker), project->configurations->len > 1);
    g_object_unref(configs);
    GtkStringList *targets = gtk_string_list_new(NULL);
    gtk_string_list_append(targets, "Все цели");
    for (guint i = 0; i < project->targets->len; i++) {
        ProjectTarget *target = g_ptr_array_index(project->targets, i);
        gtk_string_list_append(targets, target->name);
    }
    gtk_drop_down_set_model(app->target_picker, G_LIST_MODEL(targets));
    gtk_drop_down_set_selected(app->target_picker, 0);
    gtk_widget_set_sensitive(GTK_WIDGET(app->target_picker), project->targets->len > 0);
    g_object_unref(targets);
    populate_tree(app);
    gint selected = app->selected ? project_find_file(project, app->selected) : -1;
    graph_set_project(app->graph, project, selected);
    if (request->replace) {
        guint sources = 0;
        for (guint i = 0; i < project->files->len; i++) sources += ((ProjectFile *)g_ptr_array_index(project->files, i))->source;
        gtk_toggle_button_set_active(app->groups, sources > 30 || project->targets->len > 30);
    }
    SessionState *restore = request->restore;
    if (restore) {
        if (restore->cmake) gtk_editable_set_text(GTK_EDITABLE(app->cmake_entry), restore->cmake);
        if (restore->file && project_find_file(project, restore->file) >= 0) open_file(app, restore->file);
        gtk_toggle_button_set_active(app->overview, restore->overview);
        gtk_toggle_button_set_active(app->groups, restore->groups);
        gtk_toggle_button_set_active(app->target_mode, restore->target_mode);
        graph_set_expanded(app->graph, (const char *const *)restore->expanded);
        gtk_window_set_default_size(app->window, restore->width, restore->height);
        gtk_paned_set_position(app->outer, restore->sidebar); gtk_paned_set_position(app->workspace, restore->editor);
        gtk_toggle_button_set_active(app->log_toggle, restore->log_visible); gtk_paned_set_position(app->main_pane, restore->log);
        gtk_stack_set_visible_child_name(app->sidebar_pages, g_strcmp0(restore->page, "build") == 0 ? "build" : "files");
    }
    const char *target_name = restore ? restore->target : previous_target;
    if (target_name) for (guint i = 0; i < project->targets->len; i++) {
        ProjectTarget *target = g_ptr_array_index(project->targets, i);
        if (g_str_equal(target->name, target_name)) {
            app->selected_target = (gint)i; graph_select_target(app->graph, (gint)i);
            gtk_drop_down_set_selected(app->target_picker, i + 1); break;
        }
    }
    app->updating_metadata = FALSE;
    if (restore) {
        app->restore = restore; request->restore = NULL; app->restore_frames = 0;
        app->restore_tick = gtk_widget_add_tick_callback(GTK_WIDGET(app->window), restore_view, app, NULL);
    }
    language_ui_context(app->language, project->root, project->build_directory, app->selected);
    update_details(app);
    update_document_title(app);
    status(app, "Карта обновлена по файлам на диске");
    refresh_recent(app);
    setup_monitors(app);
    if (!app->smoke) { g_autofree char *card = workspace_catalog(NULL, project, app->selected, NULL); }
    if (app->queued_scan) { g_autofree char *queued = g_steal_pointer(&app->queued_scan); scan_project(app, queued, app->queued_replace); }
    g_application_release(G_APPLICATION(app->application));
}

static void scan_project(App *app, const char *path, gboolean replace)
{
    if (app->closing) return;
    if (building(app)) { if (!replace) app->deferred_rescan = TRUE; return; }
    if (app->busy) { g_free(app->queued_scan); app->queued_scan = g_strdup(path); app->queued_replace = replace; if (app->scan_cancel) g_cancellable_cancel(app->scan_cancel); return; }
    if (replace) {
        save_session(app);
        if (app->restore_tick) { gtk_widget_remove_tick_callback(GTK_WIDGET(app->window), app->restore_tick); app->restore_tick = 0; }
        session_state_free(app->restore); app->restore = NULL;
    }
    app->busy = TRUE;
    /* Keep selection/focus while reading; disable editing rather than the widget. */
    gtk_text_view_set_editable(app->editor, FALSE);
    gtk_widget_set_sensitive(app->scan_button, FALSE);
    gtk_widget_set_sensitive(app->metadata_button, FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->configuration_picker), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->target_picker), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->target_mode), FALSE);
    update_document_title(app);
    status(app, "Чтение проекта…");
    update_build_controls(app);
    ScanRequest *request = g_new0(ScanRequest, 1);
    request->path = g_strdup(path);
    request->replace = replace;
    if (!replace || !app->project) {
        request->build_directory = g_strdup(app->build_directory);
        request->configuration = g_strdup(app->configuration);
    }
    if (replace) {
        request->restore = session_load(app->sessions, path);
        if (request->restore) {
            if (!request->build_directory) request->build_directory = g_strdup(request->restore->build_directory);
            if (!request->configuration) request->configuration = g_strdup(request->restore->configuration);
        }
    }
    request->previous = replace ? NULL : app->project;
    app->scan_cancel = g_cancellable_new();
    GTask *task = g_task_new(NULL, app->scan_cancel, scan_done, app);
    g_task_set_task_data(task, request, scan_request_free);
    g_application_hold(G_APPLICATION(app->application));
    g_task_run_in_thread(task, scan_worker);
    g_object_unref(task);
}

static gboolean save_document(App *app, gboolean rescan)
{
    if (!app->document || app->busy) return FALSE;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(app->buffer, &start, &end);
    g_autofree char *text = gtk_text_buffer_get_text(app->buffer, &start, &end, FALSE);
    g_autoptr(GError) error = NULL;
    if (!document_save(app->document, text, &error)) {
        show_error(app, "Файл не сохранён", error->message);
        return FALSE;
    }
    recovery_remove(app->recovery_dir, app->document->path);
    gtk_text_buffer_set_modified(app->buffer, FALSE);
    update_document_title(app);
    status(app, "Файл сохранён");
    if (building(app)) app->deferred_rescan = TRUE;
    if (rescan && app->project) scan_project(app, app->project->root, FALSE);
    return TRUE;
}

static void perform_action(App *app, Action action, const char *path, guint line)
{
    if (action == ACTION_FILE) open_file(app, path);
    else if (action == ACTION_LOCATION) {
        if (g_strcmp0(app->selected, path) != 0 && !open_file(app, path)) return;
        if (!editor_go_to_line(app->editor, line)) {
            status(app, "Строка больше не существует · обновите карту проекта");
            return;
        }
        g_autofree char *message = g_strdup_printf("Источник зависимости: %s:%u", path, line);
        status(app, message);
    }
    else if (action == ACTION_PROJECT) scan_project(app, path, TRUE);
    else if (action == ACTION_BUILD) start_build(app);
    else if (action == ACTION_TAB_CLOSE && app->current_tab) {
        EditorTab *tab = app->current_tab; app->current_tab = NULL; gtk_box_remove(app->tab_bar, tab->button); g_ptr_array_remove(app->tabs, tab);
        document_free(app->document); app->document = NULL; g_clear_pointer(&app->selected, g_free);
        GtkTextBuffer *empty = editor_buffer_new(); use_buffer(app, empty); g_object_unref(empty); gtk_text_view_set_editable(app->editor, FALSE); update_document_title(app);
        language_ui_context(app->language, app->project->root, app->project->build_directory, NULL);
    }
    else if (action == ACTION_RELOAD && app->selected) {
        if (app->current_tab) { EditorTab *tab = app->current_tab; app->current_tab = NULL; gtk_box_remove(app->tab_bar, tab->button); g_ptr_array_remove(app->tabs, tab); }
        g_autofree char *selected = g_strdup(app->selected);
        open_file(app, selected);
        if (app->project) scan_project(app, app->project->root, FALSE);
    } else if (action == ACTION_CLOSE) {
        save_session(app);
        app->closing = TRUE;
        tools_ui_free(app->tools); app->tools = NULL;
        language_ui_free(app->language); app->language = NULL;
        gtk_window_destroy(app->window);
    }
}

static void confirm_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    App *app = user_data;
    g_autoptr(GError) error = NULL;
    gint choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
    app->confirming = FALSE;
    update_build_controls(app);
    Action action = app->pending;
    guint line = app->pending_line;
    g_autofree char *path = g_steal_pointer(&app->pending_path);
    app->pending = ACTION_NONE;
    app->pending_line = 0;
    if (choice == 2 && !save_document(app, FALSE)) return;
    if (choice != 1 && choice != 2) return;
    if (choice == 1 && action == ACTION_BUILD) { perform_action(app, action, path, line); return; }
    if (choice == 1 && app->document) { recovery_remove(app->recovery_dir, app->document->path); gtk_text_buffer_set_modified(app->buffer, FALSE); }
    if (action == ACTION_CLOSE || action == ACTION_PROJECT || action == ACTION_BUILD) { request_action_at(app, action, path, line); return; }
    perform_action(app, action, path, line);
    if (choice == 2 && (action == ACTION_FILE || action == ACTION_LOCATION) && app->project)
        scan_project(app, app->project->root, FALSE);
}

static void request_action_at(App *app, Action action, const char *path, guint line)
{
    if (app->busy || app->confirming || app->closing) return;
    if (tools_ui_busy(app->tools) && (action == ACTION_PROJECT || action == ACTION_CLOSE || action == ACTION_BUILD)) { status(app, "Сначала остановите задачу в инструментах"); return; }
    if (building(app) && (action == ACTION_PROJECT || action == ACTION_CLOSE || action == ACTION_BUILD)) {
        status(app, "Сначала остановите текущую сборку"); return;
    }
    if ((action == ACTION_FILE || action == ACTION_LOCATION) && app->current_tab && g_strcmp0(app->selected, path)) { perform_action(app, action, path, line); return; }
    if (action == ACTION_CLOSE || action == ACTION_PROJECT || action == ACTION_BUILD) {
        if (!gtk_text_buffer_get_modified(app->buffer)) for (guint i = 0; i < app->tabs->len; i++) {
            EditorTab *tab = g_ptr_array_index(app->tabs, i);
            if (tab != app->current_tab && gtk_text_buffer_get_modified(tab->buffer)) { open_file(app, tab->path); break; }
        }
    }
    if (action == ACTION_LOCATION && g_strcmp0(app->selected, path) == 0 &&
        gtk_text_buffer_get_modified(app->buffer)) {
        status(app, "Связь рассчитана по сохранённому файлу · сохраните правки для точного перехода");
        return;
    }
    if (!gtk_text_buffer_get_modified(app->buffer)) {
        perform_action(app, action, path, line);
        return;
    }
    app->pending = action;
    g_free(app->pending_path);
    app->pending_path = g_strdup(path);
    app->pending_line = line;
    app->confirming = TRUE;
    update_build_controls(app);
    GtkAlertDialog *dialog = gtk_alert_dialog_new("Сохранить изменения?");
    gtk_alert_dialog_set_detail(dialog, app->selected);
    const char *buttons[] = {"Отмена", "Не сохранять", "Сохранить", NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 2);
    gtk_alert_dialog_choose(dialog, app->window, NULL, confirm_done, app);
    g_object_unref(dialog);
}

static void request_action(App *app, Action action, const char *path)
{
    request_action_at(app, action, path, 0);
}

static gboolean details_link(GtkLabel *label, const char *uri, gpointer data)
{
    (void)label;
    App *app = data;
    if (!app->project) return TRUE;
    const char *colon = strchr(uri, ':');
    if (!colon) return TRUE;
    char *end;
    guint64 index = g_ascii_strtoull(colon + 1, &end, 10);
    if (end == colon + 1 || *end) return TRUE;
    const char *path = NULL;
    guint line = 0;
    if (g_str_has_prefix(uri, "include:") && index < app->project->edges->len) {
        ProjectEdge *edge = g_ptr_array_index(app->project->edges, (guint)index);
        ProjectFile *from = g_ptr_array_index(app->project->files, edge->from);
        path = from->path; line = edge->line;
    } else if (g_str_has_prefix(uri, "target:") && index < app->project->targets->len) {
        ProjectTarget *target = g_ptr_array_index(app->project->targets, (guint)index);
        path = target->definition; line = target->line;
    } else if (g_str_has_prefix(uri, "targetedge:") && index < app->project->target_edges->len) {
        ProjectTargetEdge *edge = g_ptr_array_index(app->project->target_edges, (guint)index);
        path = edge->definition; line = edge->line;
    }
    if (path) request_action_at(app, ACTION_LOCATION, path, line ? line : 1);
    else status(app, "Источник связи отсутствует в индексируемых файлах проекта");
    return TRUE; /* Never hand an internal link to the desktop URI launcher. */
}

static void folder_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    App *app = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
    if (folder && !app->closing && !building(app)) {
        g_autofree char *path = g_file_get_path(folder);
        if (path && g_object_get_data(source, "build-directory-picker")) {
            if (app->project && !app->busy) {
                g_free(app->build_directory); app->build_directory = g_strdup(path);
                g_clear_pointer(&app->configuration, g_free);
                scan_project(app, app->project->root, FALSE);
            }
        } else if (path) request_action(app, ACTION_PROJECT, path);
    }
    g_application_release(G_APPLICATION(app->application));
}

static void choose_directory(App *app, gboolean build)
{
    if (app->busy || app->confirming || building(app) || (build && !app->project)) return;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    g_object_set_data(G_OBJECT(dialog), "build-directory-picker", GINT_TO_POINTER(build));
    gtk_file_dialog_set_title(dialog, build ? "Каталог с compile_commands.json или CMake File API" : "Открыть папку проекта");
    if (app->project) {
        const char *path = build && app->project->build_directory ? app->project->build_directory : app->project->root;
        g_autoptr(GFile) folder = g_file_new_for_path(path);
        gtk_file_dialog_set_initial_folder(dialog, folder);
    }
    g_application_hold(G_APPLICATION(app->application));
    gtk_file_dialog_select_folder(dialog, app->window, NULL, folder_chosen, app);
    g_object_unref(dialog);
}

static void choose_folder(GtkButton *button, gpointer data)
{ (void)button; choose_directory(data, FALSE); }
static void choose_metadata(GtkButton *button, gpointer data)
{ (void)button; choose_directory(data, TRUE); }

static void configuration_changed(GObject *object, GParamSpec *spec, gpointer data)
{
    (void)object; (void)spec;
    App *app = data;
    if (app->updating_metadata || app->busy || building(app) || !app->project) return;
    guint selected = gtk_drop_down_get_selected(app->configuration_picker);
    if (selected >= app->project->configurations->len) return;
    const char *name = g_ptr_array_index(app->project->configurations, selected);
    if (g_strcmp0(name, app->project->configuration) == 0) return;
    if (!app->build_directory) app->build_directory = g_strdup(app->project->build_directory);
    g_free(app->configuration); app->configuration = g_strdup(name);
    scan_project(app, app->project->root, FALSE);
}

static void target_mode_toggled(GtkToggleButton *button, gpointer data)
{
    App *app = data;
    graph_set_target_mode(app->graph, gtk_toggle_button_get_active(button));
    update_details(app);
}

static void graph_target_selected(guint index, gpointer data)
{
    App *app = data;
    if (!app->project || app->busy || building(app) || index >= app->project->targets->len) return;
    app->selected_target = (gint)index;
    gtk_toggle_button_set_active(app->target_mode, TRUE);
    graph_select_target(app->graph, (gint)index);
    gtk_drop_down_set_selected(app->target_picker, index + 1);
    update_details(app);
}

static void target_changed(GObject *object, GParamSpec *spec, gpointer data)
{
    (void)object; (void)spec;
    App *app = data;
    if (app->updating_metadata || !app->project || app->busy || building(app)) return;
    guint index = gtk_drop_down_get_selected(app->target_picker);
    if (!index) {
        app->selected_target = -1;
        graph_select_target(app->graph, -1);
        update_details(app);
    } else if ((gint)index - 1 != app->selected_target) graph_target_selected(index - 1, app);
}

static void save_clicked(GtkButton *button, gpointer data)
{ (void)button; save_document(data, TRUE); }

static void reload_clicked(GtkButton *button, gpointer data)
{ (void)button; request_action(data, ACTION_RELOAD, NULL); }

static void scan_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    App *app = data;
    if (app->project) scan_project(app, app->project->root, FALSE);
}

static void overview_toggled(GtkToggleButton *button, gpointer data)
{
    App *app = data;
    graph_set_overview(app->graph, gtk_toggle_button_get_active(button));
}

static void groups_toggled(GtkToggleButton *button, gpointer data)
{ App *app = data; graph_set_grouped(app->graph, gtk_toggle_button_get_active(button)); }
static void graph_zoom_in(GtkButton *button, gpointer data)
{ (void)button; graph_zoom_by(((App *)data)->graph, 1.25); }
static void graph_zoom_out(GtkButton *button, gpointer data)
{ (void)button; graph_zoom_by(((App *)data)->graph, .8); }
static void graph_fit_clicked(GtkButton *button, gpointer data)
{ (void)button; graph_fit(((App *)data)->graph); }

static void recent_changed(GObject *object, GParamSpec *spec, gpointer data)
{
    (void)object; (void)spec; App *app = data;
    guint index = gtk_drop_down_get_selected(app->recent_picker);
    if (!index || app->updating_metadata || app->busy || building(app)) return;
    GtkStringObject *item = gtk_drop_down_get_selected_item(app->recent_picker);
    if (!item) return;
    g_autofree char *path = g_strdup(gtk_string_object_get_string(item));
    gtk_drop_down_set_selected(app->recent_picker, 0);
    request_action(app, ACTION_PROJECT, path);
}
static void refresh_recent(App *app)
{
    if (!app->recent_picker) return;
    g_auto(GStrv) roots = session_recent(app->sessions);
    GtkStringList *model = gtk_string_list_new(NULL); gtk_string_list_append(model, "Недавние проекты");
    for (guint i = 0; roots[i]; i++) gtk_string_list_append(model, roots[i]);
    gboolean updating = app->updating_metadata; app->updating_metadata = TRUE;
    gtk_drop_down_set_model(app->recent_picker, G_LIST_MODEL(model)); gtk_drop_down_set_selected(app->recent_picker, 0);
    app->updating_metadata = updating; g_object_unref(model);
}

static gboolean close_requested(GtkWindow *window, gpointer user_data)
{
    (void)window;
    App *app = user_data;
    if (app->confirming) return TRUE;
    if (building(app)) { status(app, "Остановите сборку перед закрытием окна"); gtk_toggle_button_set_active(app->log_toggle, TRUE); return TRUE; }
    if (tools_ui_busy(app->tools)) { tools_ui_stop(app->tools); status(app, "Завершается задача инструментов · закройте окно после её остановки"); return TRUE; }
    if (app->busy) { if (app->scan_cancel) g_cancellable_cancel(app->scan_cancel); status(app, "Отмена анализа…"); return TRUE; }
    gboolean dirty = gtk_text_buffer_get_modified(app->buffer);
    for (guint i = 0; i < app->tabs->len; i++) dirty |= gtk_text_buffer_get_modified(((EditorTab *)g_ptr_array_index(app->tabs, i))->buffer);
    if (dirty) {
        request_action(app, ACTION_CLOSE, NULL);
        return TRUE;
    }
    app->closing = TRUE;
    save_session(app);
    tools_ui_free(app->tools); app->tools = NULL;
    language_ui_free(app->language); app->language = NULL;
    return FALSE;
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType modifiers, gpointer data)
{
    (void)controller; (void)keycode;
    App *app = data;
    if (keyval == GDK_KEY_F12) { language_ui_action(app->language, "textDocument/definition"); return TRUE; }
    if (!(modifiers & GDK_CONTROL_MASK)) return FALSE;
    if (keyval == GDK_KEY_space) { language_ui_action(app->language, "textDocument/completion"); return TRUE; }
    if (keyval == GDK_KEY_p || keyval == GDK_KEY_P) {
        gtk_stack_set_visible_child_name(app->sidebar_pages, "files");
        gtk_widget_grab_focus(GTK_WIDGET(app->search));
        gtk_editable_select_region(GTK_EDITABLE(app->search), 0, -1);
        return TRUE;
    }
    if (keyval == GDK_KEY_s || keyval == GDK_KEY_S) {
        save_document(app, TRUE);
        return TRUE;
    }
    if (keyval == GDK_KEY_o || keyval == GDK_KEY_O) {
        choose_folder(NULL, app);
        return TRUE;
    }
    if (keyval == GDK_KEY_r || keyval == GDK_KEY_R) {
        scan_clicked(NULL, app);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *label_new(const char *value, const char *css_class)
{
    GtkWidget *label = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    if (css_class) gtk_widget_add_css_class(label, css_class);
    return label;
}

static GtkWidget *button_new(const char *label, GCallback callback, App *app)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    g_signal_connect(button, "clicked", callback, app);
    return button;
}

static void target_item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void)factory; (void)data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 30);
    gtk_list_item_set_child(item, label);
}

static void target_item_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void)factory; (void)data;
    const char *name = gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(item)));
    GtkWidget *label = gtk_list_item_get_child(item);
    gtk_label_set_text(GTK_LABEL(label), name);
    gtk_widget_set_tooltip_text(label, name);
}

static GtkWidget *scroll(GtkWidget *child)
{
    GtkWidget *scrollbar = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrollbar), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrollbar), child);
    gtk_widget_set_vexpand(scrollbar, TRUE);
    return scrollbar;
}

static gboolean take_screenshot(App *app)
{
    GdkPaintable *paintable = gtk_widget_paintable_new(GTK_WIDGET(app->window));
    GtkSnapshot *snapshot = gtk_snapshot_new();
    int width = gtk_widget_get_width(GTK_WIDGET(app->window));
    int height = gtk_widget_get_height(GTK_WIDGET(app->window));
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    g_object_unref(paintable);
    if (!node) return FALSE;
    GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(app->window));
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
    GdkTexture *texture = gsk_renderer_render_texture(renderer, node, &bounds);
    const char *screenshot = g_getenv("STARTAPP_SMOKE_SCREENSHOT");
    if (!screenshot) screenshot = "build/smoke-ui.png";
    g_autofree char *parent = g_path_get_dirname(screenshot);
    gboolean saved = g_mkdir_with_parents(parent, 0700) == 0 &&
                     gdk_texture_save_to_png(texture, screenshot);
    g_object_unref(texture);
    gsk_render_node_unref(node);
    return saved;
}

static GtkWidget *find_button(GtkWidget *widget, const char *label)
{
    if (GTK_IS_BUTTON(widget) &&
        g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0) return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_button(child, label);
        if (found) return found;
    }
    return NULL;
}

static gboolean smoke_answer_dialog(App *app, const char *label)
{
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
        g_autoptr(GtkWindow) window = g_list_model_get_item(windows, i);
        if (window == app->window) continue;
        GtkWidget *button = find_button(GTK_WIDGET(window), label);
        if (button) {
            g_signal_emit_by_name(button, "clicked");
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean smoke_write_metadata(App *app)
{
    const char *names[] = {"CMakeLists.txt", "build/compile_commands.json",
        "build/.cmake/api/v1/reply/index-2026.json", "build/.cmake/api/v1/reply/model.json",
        "build/.cmake/api/v1/reply/app-debug.json", "build/.cmake/api/v1/reply/app-release.json",
        "build/.cmake/api/v1/reply/core.json", "build/.cmake/api/v1/reply/toolchains.json", "CMakePresets.json"};
    const char *contents[] = {
        "project(smoke C)\nadd_library(core STATIC main.c)\nadd_executable(app main.c)\ntarget_link_libraries(app PRIVATE core)\n",
        "[{\"directory\":\"..\",\"file\":\"main.c\",\"arguments\":[\"cc\",\"-I.\"]}]",
        "{\"cmake\":{\"version\":{\"string\":\"3.31.6\"},\"generator\":{\"name\":\"Ninja Multi-Config\",\"multiConfig\":true}},"
        "\"objects\":[{\"kind\":\"codemodel\",\"version\":{\"major\":2},\"jsonFile\":\"model.json\"},"
        "{\"kind\":\"toolchains\",\"version\":{\"major\":1},\"jsonFile\":\"toolchains.json\"}]}",
        "{\"paths\":{\"source\":\"..\"},\"configurations\":["
        "{\"name\":\"Debug\",\"targets\":[{\"jsonFile\":\"app-debug.json\"},{\"jsonFile\":\"core.json\"}]},"
        "{\"name\":\"Release\",\"targets\":[{\"jsonFile\":\"app-release.json\"},{\"jsonFile\":\"core.json\"}]}]}",
        "{\"id\":\"app\",\"name\":\"app\",\"type\":\"EXECUTABLE\",\"backtrace\":0,"
        "\"artifacts\":[{\"path\":\"Debug/app.exe\"}],"
        "\"backtraceGraph\":{\"files\":[\"CMakeLists.txt\"],\"nodes\":[{\"file\":0,\"line\":3},{\"file\":0,\"line\":4}]},"
        "\"dependencies\":[{\"id\":\"core\",\"backtrace\":1}]}",
        "{\"id\":\"app\",\"name\":\"app\",\"type\":\"EXECUTABLE\",\"backtrace\":0,"
        "\"artifacts\":[{\"path\":\"Release/app.exe\"}],"
        "\"backtraceGraph\":{\"files\":[\"CMakeLists.txt\"],\"nodes\":[{\"file\":0,\"line\":3},{\"file\":0,\"line\":4}]},"
        "\"dependencies\":[{\"id\":\"core\",\"backtrace\":1}]}",
        "{\"id\":\"core\",\"name\":\"core\",\"type\":\"STATIC_LIBRARY\"}",
        "{\"toolchains\":[{\"language\":\"C\",\"compiler\":{\"id\":\"GNU\",\"version\":\"14.2\",\"path\":\"gcc\"}}]}",
        "{\"version\":3,\"configurePresets\":[{\"name\":\"debug\",\"generator\":\"Ninja\",\"binaryDir\":\"${sourceDir}/build\","
        "\"cacheVariables\":{\"CMAKE_BUILD_TYPE\":\"Debug\"}}]}"
    };
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autofree char *path = g_build_filename(app->smoke_root, names[i], NULL);
        g_autofree char *parent = g_path_get_dirname(path);
        if (g_mkdir_with_parents(parent, 0700) || !g_file_set_contents(path, contents[i], -1, NULL)) return FALSE;
    }
    return TRUE;
}

static void smoke_remove_tree(const char *path)
{
    if (!g_file_test(path, G_FILE_TEST_IS_SYMLINK) && g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_autoptr(GDir) directory = g_dir_open(path, 0, NULL);
        const char *name;
        while (directory && (name = g_dir_read_name(directory))) {
            g_autofree char *child = g_build_filename(path, name, NULL);
            smoke_remove_tree(child);
        }
        g_clear_pointer(&directory, g_dir_close);
        g_rmdir(path);
    } else g_unlink(path);
}

static gboolean smoke_editor_checks(App *app)
{
    GtkSourceStyleScheme *scheme = gtk_source_buffer_get_style_scheme(GTK_SOURCE_BUFFER(app->buffer));
    if (!scheme || g_strcmp0(gtk_source_style_scheme_get_id(scheme), "startapp-dark")) return FALSE;
    GtkSourceLanguageManager *manager = gtk_source_language_manager_get_default();
    const char *ids[] = {"c", "cpp", "cmake", "makefile", "json"};
    for (guint i = 0; i < G_N_ELEMENTS(ids); i++)
        if (!gtk_source_language_manager_get_language(manager, ids[i])) return FALSE;
    g_autoptr(GtkSourceBuffer) buffer = gtk_source_buffer_new_with_language(gtk_source_language_manager_get_language(manager, "c"));
    const char *text = "/* return\nint fake; */\nconst char *s = \"/* not a comment */\";\n";
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(buffer), text, -1);
    GtkTextIter start, end, iter;
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(buffer), &start, &end); gtk_source_buffer_ensure_highlight(buffer, &start, &end);
    gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(buffer), &iter, 1);
    if (!gtk_source_buffer_iter_has_context_class(buffer, &iter, "comment")) return FALSE;
    gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(buffer), &iter, (gint)(strstr(text, "not a comment") - text));
    if (!gtk_source_buffer_iter_has_context_class(buffer, &iter, "string") ||
        gtk_source_buffer_iter_has_context_class(buffer, &iter, "comment")) return FALSE;
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(buffer), "/* open comment\nint fake;\n", -1);
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(buffer), &start, &end); gtk_source_buffer_ensure_highlight(buffer, &start, &end);
    gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(buffer), &iter, 1);
    return gtk_source_buffer_iter_has_context_class(buffer, &iter, "comment");
}

static gboolean smoke_tick(gpointer data)
{
    App *app = data;
    g_autofree char *saved_path = NULL;
    g_autofree char *saved_text = NULL;
    if (app->busy) return G_SOURCE_CONTINUE;
    if (!app->project) {
        g_printerr("UI smoke: project did not load\n");
        app->smoke_result = 1;
        gtk_window_destroy(app->window);
        return G_SOURCE_REMOVE;
    }
    if (app->smoke_stage == 0) {
        request_action(app, ACTION_FILE, "main.c");
        if (!app->document || !smoke_editor_checks(app)) goto failed;
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 1) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(app->buffer, &end);
        gtk_text_buffer_insert(app->buffer, &end, "\n#include \"extra.h\"\n", -1);
        if (!gtk_text_buffer_get_modified(app->buffer)) goto failed;
        request_action(app, ACTION_FILE, "api.h");
        if (!app->confirming || g_strcmp0(app->selected, "main.c") != 0) goto failed;
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 2) {
        if (!smoke_answer_dialog(app, "Отмена")) goto failed;
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 3) {
        if (app->confirming) return G_SOURCE_CONTINUE;
        if (g_strcmp0(app->selected, "main.c") != 0 ||
            !gtk_text_buffer_get_modified(app->buffer) || !save_document(app, TRUE)) goto failed;
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 4) {
        if (app->project->edges->len != 2 || gtk_text_buffer_get_modified(app->buffer)) goto failed;
        g_autofree char *path = g_build_filename(app->smoke_root, "main.c", NULL);
        g_autofree char *text = NULL;
        if (!g_file_get_contents(path, &text, NULL, NULL) || !strstr(text, "extra.h")) goto failed;
        if (!take_screenshot(app)) goto failed;
        /* Use the rendered node: native window frames and screen sizes affect layout. */
        double x, y;
        gint header = project_find_file(app->project, "api.h");
        if (header < 0 || !graph_node_center(app->graph, (guint)header, &x, &y) ||
            !graph_activate_at(app->graph, x, y)) goto failed;
        if (!app->selected || g_str_equal(app->selected, "main.c")) goto failed;
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 5) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(app->buffer, &end);
        gtk_text_buffer_insert(app->buffer, &end, "// saved through confirmation\n", -1);
        if (!gtk_text_buffer_get_can_undo(app->buffer)) goto failed;
        gtk_text_buffer_undo(app->buffer);
        if (!gtk_text_buffer_get_can_redo(app->buffer)) goto failed;
        gtk_text_buffer_redo(app->buffer);
        request_action(app, ACTION_FILE, "main.c");
        if (!app->confirming) goto failed;
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 6) {
        if (!smoke_answer_dialog(app, "Сохранить")) goto failed;
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->confirming && app->smoke_stage != 10 && app->smoke_stage != 11 && app->smoke_stage != 19 && app->smoke_stage != 26)
        return G_SOURCE_CONTINUE;
    if (app->smoke_stage == 7) {
        if (g_strcmp0(app->selected, "main.c") != 0) goto failed;
        saved_path = g_build_filename(app->smoke_root, "api.h", NULL);
        if (!g_file_get_contents(saved_path, &saved_text, NULL, NULL) ||
            !strstr(saved_text, "saved through confirmation")) goto failed;
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(app->buffer, &end);
        gtk_text_buffer_insert(app->buffer, &end, "// search preserves edits\n", -1);
        gtk_editable_set_text(GTK_EDITABLE(app->search), "does-not-exist");
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 8) {
        if (g_list_model_get_n_items(G_LIST_MODEL(app->selection)) != 0 ||
            !gtk_text_buffer_get_modified(app->buffer) ||
            g_strcmp0(app->selected, "main.c") != 0) goto failed;
        gtk_editable_set_text(GTK_EDITABLE(app->search), "API.H");
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 9) {
        if (g_list_model_get_n_items(G_LIST_MODEL(app->selection)) != 1) goto failed;
        g_signal_emit_by_name(app->search, "activate");
        if (!app->confirming || g_strcmp0(app->selected, "main.c") != 0) goto failed;
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 10) {
        if (!smoke_answer_dialog(app, "Сохранить")) goto failed;
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 11) {
        if (app->smoke_link_pending) {
            if (!smoke_answer_dialog(app, "Сохранить")) goto failed;
            app->smoke_stage++;
            return G_SOURCE_CONTINUE;
        }
        if (g_strcmp0(app->selected, "api.h") != 0 ||
            g_list_model_get_n_items(G_LIST_MODEL(app->selection)) != 1 ||
            !strstr(gtk_label_get_label(app->details), "include:0")) goto failed;
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(app->buffer, &end);
        gtk_text_buffer_insert(app->buffer, &end, "// saved before source navigation\n", -1);
        gboolean handled = FALSE;
        g_signal_emit_by_name(app->details, "activate-link", "include:0", &handled);
        if (!handled || !app->confirming || app->pending_line != 101 ||
            g_strcmp0(app->selected, "api.h") != 0) goto failed;
        app->smoke_link_pending = TRUE;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 12) {
        GtkTextIter cursor, end;
        gtk_text_buffer_get_iter_at_mark(app->buffer, &cursor, gtk_text_buffer_get_insert(app->buffer));
        if (gtk_text_iter_get_line(&cursor) != 100 ||
            !gtk_text_buffer_get_selection_bounds(app->buffer, &cursor, &end)) goto failed;
        saved_text = gtk_text_buffer_get_text(app->buffer, &cursor, &end, FALSE);
        if (!g_str_equal(saved_text, "#include \"api.h\"")) goto failed;
        guint first, last;
        if (!editor_gutter_visible_lines(app->editor, &first, &last) ||
            first <= 1 || first > 101 || last < 101) goto failed;
        if (!key_pressed(NULL, GDK_KEY_p, 0, GDK_CONTROL_MASK, app)) goto failed;
        GtkWidget *focus = gtk_window_get_focus(app->window);
        if (!focus || (focus != GTK_WIDGET(app->search) &&
            !gtk_widget_is_ancestor(focus, GTK_WIDGET(app->search)))) goto failed;
        gtk_editable_set_text(GTK_EDITABLE(app->search), "модуль");
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 13) {
        if (g_list_model_get_n_items(G_LIST_MODEL(app->selection)) != 2) goto failed;
        g_signal_emit_by_name(app->search, "activate");
        if (g_strcmp0(app->selected, "src/МОДУЛЬ.c") != 0) goto failed;
        g_signal_emit_by_name(app->search, "stop-search");
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 14) {
        if (*gtk_editable_get_text(GTK_EDITABLE(app->search)) ||
            g_list_model_get_n_items(G_LIST_MODEL(app->selection)) != 6) goto failed;
        request_action_at(app, ACTION_LOCATION, "main.c", 101);
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 15) {
        if (!take_screenshot(app)) goto failed;
        GtkTextIter start, cursor;
        gtk_text_buffer_get_start_iter(app->buffer, &start);
        gtk_text_buffer_insert(app->buffer, &start, "\n", -1);
        gtk_text_buffer_get_iter_at_mark(app->buffer, &cursor, gtk_text_buffer_get_insert(app->buffer));
        int offset = gtk_text_iter_get_offset(&cursor);
        gboolean handled = FALSE;
        g_signal_emit_by_name(app->details, "activate-link", "include:0", &handled);
        gtk_text_buffer_get_iter_at_mark(app->buffer, &cursor, gtk_text_buffer_get_insert(app->buffer));
        if (!handled || !gtk_text_buffer_get_modified(app->buffer) ||
            gtk_text_iter_get_offset(&cursor) != offset) goto failed;
        gtk_text_buffer_undo(app->buffer);
        if (editor_go_to_line(app->editor, 0) || editor_go_to_line(app->editor, 99999)) goto failed;
        if (!editor_go_to_line(app->editor, (guint)gtk_text_buffer_get_line_count(app->buffer))) goto failed;
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 16) {
        guint first, last;
        if (!editor_gutter_visible_lines(app->editor, &first, &last) ||
            last != (guint)gtk_text_buffer_get_line_count(app->buffer) || !smoke_write_metadata(app)) goto failed;
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(app->buffer, &end);
        gtk_text_buffer_insert(app->buffer, &end, "// metadata preserves edits\n", -1);
        app->build_directory = g_build_filename(app->smoke_root, "build", NULL);
        app->configuration = g_strdup("Debug");
        scan_project(app, app->project->root, FALSE);
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 17) {
        if (app->project->targets->len != 2 || app->project->target_edges->len != 1 ||
            app->project->compile_units != 1 || !gtk_text_buffer_get_modified(app->buffer) ||
            g_strcmp0(app->selected, "main.c") != 0) goto failed;
        gtk_toggle_button_set_active(app->target_mode, TRUE);
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 18) {
        double x, y;
        if (!graph_node_center(app->graph, 0, &x, &y) || !graph_activate_at(app->graph, x, y) || app->selected_target != 0 ||
            gtk_drop_down_get_selected(app->target_picker) != 1 ||
            !gtk_text_buffer_get_modified(app->buffer)) goto failed;
        gboolean handled = FALSE;
        g_signal_emit_by_name(app->details, "activate-link", "targetedge:0", &handled);
        if (!handled || !app->confirming || app->pending_line != 4) goto failed;
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 19) {
        if (!smoke_answer_dialog(app, "Сохранить")) goto failed;
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 20) {
        GtkSourceLanguage *language = gtk_source_buffer_get_language(GTK_SOURCE_BUFFER(app->buffer));
        if (!language || g_strcmp0(gtk_source_language_get_id(language), "cmake")) goto failed;
        GtkTextIter cursor;
        gtk_text_buffer_get_iter_at_mark(app->buffer, &cursor, gtk_text_buffer_get_insert(app->buffer));
        if (g_strcmp0(app->selected, "CMakeLists.txt") || gtk_text_iter_get_line(&cursor) != 3) goto failed;
        gtk_drop_down_set_selected(app->configuration_picker, 1);
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 21) {
        if (g_strcmp0(app->project->configuration, "Release") || app->project->targets->len != 2 ||
            app->project->target_edges->len != 1 || g_strcmp0(app->selected, "CMakeLists.txt")) goto failed;
        ProjectTarget *target = g_ptr_array_index(app->project->targets, 0);
        if (!target->artifact || !strstr(target->artifact, "Release") ||
            !strstr(gtk_label_get_text(app->build_chain), "Ninja Multi-Config") ||
            !strstr(gtk_label_get_text(app->build_profile), "14.2") || app->project->presets->len != 1) goto failed;
        gtk_drop_down_set_selected(app->target_picker, 1);
        if (!gtk_window_get_decorated(app->window) || gtk_window_get_titlebar(app->window)) goto failed;
        gtk_window_set_default_size(app->window, 1080, 720);
        app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 22) {
        double x, y, after_x, after_y;
        if (!graph_node_center(app->graph, 0, &x, &y)) goto failed;
        double previous = graph_get_zoom(app->graph);
        graph_zoom_at(app->graph, 1.25, x, y);
        if (graph_get_zoom(app->graph) <= previous || !graph_node_center(app->graph, 0, &after_x, &after_y) ||
            fabs(after_x - x) > .01 || fabs(after_y - y) > .01) goto failed;
        graph_pan(app->graph, 12, 15);
        if (!graph_node_center(app->graph, 0, &after_x, &after_y) || fabs(after_x - x - 12) > .01 ||
            fabs(after_y - y - 15) > .01 || !graph_activate_at(app->graph, after_x, after_y)) goto failed;
        gtk_toggle_button_set_active(app->overview, TRUE);
        gtk_toggle_button_set_active(app->groups, TRUE);
        gtk_stack_set_visible_child_name(app->sidebar_pages, "build");
        app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 23) {
        if (graph_visible_count(app->graph) != 2) goto failed;
        /* Switch to files: the root group includes main.c/api.h/extra.h. */
        gtk_toggle_button_set_active(app->target_mode, FALSE);
        if (graph_visible_count(app->graph) != 2) goto failed;
        graph_fit(app->graph);
        app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 24) {
        /* Expand the single root group through the actual click controller. */
        guint before = graph_visible_count(app->graph);
        gboolean expanded = FALSE;
        int w = gtk_widget_get_width(graph_widget(app->graph)), h = gtk_widget_get_height(graph_widget(app->graph));
        for (int y = 64; y < h && !expanded; y += 8) for (int x = 8; x < w && !expanded; x += 8)
            if (graph_activate_at(app->graph, x, y)) expanded = TRUE;
        if (!expanded || graph_visible_count(app->graph) <= before) goto failed;
        gtk_toggle_button_set_active(app->groups, FALSE);
        gtk_toggle_button_set_active(app->overview, FALSE);
        gtk_toggle_button_set_active(app->target_mode, TRUE);
        gtk_drop_down_set_selected(app->target_picker, 1);
        graph_fit(app->graph);
        app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 25) {
        GtkTextIter end; gtk_text_buffer_get_end_iter(app->buffer, &end);
        gtk_text_buffer_insert(app->buffer, &end, "# saved before build\n", -1);
        app->smoke_build_mode = "failure";
        g_signal_emit_by_name(app->build_button, "clicked");
        if (!app->confirming || building(app)) goto failed;
        app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 26) {
        if (!smoke_answer_dialog(app, "Сохранить")) goto failed;
        app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 27) {
        if (building(app)) return G_SOURCE_CONTINUE;
        if (build_panel_exit_code(app->build_panel) != 2 || build_panel_diagnostic_count(app->build_panel) != 1 ||
            gtk_text_buffer_get_modified(app->buffer) || !build_panel_open_diagnostic(app->build_panel, 0)) goto failed;
        app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 28) {
        GtkTextIter cursor; gtk_text_buffer_get_iter_at_mark(app->buffer, &cursor, gtk_text_buffer_get_insert(app->buffer));
        if (g_strcmp0(app->selected, "main.c") || gtk_text_iter_get_line(&cursor) != 100) goto failed;
        app->smoke_build_mode = "success"; g_signal_emit_by_name(app->build_button, "clicked");
        app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 29) {
        if (building(app)) return G_SOURCE_CONTINUE;
        if (build_panel_exit_code(app->build_panel) || build_panel_diagnostic_count(app->build_panel) || app->selected_target != 0 ||
            g_strcmp0(app->selected, "main.c")) goto failed;
        app->smoke_build_mode = "tree"; g_signal_emit_by_name(app->build_button, "clicked");
        app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 30) {
        if (!building(app)) goto failed;
        request_action(app, ACTION_PROJECT, app->smoke_root);
        if (app->busy || !close_requested(app->window, app)) goto failed;
        GtkTextIter end; gtk_text_buffer_get_end_iter(app->buffer, &end); gtk_text_buffer_insert(app->buffer, &end, "// saved during stopped build\n", -1);
        if (!save_document(app, TRUE) || !app->deferred_rescan) goto failed;
        build_panel_stop(app->build_panel); app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 31) {
        if (building(app)) return G_SOURCE_CONTINUE;
        if (!build_panel_stopped(app->build_panel)) goto failed;
        GtkTextIter cursor; gtk_text_buffer_get_iter_at_line_offset(app->buffer, &cursor, 100, 3);
        gtk_text_buffer_place_cursor(app->buffer, &cursor); graph_zoom_by(app->graph, 1.25); graph_pan(app->graph, 20, -15);
        save_session(app);
        g_autofree char *path = g_build_filename(app->smoke_root, "build", ".startapp-test", "workspace.ini", NULL);
        session_store_free(app->sessions); app->sessions = session_store_new(path);
        document_free(app->document); app->document = NULL; g_clear_pointer(&app->selected, g_free);
        graph_set_project(app->graph, NULL, -1); project_free(app->project); app->project = NULL;
        scan_project(app, app->smoke_root, TRUE); app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 32) {
        if (app->restore) return G_SOURCE_CONTINUE;
        GtkTextIter cursor; gtk_text_buffer_get_iter_at_mark(app->buffer, &cursor, gtk_text_buffer_get_insert(app->buffer));
        if (g_strcmp0(app->selected, "main.c") || gtk_text_iter_get_line(&cursor) != 100 ||
            gtk_text_iter_get_line_offset(&cursor) != 3 || app->selected_target != 0 || g_strcmp0(app->configuration, "Release")) goto failed;
        SessionState *saved = session_load(app->sessions, app->smoke_root);
        gboolean matches = saved && fabs(saved->zoom - graph_get_zoom(app->graph)) < .001;
        session_state_free(saved); if (!matches) goto failed;
        app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 33) {
        pin_tab(NULL, app); if (!app->current_tab) goto failed;
        app->smoke_tab_buffer = app->buffer;
        GtkTextIter end; gtk_text_buffer_get_end_iter(app->buffer, &end); gtk_text_buffer_insert(app->buffer, &end, "// preserved tab edit\n", -1);
        request_action(app, ACTION_FILE, "api.h");
        if (g_strcmp0(app->selected, "api.h") || app->confirming) goto failed;
        app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 34) {
        request_action(app, ACTION_FILE, "main.c");
        if (app->buffer != app->smoke_tab_buffer || !gtk_text_buffer_get_modified(app->buffer) || !gtk_text_buffer_get_can_undo(app->buffer)) goto failed;
        gtk_text_buffer_undo(app->buffer);
        if (!save_document(app, FALSE)) goto failed;
        request_action(app, ACTION_TAB_CLOSE, NULL);
        if (app->current_tab || app->tabs->len || app->document) goto failed;
        open_file(app, "main.c"); editor_go_to_line(app->editor, 101);
        app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 35) {
        gtk_toggle_button_set_active(app->target_mode, FALSE);
        graph_set_filter(app->graph, "", 2, 0); graph_set_project(app->graph, app->project, project_find_file(app->project, "api.h"));
        if (graph_visible_count(app->graph) < 2) goto failed;
        graph_set_filter(app->graph, "no-file-XYZ", 0, 1);
        if (graph_visible_count(app->graph) != 1) goto failed;
        graph_set_filter(app->graph, "", 0, 1); graph_set_project(app->graph, app->project, project_find_file(app->project, "main.c"));
        tools_clicked(NULL, app); if (!app->tools) goto failed;
        app->smoke_stage++; return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage >= 36 && app->smoke_stage <= 43) {
        gint result = tools_ui_smoke_step(app->tools, app->smoke_stage-36);
        if (result < 0) goto failed;
        if (result) app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 44) { tools_ui_free(app->tools); app->tools = NULL; tools_configured(app->project->root, app->project->build_directory, app); app->smoke_stage++; return G_SOURCE_CONTINUE; }
    if (app->smoke_stage >= 45 && app->smoke_stage <= 48) {
        if (app->configuration || g_strcmp0(app->project->configuration, "Debug")) goto failed;
        gint result = language_ui_smoke_step(app->language, app->smoke_stage-45);
        if (result < 0) goto failed;
        if (result) app->smoke_stage++;
        return G_SOURCE_CONTINUE;
    }
    if (app->smoke_stage == 49) { gtk_text_buffer_set_text(app->buffer, app->document->text, -1); gtk_text_buffer_set_modified(app->buffer, FALSE); editor_go_to_line(app->editor, 101); app->smoke_stage++; return G_SOURCE_CONTINUE; }
    if (!take_screenshot(app)) goto failed;
    GtkWidget *controls[] = {app->scan_button, app->save_button, GTK_WIDGET(app->target_mode), GTK_WIDGET(app->position)};
    for (guint i = 0; i < G_N_ELEMENTS(controls); i++) {
        graphene_rect_t bounds;
        if (!gtk_widget_compute_bounds(controls[i], GTK_WIDGET(app->window), &bounds) ||
            bounds.origin.x < 0 || bounds.origin.x + bounds.size.width > gtk_widget_get_width(GTK_WIDGET(app->window)) + 1)
            goto failed;
    }
    save_session(app);
    g_print("UI smoke PASS: editor/graph, build save dialog, streamed failure and navigation, success/rescan, cancellation, session restore, pinned tabs/undo, graph filters, search/impact/rules/compare/environment/export/catalog, optional clangd diagnostics/completion/definition\n");
    app->closing = TRUE;
    gtk_window_destroy(app->window);
    return G_SOURCE_REMOVE;
failed:
    g_printerr("UI smoke FAIL at stage %u: %s\n", app->smoke_stage, gtk_label_get_text(app->status));
    {
        GtkTextIter cursor;
        guint first = 0, last = 0;
        gtk_text_buffer_get_iter_at_mark(app->buffer, &cursor, gtk_text_buffer_get_insert(app->buffer));
        editor_gutter_visible_lines(app->editor, &first, &last);
        take_screenshot(app);
        g_printerr("file=%s cursor=%d rows=%u gutter=%u..%u total=%d query=%s\n",
            app->selected ? app->selected : "(none)", gtk_text_iter_get_line(&cursor) + 1,
            g_list_model_get_n_items(G_LIST_MODEL(app->selection)), first, last,
            gtk_text_buffer_get_line_count(app->buffer), gtk_editable_get_text(GTK_EDITABLE(app->search)));
        GtkTextIter start, end;
        gboolean has_selection = gtk_text_buffer_get_selection_bounds(app->buffer, &start, &end);
        g_autofree char *selection = gtk_text_buffer_get_text(app->buffer, &start, &end, FALSE);
        GtkWidget *focus = gtk_window_get_focus(app->window);
        g_printerr("selection=%d [%s] focus=%s search_focus=%d\n", has_selection, selection,
            focus ? G_OBJECT_TYPE_NAME(focus) : "none", focus &&
            (focus == GTK_WIDGET(app->search) || gtk_widget_is_ancestor(focus, GTK_WIDGET(app->search))));
    }
    app->smoke_result = 1;
    app->closing = TRUE;
    gtk_window_destroy(app->window);
    return G_SOURCE_REMOVE;
}

static void language_navigate(const char *path, guint line, gpointer data)
{
    App *app = data;
    if (g_strcmp0(app->selected, path) == 0) { editor_go_to_line(app->editor, line); return; }
    if (app->project && project_find_file(app->project, path) >= 0) request_action_at(app, ACTION_LOCATION, path, line);
}
static void language_status(const char *message, gpointer data) { status(data, message); }
static void graph_filter_changed(gpointer object, gpointer unused, gpointer data)
{
    (void)object; (void)unused; App *app = data;
    graph_set_filter(app->graph, gtk_editable_get_text(GTK_EDITABLE(app->graph_search)), gtk_drop_down_get_selected(app->graph_direction), (guint)gtk_spin_button_get_value_as_int(app->graph_depth));
}
static void graph_filter_entry(GtkEditable *e, gpointer data) { graph_filter_changed(e, NULL, data); }
static void graph_filter_depth(GtkSpinButton *s, gpointer data) { graph_filter_changed(s, NULL, data); }
static void graph_explain(gboolean targets, guint index, gpointer data)
{
    App *app = data; if (!app->project) return;
    guint from, to; const char *path; guint line;
    if (targets) { ProjectTargetEdge *e = g_ptr_array_index(app->project->target_edges, index); from = e->from; to = e->to; path = e->definition; line = e->line; }
    else { ProjectEdge *e = g_ptr_array_index(app->project->edges, index); if (e->to < 0) return; from = e->from; to = (guint)e->to; path = ((ProjectFile *)g_ptr_array_index(app->project->files, from))->path; line = e->line; }
    g_autofree char *why = workspace_explain(app->project, targets, from, to); g_autofree char *escaped = g_markup_escape_text(why, -1);
    GString *text = g_string_new(escaped); append_origin(text, targets ? "targetedge" : "include", index, path, line);
    gtk_label_set_markup(app->details, text->str); g_string_free(text, TRUE);
}

static gboolean monitor_refresh(gpointer data)
{
    App *app = data;
    if (app->closing) { app->monitor_timer = 0; return G_SOURCE_REMOVE; }
    if (app->busy || app->confirming || building(app)) return G_SOURCE_CONTINUE;
    app->monitor_timer = 0;
    if (app->document) {
        Document *disk = document_open(app->document->path, NULL);
        if (disk && g_strcmp0(disk->text, app->document->text)) {
            if (gtk_text_buffer_get_modified(app->buffer)) status(app, "Файл изменён извне · правки сохранены в буфере; перед записью сравните версии");
            else {
                document_free(app->document); app->document = disk; disk = NULL;
                GtkTextIter at; gtk_text_buffer_get_iter_at_mark(app->buffer, &at, gtk_text_buffer_get_insert(app->buffer)); guint row = gtk_text_iter_get_line(&at)+1;
                gtk_text_buffer_set_enable_undo(app->buffer, FALSE); gtk_text_buffer_set_text(app->buffer, app->document->text, -1); gtk_text_buffer_set_enable_undo(app->buffer, TRUE); gtk_text_buffer_set_modified(app->buffer, FALSE); editor_go_to_line(app->editor, MIN(row, (guint)gtk_text_buffer_get_line_count(app->buffer)));
            }
        }
        document_free(disk);
    }
    if (app->project) scan_project(app, app->project->root, FALSE);
    return G_SOURCE_REMOVE;
}
static void monitor_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data)
{
    (void)monitor; (void)other; App *app = data;
    if (app->closing || event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED) return;
    g_autofree char *base = g_file_get_basename(file);
    if (base && (g_str_has_prefix(base, ".goutputstream-") || g_str_equal(base, "workspace.ini") || g_str_equal(base, "catalog.ini"))) return;
    if (app->monitor_timer) g_source_remove(app->monitor_timer);
    app->monitor_timer = g_timeout_add(1200, monitor_refresh, app);
}
static void setup_monitors(App *app)
{
    g_ptr_array_set_size(app->monitors, 0);
    if (app->smoke || !app->project) return;
    g_autoptr(GHashTable) dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL); g_hash_table_add(dirs, g_strdup(app->project->root));
    for (guint i = 0; i < app->project->files->len && g_hash_table_size(dirs) < 256; i++) {
        ProjectFile *f = g_ptr_array_index(app->project->files, i); g_autofree char *absolute = g_build_filename(app->project->root, f->path, NULL); g_hash_table_add(dirs, g_path_get_dirname(absolute));
    }
    if (app->project->build_directory) {
        g_hash_table_add(dirs, g_strdup(app->project->build_directory));
        g_hash_table_add(dirs, g_build_filename(app->project->build_directory, ".cmake", "api", "v1", "reply", NULL));
    }
    GHashTableIter it; gpointer dir; g_hash_table_iter_init(&it, dirs);
    while (g_hash_table_iter_next(&it, &dir, NULL)) {
        g_autoptr(GFile) file = g_file_new_for_path(dir); GFileMonitor *monitor = g_file_monitor_directory(file, G_FILE_MONITOR_NONE, NULL, NULL);
        if (monitor) { g_signal_connect(monitor, "changed", G_CALLBACK(monitor_changed), app); g_ptr_array_add(app->monitors, monitor); }
    }
}

static void tools_navigate(const char *root, const char *path, guint line, gpointer data)
{
    App *app = data;
    if (!app->project || g_strcmp0(root, app->project->root)) request_action(app, ACTION_PROJECT, root);
    else if (path && project_find_file(app->project, path) >= 0) request_action_at(app, ACTION_LOCATION, path, MAX(1u, line));
    gtk_window_present(app->window);
}
static void tools_configured(const char *root, const char *directory, gpointer data)
{
    App *app = data;
    if (!app->project || g_strcmp0(root, app->project->root)) return;
    g_free(app->build_directory); app->build_directory = g_strdup(directory);
    g_clear_pointer(&app->configuration, g_free);
    scan_project(app, root, FALSE);
}
static void git_overlay(const char *root, GHashTable *paths, gpointer data)
{
    App *app = data; if (!app->project || g_strcmp0(root, app->project->root)) return;
    g_clear_pointer(&app->git_changed, g_hash_table_unref); app->git_changed = g_hash_table_ref(paths);
    graph_set_changed(app->graph, paths); populate_tree(app); status(app, "Изменённые файлы отмечены на карте · обновление через Git → Статус файлов");
}

static void tools_clicked(GtkButton *button, gpointer data)
{
    (void)button; App *app = data;
    if (!app->project || app->busy || building(app)) return;
    if (!app->tools) { app->tools = tools_ui_new(app->application, app->window, tools_navigate, tools_configured, app); tools_ui_set_git(app->tools, git_overlay); }
    tools_ui_show(app->tools, app->project, app->selected, app->selected_target);
}
static void cancel_scan(GtkButton *button, gpointer data)
{ (void)button; App *app = data; if (app->scan_cancel) g_cancellable_cancel(app->scan_cancel); }

static void activate(GtkApplication *application, gpointer data)
{
    App *app = data;
    theme_install();
    app->window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(app->window, "StartApp · карта проекта");
    gtk_window_set_default_size(app->window, app->smoke ? 1200 : 1420, app->smoke ? 800 : 900);
    /* No custom titlebar: the OS/window manager supplies window controls. */
    gtk_window_set_decorated(app->window, TRUE);
    g_signal_connect(app->window, "close-request", G_CALLBACK(close_requested), app);
    GtkEventController *keyboard = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keyboard, GTK_PHASE_CAPTURE);
    g_signal_connect(keyboard, "key-pressed", G_CALLBACK(key_pressed), app);
    gtk_widget_add_controller(GTK_WIDGET(app->window), keyboard);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(app->window, body);
    app->build_panel = build_panel_new(build_navigate, build_finished, app);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(header, "toolbar");
    GtkWidget *brand = label_new("StartApp", "title");
    gtk_widget_add_css_class(brand, "brand");
    gtk_box_append(GTK_BOX(header), brand);
    GtkWidget *open = button_new("Открыть проект", G_CALLBACK(choose_folder), app);
    gtk_widget_add_css_class(open, "suggested-action");
    gtk_box_append(GTK_BOX(header), open);
    gtk_box_append(GTK_BOX(header), button_new("Инструменты", G_CALLBACK(tools_clicked), app));
    GtkWidget *subtitle = label_new("Карта проекта", "muted");
    gtk_widget_set_hexpand(subtitle, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(header), subtitle);
    app->scan_button = button_new("Обновить карту", G_CALLBACK(scan_clicked), app);
    gtk_box_append(GTK_BOX(header), app->scan_button);
    GtkWidget *cancel = button_new("×", G_CALLBACK(cancel_scan), app); gtk_widget_set_tooltip_text(cancel, "Отменить сканирование"); gtk_box_append(GTK_BOX(header), cancel);
    app->log_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Журнал сборки"));
    g_signal_connect(app->log_toggle, "toggled", G_CALLBACK(log_toggled), app);
    gtk_box_append(GTK_BOX(header), GTK_WIDGET(app->log_toggle));
    gtk_box_append(GTK_BOX(body), header);
    app->main_pane = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_VERTICAL));
    gtk_widget_set_vexpand(GTK_WIDGET(app->main_pane), TRUE); gtk_box_append(GTK_BOX(body), GTK_WIDGET(app->main_pane));
    GtkWidget *outer = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    app->outer = GTK_PANED(outer);
    gtk_widget_set_vexpand(outer, TRUE);
    gtk_paned_set_start_child(app->main_pane, outer);
    gtk_paned_set_end_child(app->main_pane, build_panel_widget(app->build_panel));
    gtk_paned_set_position(app->main_pane, 520); gtk_paned_set_shrink_start_child(app->main_pane, FALSE);
    gtk_paned_set_shrink_end_child(app->main_pane, TRUE); gtk_widget_set_visible(build_panel_widget(app->build_panel), FALSE);
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 240, -1);
    gtk_box_append(GTK_BOX(sidebar), label_new("ПРОЕКТ", "section"));
    app->project_name = GTK_LABEL(label_new("Папка не выбрана", "title"));
    gtk_label_set_ellipsize(app->project_name, PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(app->project_name));
    const char *recent_default[] = {"Недавние проекты", NULL};
    app->recent_picker = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(recent_default));
    GtkListItemFactory *recent_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(recent_factory, "setup", G_CALLBACK(target_item_setup), NULL);
    g_signal_connect(recent_factory, "bind", G_CALLBACK(target_item_bind), NULL);
    gtk_drop_down_set_factory(app->recent_picker, recent_factory); gtk_drop_down_set_list_factory(app->recent_picker, recent_factory);
    g_object_unref(recent_factory);
    g_signal_connect(app->recent_picker, "notify::selected", G_CALLBACK(recent_changed), app);
    gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(app->recent_picker)); refresh_recent(app);
    app->summary = GTK_LABEL(label_new("Выберите папку с исходниками.\nФайлы сборки также доступны\nдля просмотра и редактирования.", "muted"));
    gtk_label_set_wrap(app->summary, TRUE);
    gtk_label_set_max_width_chars(app->summary, 28);
    gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(app->summary));
    app->build_chain = GTK_LABEL(label_new("Сборка ещё не определена", "muted"));
    gtk_label_set_wrap(app->build_chain, TRUE);
    gtk_label_set_max_width_chars(app->build_chain, 28);
    gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(app->build_chain));
    app->sidebar_pages = GTK_STACK(gtk_stack_new());
    gtk_stack_set_vhomogeneous(app->sidebar_pages, FALSE);
    gtk_widget_set_vexpand(GTK_WIDGET(app->sidebar_pages), TRUE);
    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), app->sidebar_pages);
    gtk_box_append(GTK_BOX(sidebar), switcher);
    gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(app->sidebar_pages));
    GtkWidget *file_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *build_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_stack_add_titled(app->sidebar_pages, file_panel, "files", "Файлы");
    gtk_stack_add_titled(app->sidebar_pages, scroll(build_panel), "build", "Сборка");
    app->metadata_button = button_new("Каталог сборки…", G_CALLBACK(choose_metadata), app);
    gtk_widget_set_sensitive(app->metadata_button, FALSE);
    gtk_box_append(GTK_BOX(build_panel), app->metadata_button);
    app->build_name = GTK_LABEL(label_new("Автопоиск метаданных", "muted"));
    gtk_label_set_ellipsize(app->build_name, PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(build_panel), GTK_WIDGET(app->build_name));
    const char *default_config[] = {"Без конфигурации", NULL};
    app->configuration_picker = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(default_config));
    gtk_widget_set_sensitive(GTK_WIDGET(app->configuration_picker), FALSE);
    g_signal_connect(app->configuration_picker, "notify::selected", G_CALLBACK(configuration_changed), app);
    gtk_box_append(GTK_BOX(build_panel), GTK_WIDGET(app->configuration_picker));
    gtk_box_append(GTK_BOX(build_panel), label_new("Инструмент CMake", "muted"));
    app->cmake_entry = GTK_ENTRY(gtk_entry_new());
    g_autofree char *cmake_program = g_find_program_in_path("cmake");
    gtk_editable_set_text(GTK_EDITABLE(app->cmake_entry), cmake_program ? cmake_program : "cmake");
    gtk_entry_set_placeholder_text(app->cmake_entry, "Путь к cmake / cmake.exe");
    g_signal_connect(app->cmake_entry, "changed", G_CALLBACK(cmake_changed), app);
    gtk_box_append(GTK_BOX(build_panel), GTK_WIDGET(app->cmake_entry));
    app->build_command = GTK_LABEL(label_new("Откройте настроенный CMake-проект", "muted"));
    gtk_label_set_ellipsize(app->build_command, PANGO_ELLIPSIZE_MIDDLE); gtk_label_set_max_width_chars(app->build_command, 28);
    gtk_box_append(GTK_BOX(build_panel), GTK_WIDGET(app->build_command));
    app->build_button = button_new("Собрать выбранную цель", G_CALLBACK(build_clicked), app);
    gtk_widget_add_css_class(app->build_button, "suggested-action"); gtk_box_append(GTK_BOX(build_panel), app->build_button);
    app->build_profile = GTK_LABEL(label_new("Откройте проект для просмотра параметров сборки.", "muted"));
    gtk_label_set_wrap(app->build_profile, TRUE);
    gtk_label_set_wrap_mode(app->build_profile, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(app->build_profile, 28);
    gtk_label_set_selectable(app->build_profile, TRUE);
    gtk_box_append(GTK_BOX(build_panel), GTK_WIDGET(app->build_profile));
    app->search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(app->search, "Поиск файлов · Ctrl+P");
    gtk_search_entry_set_search_delay(app->search, 150);
    g_signal_connect(app->search, "search-changed", G_CALLBACK(search_changed), app);
    g_signal_connect(app->search, "stop-search", G_CALLBACK(search_stopped), app);
    g_signal_connect(app->search, "activate", G_CALLBACK(search_activate), app);
    gtk_box_append(GTK_BOX(file_panel), GTK_WIDGET(app->search));
    app->search_result = GTK_LABEL(label_new("", "muted"));
    gtk_box_append(GTK_BOX(file_panel), GTK_WIDGET(app->search_result));
    app->selection = gtk_single_selection_new(NULL);
    gtk_single_selection_set_autoselect(app->selection, FALSE);
    gtk_single_selection_set_can_unselect(app->selection, TRUE);
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(tree_setup), app);
    g_signal_connect(factory, "bind", G_CALLBACK(tree_bind), app);
    g_signal_connect(factory, "unbind", G_CALLBACK(tree_unbind), app);
    app->files = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(app->selection), factory));
    gtk_list_view_set_single_click_activate(app->files, TRUE);
    g_signal_connect(app->files, "activate", G_CALLBACK(tree_activate), app);
    gtk_box_append(GTK_BOX(file_panel), scroll(GTK_WIDGET(app->files)));
    gtk_paned_set_start_child(GTK_PANED(outer), sidebar);
    gtk_paned_set_resize_start_child(GTK_PANED(outer), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(outer), FALSE);
    gtk_paned_set_position(GTK_PANED(outer), 250);

    GtkWidget *workspace = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    app->workspace = GTK_PANED(workspace);
    gtk_paned_set_end_child(GTK_PANED(outer), workspace);
    GtkWidget *editor_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *editor_toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(editor_toolbar, "toolbar");
    app->file_name = GTK_LABEL(label_new("Выберите файл", NULL));
    gtk_label_set_ellipsize(app->file_name, PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(GTK_WIDGET(app->file_name), TRUE);
    gtk_box_append(GTK_BOX(editor_toolbar), GTK_WIDGET(app->file_name));
    app->reload_button = button_new("Перечитать", G_CALLBACK(reload_clicked), app);
    app->save_button = button_new("Сохранить", G_CALLBACK(save_clicked), app);
    gtk_box_append(GTK_BOX(editor_toolbar), app->reload_button);
    gtk_box_append(GTK_BOX(editor_toolbar), app->save_button);
    gtk_box_append(GTK_BOX(editor_box), editor_toolbar);
    app->tab_bar = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3));
    GtkWidget *tab_scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tab_scroll), GTK_WIDGET(app->tab_bar)); gtk_box_append(GTK_BOX(editor_box), tab_scroll);
    gtk_box_append(app->tab_bar, button_new("＋", G_CALLBACK(pin_tab), app)); gtk_box_append(app->tab_bar, button_new("Закрыть вкладку", G_CALLBACK(close_tab), app));
    app->editor = editor_new();
    gtk_text_view_set_monospace(app->editor, TRUE);
    gtk_text_view_set_editable(app->editor, FALSE);
    gtk_text_view_set_left_margin(app->editor, 16);
    gtk_text_view_set_right_margin(app->editor, 16);
    gtk_text_view_set_top_margin(app->editor, 16);
    app->buffer = gtk_text_view_get_buffer(app->editor);
    gtk_text_buffer_set_enable_undo(app->buffer, TRUE);
    g_signal_connect(app->buffer, "changed", G_CALLBACK(buffer_changed), app);
    g_signal_connect(app->buffer, "modified-changed", G_CALLBACK(buffer_changed), app);
    g_signal_connect(app->buffer, "mark-set", G_CALLBACK(cursor_moved), app);
    GtkWidget *editor_scroll = scroll(GTK_WIDGET(app->editor));
    editor_attach_gutter(app->editor, GTK_SCROLLED_WINDOW(editor_scroll));
    app->language = language_ui_new(app->application, app->window, app->editor, language_navigate, language_status, app);
    gtk_box_append(GTK_BOX(editor_box), language_ui_widget(app->language));
    gtk_box_append(GTK_BOX(editor_box), editor_scroll);
    gtk_paned_set_start_child(GTK_PANED(workspace), editor_box);

    GtkWidget *graph_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *graph_toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(graph_toolbar, "toolbar");
    GtkWidget *graph_title = label_new("КАРТА", "section");
    gtk_widget_set_hexpand(graph_title, TRUE);
    gtk_box_append(GTK_BOX(graph_toolbar), graph_title);
    const char *all_targets[] = {"Все цели", NULL};
    app->target_picker = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(all_targets));
    GtkListItemFactory *target_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(target_factory, "setup", G_CALLBACK(target_item_setup), NULL);
    g_signal_connect(target_factory, "bind", G_CALLBACK(target_item_bind), NULL);
    gtk_drop_down_set_factory(app->target_picker, target_factory);
    gtk_drop_down_set_list_factory(app->target_picker, target_factory);
    g_object_unref(target_factory);
    gtk_drop_down_set_enable_search(app->target_picker, TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->target_picker), FALSE);
    gtk_widget_set_size_request(GTK_WIDGET(app->target_picker), 120, -1);
    g_signal_connect(app->target_picker, "notify::selected", G_CALLBACK(target_changed), app);
    app->target_mode = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Цели"));
    gtk_box_append(GTK_BOX(graph_toolbar), GTK_WIDGET(app->target_mode));
    app->overview = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Весь проект"));
    g_signal_connect(app->overview, "toggled", G_CALLBACK(overview_toggled), app);
    gtk_box_append(GTK_BOX(graph_toolbar), GTK_WIDGET(app->overview));
    gtk_box_append(GTK_BOX(graph_box), graph_toolbar);
    GtkWidget *target_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(target_row, "toolbar");
    gtk_box_append(GTK_BOX(target_row), label_new("Цель сборки", "muted"));
    gtk_widget_set_hexpand(GTK_WIDGET(app->target_picker), TRUE);
    gtk_box_append(GTK_BOX(target_row), GTK_WIDGET(app->target_picker));
    gtk_box_append(GTK_BOX(graph_box), target_row);
    app->graph = graph_new(graph_open, graph_target_selected, app);
    graph_set_explain(app->graph, graph_explain);
    GtkWidget *filter = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    app->graph_search = GTK_ENTRY(gtk_entry_new()); gtk_entry_set_placeholder_text(app->graph_search, "Фильтр карты"); gtk_widget_set_hexpand(GTK_WIDGET(app->graph_search), TRUE); gtk_box_append(GTK_BOX(graph_box), GTK_WIDGET(app->graph_search));
    const char *directions[] = {"Оба направления", "Зависимости", "Пользователи", NULL}; app->graph_direction = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(directions)); gtk_box_append(GTK_BOX(filter), GTK_WIDGET(app->graph_direction));
    app->graph_depth = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 64, 1)); gtk_editable_set_width_chars(GTK_EDITABLE(app->graph_depth), 2); gtk_spin_button_set_value(app->graph_depth, 1); gtk_widget_set_tooltip_text(GTK_WIDGET(app->graph_depth), "Глубина связей; 0 — все уровни"); gtk_box_append(GTK_BOX(filter), GTK_WIDGET(app->graph_depth));
    g_signal_connect(app->graph_search, "changed", G_CALLBACK(graph_filter_entry), app); g_signal_connect(app->graph_direction, "notify::selected", G_CALLBACK(graph_filter_changed), app); g_signal_connect(app->graph_depth, "value-changed", G_CALLBACK(graph_filter_depth), app);
    gtk_box_append(GTK_BOX(graph_box), filter);
    g_signal_connect(app->target_mode, "toggled", G_CALLBACK(target_mode_toggled), app);
    GtkWidget *view_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(view_row, "toolbar");
    GtkWidget *zoom_out = button_new("−", G_CALLBACK(graph_zoom_out), app);
    GtkWidget *zoom_in = button_new("+", G_CALLBACK(graph_zoom_in), app);
    gtk_widget_set_tooltip_text(zoom_out, "Уменьшить масштаб карты");
    gtk_widget_set_tooltip_text(zoom_in, "Увеличить масштаб карты");
    gtk_box_append(GTK_BOX(view_row), zoom_out);
    gtk_box_append(GTK_BOX(view_row), zoom_in);
    gtk_box_append(GTK_BOX(view_row), button_new("Вписать", G_CALLBACK(graph_fit_clicked), app));
    app->groups = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Группы"));
    g_signal_connect(app->groups, "toggled", G_CALLBACK(groups_toggled), app);
    gtk_widget_set_tooltip_text(GTK_WIDGET(app->groups), "В обзоре: свернуть каталоги или типы целей. Нажмите на группу для раскрытия, ПКМ — для сворачивания.");
    gtk_box_append(GTK_BOX(view_row), GTK_WIDGET(app->groups));
    gtk_box_append(GTK_BOX(graph_box), view_row);
    gtk_box_append(GTK_BOX(graph_box), graph_widget(app->graph));
    app->details = GTK_LABEL(label_new("Нажмите на узел, чтобы открыть файл.\nСтрелка: исходник → включаемый заголовок.", "details"));
    gtk_label_set_wrap(app->details, TRUE);
    gtk_label_set_max_width_chars(app->details, 46);
    gtk_label_set_selectable(app->details, TRUE);
    g_signal_connect(app->details, "activate-link", G_CALLBACK(details_link), app);
    GtkWidget *details_scroll = scroll(GTK_WIDGET(app->details));
    gtk_widget_set_vexpand(details_scroll, FALSE);
    gtk_widget_set_size_request(details_scroll, -1, 100);
    gtk_box_append(GTK_BOX(graph_box), details_scroll);
    gtk_paned_set_end_child(GTK_PANED(workspace), graph_box);
    gtk_paned_set_position(GTK_PANED(workspace), 470);
    gtk_paned_set_shrink_start_child(GTK_PANED(workspace), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(workspace), FALSE);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(footer, "status");
    app->status = GTK_LABEL(label_new("Готово · Ctrl+O открыть · Ctrl+P поиск · Ctrl+S сохранить · Ctrl+R карта", NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(app->status), TRUE);
    gtk_label_set_ellipsize(app->status, PANGO_ELLIPSIZE_END);
    app->position = GTK_LABEL(label_new("UTF-8", NULL));
    gtk_box_append(GTK_BOX(footer), GTK_WIDGET(app->status));
    gtk_box_append(GTK_BOX(footer), GTK_WIDGET(app->position));
    gtk_box_append(GTK_BOX(body), footer);
    update_document_title(app);
    gtk_window_present(app->window);
    update_build_controls(app);
    if (app->initial_path) scan_project(app, app->initial_path, TRUE);
    if (app->smoke) g_timeout_add(350, smoke_tick, app);
}

int app_run(const char *project_path, const char *build_directory, const char *configuration, gboolean smoke_test)
{
    App app = {0};
    app.tabs = g_ptr_array_new_with_free_func(tab_free);
    app.monitors = g_ptr_array_new_with_free_func(g_object_unref);
    app.smoke = smoke_test;
    app.selected_target = -1;
    app.build_directory = g_strdup(build_directory);
    app.configuration = g_strdup(configuration);
    app.initial_path = g_strdup(project_path);
    if (smoke_test) {
        app.smoke_root = g_dir_make_tmp("startapp-ui-XXXXXX", NULL);
        if (!app.smoke_root) return 1;
        GString *main_text = g_string_new(NULL);
        for (guint i = 1; i <= 100; i++)
            g_string_append_printf(main_text, "// Context line %u\n", i);
        g_string_append(main_text, "#include \"api.h\"\nint main(void) { return 0; }\n");
        const char *names[] = {"main.c", "api.h", "extra.h", "Makefile", "src/МОДУЛЬ.c"};
        const char *contents[] = {main_text->str, "#pragma once\n", "#pragma once\n",
                                 "all:\n\tcc main.c -o app\n", "// Unicode search fixture\n"};
        for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
            g_autofree char *path = g_build_filename(app.smoke_root, names[i], NULL);
            g_autofree char *parent = g_path_get_dirname(path);
            if (g_mkdir_with_parents(parent, 0700) != 0) return 1;
            if (!g_file_set_contents(path, contents[i], -1, NULL)) return 1;
        }
        g_string_free(main_text, TRUE);
        g_free(app.initial_path);
        app.initial_path = g_strdup(app.smoke_root);
    }
    g_autofree char *session_file = app.smoke_root ? g_build_filename(app.smoke_root, "build", ".startapp-test", "workspace.ini", NULL) : NULL;
    app.recovery_dir = app.smoke_root ? g_build_filename(app.smoke_root, "build", ".startapp-test", "recovery", NULL) : g_build_filename(g_get_user_config_dir(), "startapp", "recovery", NULL);
    app.sessions = session_store_new(session_file);
    if (!app.initial_path) {
        g_auto(GStrv) recent = session_recent(app.sessions);
        for (guint i = 0; recent[i]; i++) if (g_file_test(recent[i], G_FILE_TEST_IS_DIR)) { app.initial_path = g_strdup(recent[i]); break; }
    }
    app.application = gtk_application_new("io.github.PaperGooseRus.StartApp", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
    int result = g_application_run(G_APPLICATION(app.application), 0, NULL);
    app.closing = TRUE;
    if (app.recovery_timer) g_source_remove(app.recovery_timer);
    if (app.monitor_timer) g_source_remove(app.monitor_timer);
    g_object_unref(app.application);
    if (app.current_tab) { app.current_tab->document = app.document; app.document = NULL; }
    document_free(app.document);
    g_ptr_array_unref(app.tabs);
    g_ptr_array_unref(app.monitors);
    g_clear_pointer(&app.git_changed, g_hash_table_unref);
    g_free(app.recovery_dir);
    project_free(app.project);
    if (app.graph) graph_free(app.graph);
    language_ui_free(app.language);
    tools_ui_free(app.tools);
    build_panel_free(app.build_panel); session_state_free(app.restore); session_store_free(app.sessions);
    g_free(app.selected);
    g_free(app.pending_path);
    g_free(app.queued_scan);
    g_free(app.initial_path);
    g_free(app.build_directory);
    g_free(app.configuration);
    if (app.smoke_root) {
        smoke_remove_tree(app.smoke_root);
        g_free(app.smoke_root);
    }
    return app.smoke_result ? app.smoke_result : result;
}
