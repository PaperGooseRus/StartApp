#include "app.h"
#include "document.h"
#include "editor.h"
#include "graph.h"
#include "theme.h"
#include <glib/gstdio.h>
#include <math.h>

typedef enum { ACTION_NONE, ACTION_FILE, ACTION_LOCATION, ACTION_PROJECT, ACTION_CLOSE, ACTION_RELOAD } Action;

typedef struct {
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
    gboolean smoke_link_pending;
    gint smoke_result;
    char *smoke_root;
    char *initial_path;
} App;

typedef struct { char *path, *build_directory, *configuration; gboolean replace; } ScanRequest;

static void scan_project(App *app, const char *path, gboolean replace);
static gboolean save_document(App *app, gboolean rescan);
static void request_action(App *app, Action action, const char *path);
static void request_action_at(App *app, Action action, const char *path, guint line);
static void cursor_moved(GtkTextBuffer *buffer, const GtkTextIter *location,
                         GtkTextMark *mark, gpointer user_data);
static void select_current_file(App *app);
static void graph_target_selected(guint index, gpointer data);

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
    if (gtk_toggle_button_get_active(app->target_mode)) {
        GString *details = g_string_new("<b>CMake · граф целей сборки</b>\n");
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
    editor_set_language(app->editor, document->path);
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
    g_free(request->build_directory);
    g_free(request->configuration);
    g_free(request);
}

static void scan_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)cancel;
    ScanRequest *request = data;
    g_autoptr(GError) error = NULL;
    Project *project = project_scan_build(request->path, request->build_directory, request->configuration, &error);
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
    gtk_widget_set_sensitive(app->metadata_button, project != NULL || app->project != NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(app->target_mode), TRUE);
    if (!project) {
        gtk_widget_set_sensitive(GTK_WIDGET(app->configuration_picker), app->project && app->project->configurations->len > 1);
        gtk_widget_set_sensitive(GTK_WIDGET(app->target_picker), app->project && app->project->targets->len > 0);
        show_error(app, "Не удалось прочитать проект", error->message);
        update_document_title(app);
        g_application_release(G_APPLICATION(app->application));
        return;
    }
    graph_set_project(app->graph, NULL, -1);
    project_free(app->project);
    app->project = project;
    app->selected_target = -1;
    if (request->replace) {
        g_clear_pointer(&app->build_directory, g_free);
        g_clear_pointer(&app->configuration, g_free);
        app->build_directory = g_strdup(request->build_directory);
        app->configuration = g_strdup(request->configuration);
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
    g_autofree char *chain = project->generator ? g_strdup_printf("CMake → %s → %s\n%s", project->generator,
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
    app->updating_metadata = FALSE;
    populate_tree(app);
    gint selected = app->selected ? project_find_file(project, app->selected) : -1;
    graph_set_project(app->graph, project, selected);
    if (request->replace) {
        guint sources = 0;
        for (guint i = 0; i < project->files->len; i++) sources += ((ProjectFile *)g_ptr_array_index(project->files, i))->source;
        gtk_toggle_button_set_active(app->groups, sources > 30 || project->targets->len > 30);
    }
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
    gtk_widget_set_sensitive(app->metadata_button, FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->configuration_picker), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->target_picker), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->target_mode), FALSE);
    update_document_title(app);
    status(app, "Чтение проекта…");
    ScanRequest *request = g_new0(ScanRequest, 1);
    request->path = g_strdup(path);
    request->replace = replace;
    if (!replace || !app->project) {
        request->build_directory = g_strdup(app->build_directory);
        request->configuration = g_strdup(app->configuration);
    }
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
    if (folder && !app->closing) {
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
    if (app->busy || app->confirming || (build && !app->project)) return;
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
    if (app->updating_metadata || app->busy || !app->project) return;
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
    if (!app->project || app->busy || index >= app->project->targets->len) return;
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
    if (app->updating_metadata || !app->project || app->busy) return;
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
    if (app->confirming && app->smoke_stage != 10 && app->smoke_stage != 11 && app->smoke_stage != 19)
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
    if (!take_screenshot(app)) goto failed;
    GtkWidget *controls[] = {app->scan_button, app->save_button, GTK_WIDGET(app->target_mode), GTK_WIDGET(app->position)};
    for (guint i = 0; i < G_N_ELEMENTS(controls); i++) {
        graphene_rect_t bounds;
        if (!gtk_widget_compute_bounds(controls[i], GTK_WIDGET(app->window), &bounds) ||
            bounds.origin.x < 0 || bounds.origin.x + bounds.size.width > gtk_widget_get_width(GTK_WIDGET(app->window)) + 1)
            goto failed;
    }
    g_print("UI smoke PASS: syntax and multiline contexts, editing, navigation, build passport, Debug/Release, layered graph, zoom/pan/groups, native decorations\n");
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
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(header, "toolbar");
    GtkWidget *brand = label_new("StartApp", "title");
    gtk_widget_add_css_class(brand, "brand");
    gtk_box_append(GTK_BOX(header), brand);
    GtkWidget *open = button_new("Открыть проект", G_CALLBACK(choose_folder), app);
    gtk_widget_add_css_class(open, "suggested-action");
    gtk_box_append(GTK_BOX(header), open);
    GtkWidget *subtitle = label_new("Карта проекта", "muted");
    gtk_widget_set_hexpand(subtitle, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(header), subtitle);
    app->scan_button = button_new("Обновить карту", G_CALLBACK(scan_clicked), app);
    gtk_box_append(GTK_BOX(header), app->scan_button);
    gtk_box_append(GTK_BOX(body), header);
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
    gtk_label_set_max_width_chars(app->summary, 28);
    gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(app->summary));
    app->build_chain = GTK_LABEL(label_new("Сборка ещё не определена", "muted"));
    gtk_label_set_wrap(app->build_chain, TRUE);
    gtk_label_set_max_width_chars(app->build_chain, 28);
    gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(app->build_chain));
    app->sidebar_pages = GTK_STACK(gtk_stack_new());
    gtk_widget_set_vexpand(GTK_WIDGET(app->sidebar_pages), TRUE);
    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), app->sidebar_pages);
    gtk_box_append(GTK_BOX(sidebar), switcher);
    gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(app->sidebar_pages));
    GtkWidget *file_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *build_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_stack_add_titled(app->sidebar_pages, file_panel, "files", "Файлы");
    gtk_stack_add_titled(app->sidebar_pages, build_panel, "build", "Сборка");
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
    app->build_profile = GTK_LABEL(label_new("Откройте проект для просмотра параметров сборки.", "muted"));
    gtk_label_set_wrap(app->build_profile, TRUE);
    gtk_label_set_wrap_mode(app->build_profile, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(app->build_profile, 28);
    gtk_label_set_selectable(app->build_profile, TRUE);
    gtk_box_append(GTK_BOX(build_panel), scroll(GTK_WIDGET(app->build_profile)));
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

int app_run(const char *project_path, const char *build_directory, const char *configuration, gboolean smoke_test)
{
    App app = {0};
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
    app.application = gtk_application_new("io.github.PaperGooseRus.StartApp", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
    int result = g_application_run(G_APPLICATION(app.application), 0, NULL);
    g_object_unref(app.application);
    document_free(app.document);
    project_free(app.project);
    if (app.graph) graph_free(app.graph);
    g_free(app.selected);
    g_free(app.pending_path);
    g_free(app.initial_path);
    g_free(app.build_directory);
    g_free(app.configuration);
    if (app.smoke_root) {
        smoke_remove_tree(app.smoke_root);
        g_free(app.smoke_root);
    }
    return app.smoke_result ? app.smoke_result : result;
}
