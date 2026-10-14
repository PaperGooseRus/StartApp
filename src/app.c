#include "app.h"
#include "document.h"
#include "editor.h"
#include "graph.h"
#include <glib/gstdio.h>

typedef enum { ACTION_NONE, ACTION_FILE, ACTION_LOCATION, ACTION_PROJECT, ACTION_CLOSE, ACTION_RELOAD } Action;

typedef struct {
    GtkApplication *application;
    GtkWindow *window;
    GtkTextView *editor;
    GtkTextBuffer *buffer;
    GtkListView *files;
    GtkSingleSelection *selection;
    GtkSearchEntry *search;
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
    guint highlight_source;
    gboolean smoke;
    guint smoke_stage;
    gboolean smoke_link_pending;
    gint smoke_result;
    char *smoke_root;
    char *initial_path;
} App;

typedef struct { char *path; gboolean replace; } ScanRequest;

static void scan_project(App *app, const char *path, gboolean replace);
static gboolean save_document(App *app, gboolean rescan);
static void request_action(App *app, Action action, const char *path);
static void request_action_at(App *app, Action action, const char *path, guint line);
static void cursor_moved(GtkTextBuffer *buffer, const GtkTextIter *location,
                         GtkTextMark *mark, gpointer user_data);
static void select_current_file(App *app);

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
    GString *details = g_string_new("Нажмите на <b>путь:строку</b>, чтобы перейти к источнику связи.\n"
        "Статический анализ #include. Условия препроцессора и -I не вычисляются.\n");
    gint selected = app->selected ? project_find_file(app->project, app->selected) : -1;
    guint shown = 0;
    for (guint i = 0; i < app->project->edges->len; i++) {
        ProjectEdge *edge = g_ptr_array_index(app->project->edges, i);
        if (selected >= 0 ? ((gint)edge->from != selected && edge->to != selected) :
            (edge->to >= 0 || edge->system)) continue;
        if (++shown > 25) { g_string_append(details, "… остальные связи доступны через --inspect\n"); break; }
        ProjectFile *from = g_ptr_array_index(app->project->files, edge->from);
        const char *kind = edge->to >= 0 ? "локальный" : edge->ambiguous ? "неоднозначный" :
                           edge->system ? "внешний / системный" : "не разрешён";
        g_autofree char *path = g_markup_escape_text(from->path, -1);
        g_autofree char *include = g_markup_escape_text(edge->include, -1);
        g_string_append_printf(details, "<a href=\"include:%u\">%s:%u</a> → %s [%s]\n",
                               i, path, edge->line, include, kind);
    }
    for (guint i = 0; i < app->project->notes->len && i < 5; i++) {
        g_autofree char *note = g_markup_escape_text(g_ptr_array_index(app->project->notes, i), -1);
        g_string_append_printf(details, "%s\n", note);
    }
    gtk_label_set_markup(app->details, details->str);
    g_string_free(details, TRUE);
}

static gboolean highlight(gpointer user_data)
{
    App *app = user_data;
    app->highlight_source = 0;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(app->buffer, &start, &end);
    gtk_text_buffer_remove_all_tags(app->buffer, &start, &end);
    if (!app->document) return G_SOURCE_REMOVE;
    g_autofree char *text = gtk_text_buffer_get_text(app->buffer, &start, &end, FALSE);
    struct { const char *pattern, *tag; } patterns[] = {
        {"\\b(int|char|void|float|double|long|short|unsigned|signed|const|static|"
         "struct|typedef|enum|union|return|if|else|for|while|switch|case|break|sizeof|"
         "NULL|gboolean|guint|gint)\\b", "keyword"},
        {"(?m)^\\s*#.*$", "directive"},
        {"\"(?:\\\\.|[^\"\\\\])*\"", "string"},
        {"(?m)//[^\\n]*|(?s:/\\*.*?\\*/)", "comment"}
    };
    if (!g_str_has_suffix(app->document->path, ".c") &&
        !g_str_has_suffix(app->document->path, ".h")) return G_SOURCE_REMOVE;
    for (guint i = 0; i < G_N_ELEMENTS(patterns); i++) {
        g_autoptr(GRegex) regex = g_regex_new(patterns[i].pattern, G_REGEX_OPTIMIZE, 0, NULL);
        g_autoptr(GMatchInfo) matches = NULL;
        g_regex_match(regex, text, 0, &matches);
        while (g_match_info_matches(matches)) {
            gint begin, finish;
            g_match_info_fetch_pos(matches, 0, &begin, &finish);
            gtk_text_buffer_get_iter_at_offset(app->buffer, &start,
                (gint)g_utf8_pointer_to_offset(text, text + begin));
            gtk_text_buffer_get_iter_at_offset(app->buffer, &end,
                (gint)g_utf8_pointer_to_offset(text, text + finish));
            gtk_text_buffer_apply_tag_by_name(app->buffer, patterns[i].tag, &start, &end);
            g_match_info_next(matches, NULL);
        }
    }
    return G_SOURCE_REMOVE;
}

static void buffer_changed(GtkTextBuffer *buffer, gpointer user_data)
{
    App *app = user_data;
    update_document_title(app);
    GtkTextIter cursor;
    GtkTextMark *insert = gtk_text_buffer_get_insert(buffer);
    gtk_text_buffer_get_iter_at_mark(buffer, &cursor, insert);
    cursor_moved(buffer, &cursor, insert, app);
    if (app->highlight_source) g_source_remove(app->highlight_source);
    app->highlight_source = g_timeout_add(180, highlight, app);
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

static gboolean open_file(App *app, const char *relative)
{
    if (!app->project) return FALSE;
    g_autofree char *absolute = g_build_filename(app->project->root, relative, NULL);
    g_autoptr(GError) error = NULL;
    Document *document = document_open(absolute, &error);
    if (!document) { show_error(app, "Не удалось открыть файл", error->message); return FALSE; }
    document_free(app->document);
    app->document = document;
    g_free(app->selected);
    app->selected = g_strdup(relative);
    select_current_file(app);
    /* Reset undo history when switching files. */
    gtk_text_buffer_set_enable_undo(app->buffer, FALSE);
    gtk_text_buffer_set_text(app->buffer, document->text, -1);
    gtk_text_buffer_set_enable_undo(app->buffer, TRUE);
    gtk_text_buffer_set_modified(app->buffer, FALSE);
    gtk_text_view_set_editable(app->editor, TRUE);
    graph_set_project(app->graph, app->project, project_find_file(app->project, relative));
    update_details(app);
    update_document_title(app);
    status(app, "Файл открыт · изменения графа появятся после сохранения");
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
    (void)factory; (void)data;
    GtkTreeListRow *row = gtk_list_item_get_item(item);
    GFile *file = gtk_tree_list_row_get_item(row);
    GtkTreeExpander *expander = GTK_TREE_EXPANDER(gtk_list_item_get_child(item));
    gtk_tree_expander_set_list_row(expander, row);
    g_autofree char *name = g_file_get_basename(file);
    gboolean directory = g_object_get_data(G_OBJECT(file), "children") != NULL;
    g_autofree char *label = g_strdup_printf("%s %s", directory ? "▸" : "·", name);
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
    g_free(request);
}

static void scan_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)cancel;
    ScanRequest *request = data;
    g_autoptr(GError) error = NULL;
    Project *project = project_scan(request->path, &error);
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
    if (app->closing) {
        project_free(project);
        g_application_release(G_APPLICATION(app->application));
        return;
    }
    gtk_text_view_set_editable(app->editor, app->document != NULL);
    gtk_widget_set_sensitive(app->scan_button, TRUE);
    if (!project) {
        show_error(app, "Не удалось прочитать проект", error->message);
        update_document_title(app);
        g_application_release(G_APPLICATION(app->application));
        return;
    }
    graph_set_project(app->graph, NULL, -1);
    project_free(app->project);
    app->project = project;
    if (request->replace) {
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
    populate_tree(app);
    gint selected = app->selected ? project_find_file(project, app->selected) : -1;
    graph_set_project(app->graph, project, selected);
    update_details(app);
    update_document_title(app);
    status(app, "Карта обновлена · скрипты проекта не запускались");
    g_application_release(G_APPLICATION(app->application));
}

static void scan_project(App *app, const char *path, gboolean replace)
{
    if (app->busy || app->closing) return;
    app->busy = TRUE;
    /* Keep selection/focus while reading; disable editing rather than the widget. */
    gtk_text_view_set_editable(app->editor, FALSE);
    gtk_widget_set_sensitive(app->scan_button, FALSE);
    update_document_title(app);
    status(app, "Чтение проекта…");
    ScanRequest *request = g_new0(ScanRequest, 1);
    request->path = g_strdup(path);
    request->replace = replace;
    GTask *task = g_task_new(NULL, NULL, scan_done, app);
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
    gtk_text_buffer_set_modified(app->buffer, FALSE);
    update_document_title(app);
    status(app, "Файл сохранён");
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
    else if (action == ACTION_RELOAD && app->selected) {
        g_autofree char *selected = g_strdup(app->selected);
        open_file(app, selected);
        if (app->project) scan_project(app, app->project->root, FALSE);
    } else if (action == ACTION_CLOSE) {
        app->closing = TRUE;
        gtk_window_destroy(app->window);
    }
}

static void confirm_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    App *app = user_data;
    g_autoptr(GError) error = NULL;
    gint choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
    app->confirming = FALSE;
    Action action = app->pending;
    guint line = app->pending_line;
    g_autofree char *path = g_steal_pointer(&app->pending_path);
    app->pending = ACTION_NONE;
    app->pending_line = 0;
    if (choice == 2 && !save_document(app, FALSE)) return;
    if (choice != 1 && choice != 2) return;
    perform_action(app, action, path, line);
    if (choice == 2 && (action == ACTION_FILE || action == ACTION_LOCATION) && app->project)
        scan_project(app, app->project->root, FALSE);
}

static void request_action_at(App *app, Action action, const char *path, guint line)
{
    if (app->busy || app->confirming || app->closing) return;
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
    if (!app->project || !g_str_has_prefix(uri, "include:")) return TRUE;
    char *end;
    guint64 index = g_ascii_strtoull(uri + 8, &end, 10);
    if (end == uri + 8 || *end || index >= app->project->edges->len) return TRUE;
    ProjectEdge *edge = g_ptr_array_index(app->project->edges, (guint)index);
    ProjectFile *from = g_ptr_array_index(app->project->files, edge->from);
    request_action_at(app, ACTION_LOCATION, from->path, edge->line);
    return TRUE; /* Never hand an internal link to the desktop URI launcher. */
}

static void folder_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    App *app = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
    if (folder && !app->closing) {
        g_autofree char *path = g_file_get_path(folder);
        if (path) request_action(app, ACTION_PROJECT, path);
    }
    g_application_release(G_APPLICATION(app->application));
}

static void choose_folder(GtkButton *button, gpointer user_data)
{
    (void)button;
    App *app = user_data;
    if (app->busy || app->confirming) return;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Открыть папку проекта");
    if (app->project) {
        g_autoptr(GFile) folder = g_file_new_for_path(app->project->root);
        gtk_file_dialog_set_initial_folder(dialog, folder);
    }
    g_application_hold(G_APPLICATION(app->application));
    gtk_file_dialog_select_folder(dialog, app->window, NULL, folder_chosen, app);
    g_object_unref(dialog);
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

static gboolean close_requested(GtkWindow *window, gpointer user_data)
{
    (void)window;
    App *app = user_data;
    if (app->confirming) return TRUE;
    if (app->busy) { status(app, "Дождитесь завершения анализа"); return TRUE; }
    if (gtk_text_buffer_get_modified(app->buffer)) {
        request_action(app, ACTION_CLOSE, NULL);
        return TRUE;
    }
    app->closing = TRUE;
    return FALSE;
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType modifiers, gpointer data)
{
    (void)controller; (void)keycode;
    App *app = data;
    if (!(modifiers & GDK_CONTROL_MASK)) return FALSE;
    if (keyval == GDK_KEY_p || keyval == GDK_KEY_P) {
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

static GtkWidget *scroll(GtkWidget *child)
{
    GtkWidget *scrollbar = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrollbar), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrollbar), child);
    gtk_widget_set_vexpand(scrollbar, TRUE);
    return scrollbar;
}

static void install_style(void)
{
    const char *css =
        "window { background: #101722; color: #dce5f1; }"
        "headerbar { background: #131e2c; border-bottom: 1px solid #29374b; }"
        "button { background: #223047; color: #dce5f1; border: 1px solid #33455d; box-shadow: none; }"
        "button:hover { background: #2c405c; }"
        "button:disabled { opacity: .4; }"
        "button.suggested-action { background: #246c61; border-color: #368a7c; }"
        ".sidebar { background: #131e2c; padding: 16px 12px; }"
        ".title { font-size: 17px; font-weight: bold; }"
        ".muted { color: #96a8bf; font-size: 11px; }"
        ".section { color: #64d3b5; font-size: 10px; font-weight: bold; letter-spacing: 1px; }"
        ".toolbar { padding: 10px 12px; background: #172232; }"
        ".status { padding: 8px 12px; background: #131e2c; color: #a6b8cc; }"
        "listview { background: transparent; color: #dce5f1; }"
        "listview row:selected { background: #263b51; }"
        "textview, textview text { background: #101722; color: #dce5f1; }"
        "textview text selection { background: #284b59; color: #effaff; }"
        "entry, searchentry { background: #101722; color: #dce5f1; border-color: #33455d; }"
        "label link { color: #83b9ef; }"
        ".details { background: #131e2c; padding: 12px; color: #a6b8cc; font-size: 11px; }";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
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
        if (!app->document) goto failed;
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
        /* Hit the first inbound/outbound column node through the graph's click path. */
        if (!graph_activate_at(app->graph,
            gtk_widget_get_width(graph_widget(app->graph)) - 70, 110)) goto failed;
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
    if (app->confirming && app->smoke_stage != 10 && app->smoke_stage != 11) return G_SOURCE_CONTINUE;
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
    guint first, last;
    if (!editor_gutter_visible_lines(app->editor, &first, &last) ||
        last != (guint)gtk_text_buffer_get_line_count(app->buffer)) goto failed;
    g_print("UI smoke PASS: edit/save/rescan, graph, undo/redo, dialogs, search/Unicode/Enter/Esc, source-line links, scrolled gutter\n");
    app->closing = TRUE;
    gtk_window_destroy(app->window);
    return G_SOURCE_REMOVE;
failed:
    g_printerr("UI smoke FAIL at stage %u\n", app->smoke_stage);
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

static void activate(GtkApplication *application, gpointer data)
{
    App *app = data;
    install_style();
    app->window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(app->window, "StartApp · карта проекта");
    gtk_window_set_default_size(app->window, 1420, 900);
    g_signal_connect(app->window, "close-request", G_CALLBACK(close_requested), app);
    GtkEventController *keyboard = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keyboard, GTK_PHASE_CAPTURE);
    g_signal_connect(keyboard, "key-pressed", G_CALLBACK(key_pressed), app);
    gtk_widget_add_controller(GTK_WIDGET(app->window), keyboard);

    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *brand = label_new("StartApp  /  Карта проекта", "title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), brand);
    GtkWidget *open = button_new("Открыть проект", G_CALLBACK(choose_folder), app);
    gtk_widget_add_css_class(open, "suggested-action");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), open);
    app->scan_button = button_new("Обновить карту", G_CALLBACK(scan_clicked), app);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), app->scan_button);
    gtk_window_set_titlebar(app->window, header);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(app->window, body);
    GtkWidget *outer = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(outer, TRUE);
    gtk_box_append(GTK_BOX(body), outer);
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 240, -1);
    gtk_box_append(GTK_BOX(sidebar), label_new("ПРОЕКТ", "section"));
    app->project_name = GTK_LABEL(label_new("Папка не выбрана", "title"));
    gtk_label_set_ellipsize(app->project_name, PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(app->project_name));
    app->summary = GTK_LABEL(label_new("Выберите папку с исходниками.\nФайлы сборки также доступны\nдля просмотра и редактирования.", "muted"));
    gtk_label_set_wrap(app->summary, TRUE);
    gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(app->summary));
    gtk_box_append(GTK_BOX(sidebar), label_new("ФАЙЛЫ", "section"));
    app->search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(app->search, "Поиск файлов · Ctrl+P");
    gtk_search_entry_set_search_delay(app->search, 150);
    g_signal_connect(app->search, "search-changed", G_CALLBACK(search_changed), app);
    g_signal_connect(app->search, "stop-search", G_CALLBACK(search_stopped), app);
    g_signal_connect(app->search, "activate", G_CALLBACK(search_activate), app);
    gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(app->search));
    app->search_result = GTK_LABEL(label_new("", "muted"));
    gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(app->search_result));
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
    gtk_box_append(GTK_BOX(sidebar), scroll(GTK_WIDGET(app->files)));
    gtk_paned_set_start_child(GTK_PANED(outer), sidebar);
    gtk_paned_set_resize_start_child(GTK_PANED(outer), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(outer), FALSE);
    gtk_paned_set_position(GTK_PANED(outer), 250);

    GtkWidget *workspace = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
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
    app->editor = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_monospace(app->editor, TRUE);
    gtk_text_view_set_editable(app->editor, FALSE);
    gtk_text_view_set_left_margin(app->editor, 16);
    gtk_text_view_set_right_margin(app->editor, 16);
    gtk_text_view_set_top_margin(app->editor, 16);
    app->buffer = gtk_text_view_get_buffer(app->editor);
    gtk_text_buffer_set_enable_undo(app->buffer, TRUE);
    gtk_text_buffer_create_tag(app->buffer, "keyword", "foreground", "#c6a0f6", NULL);
    gtk_text_buffer_create_tag(app->buffer, "directive", "foreground", "#8aadf4", NULL);
    gtk_text_buffer_create_tag(app->buffer, "string", "foreground", "#a6da95", NULL);
    gtk_text_buffer_create_tag(app->buffer, "comment", "foreground", "#7e91a8", NULL);
    g_signal_connect(app->buffer, "changed", G_CALLBACK(buffer_changed), app);
    g_signal_connect(app->buffer, "modified-changed", G_CALLBACK(buffer_changed), app);
    g_signal_connect(app->buffer, "mark-set", G_CALLBACK(cursor_moved), app);
    GtkWidget *editor_scroll = scroll(GTK_WIDGET(app->editor));
    editor_attach_gutter(app->editor, GTK_SCROLLED_WINDOW(editor_scroll));
    gtk_box_append(GTK_BOX(editor_box), editor_scroll);
    gtk_paned_set_start_child(GTK_PANED(workspace), editor_box);

    GtkWidget *graph_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *graph_toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(graph_toolbar, "toolbar");
    GtkWidget *graph_title = label_new("КАРТА ЗАВИСИМОСТЕЙ", "section");
    gtk_widget_set_hexpand(graph_title, TRUE);
    gtk_box_append(GTK_BOX(graph_toolbar), graph_title);
    GtkWidget *overview = gtk_toggle_button_new_with_label("Весь проект");
    g_signal_connect(overview, "toggled", G_CALLBACK(overview_toggled), app);
    gtk_box_append(GTK_BOX(graph_toolbar), overview);
    gtk_box_append(GTK_BOX(graph_box), graph_toolbar);
    app->graph = graph_new(graph_open, app);
    gtk_box_append(GTK_BOX(graph_box), scroll(graph_widget(app->graph)));
    app->details = GTK_LABEL(label_new("Нажмите на узел, чтобы открыть файл.\nСтрелка: исходник → включаемый заголовок.", "details"));
    gtk_label_set_wrap(app->details, TRUE);
    gtk_label_set_selectable(app->details, TRUE);
    g_signal_connect(app->details, "activate-link", G_CALLBACK(details_link), app);
    GtkWidget *details_scroll = scroll(GTK_WIDGET(app->details));
    gtk_widget_set_vexpand(details_scroll, FALSE);
    gtk_widget_set_size_request(details_scroll, -1, 140);
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
    if (app->initial_path) scan_project(app, app->initial_path, TRUE);
    if (app->smoke) g_timeout_add(350, smoke_tick, app);
}

int app_run(const char *project_path, gboolean smoke_test)
{
    App app = {0};
    app.smoke = smoke_test;
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
    app.application = gtk_application_new("io.github.PaperGooseRus.StartApp", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
    int result = g_application_run(G_APPLICATION(app.application), 0, NULL);
    if (app.highlight_source) g_source_remove(app.highlight_source);
    g_object_unref(app.application);
    document_free(app.document);
    project_free(app.project);
    if (app.graph) graph_free(app.graph);
    g_free(app.selected);
    g_free(app.pending_path);
    g_free(app.initial_path);
    if (app.smoke_root) {
        g_autofree char *module = g_build_filename(app.smoke_root, "src", "МОДУЛЬ.c", NULL);
        g_autofree char *src = g_build_filename(app.smoke_root, "src", NULL);
        g_unlink(module);
        g_rmdir(src);
        GDir *directory = g_dir_open(app.smoke_root, 0, NULL);
        const char *name;
        while (directory && (name = g_dir_read_name(directory))) {
            g_autofree char *path = g_build_filename(app.smoke_root, name, NULL);
            g_unlink(path);
        }
        if (directory) g_dir_close(directory);
        g_rmdir(app.smoke_root);
        g_free(app.smoke_root);
    }
    return app.smoke_result ? app.smoke_result : result;
}
