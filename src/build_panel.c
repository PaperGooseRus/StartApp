#include "build_panel.h"

struct BuildPanel {
    GtkWidget *box, *stop;
    GtkTextView *log;
    GtkLabel *result, *diagnostic_title;
    GtkListBox *diagnostics_list;
    GPtrArray *diagnostics;
    BuildRun *run;
    BuildPlan *plan;
    const Project *project;
    BuildNavigate navigate;
    BuildFinished finished;
    gpointer data;
    gint exit_code;
    gboolean stopped;
};

static void append_log(BuildPanel *panel, const char *line)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(panel->log);
    GtkWidget *scroll = gtk_widget_get_ancestor(GTK_WIDGET(panel->log), GTK_TYPE_SCROLLED_WINDOW);
    GtkAdjustment *adjustment = scroll ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)) : NULL;
    gboolean follow = !adjustment || gtk_adjustment_get_value(adjustment) >=
        gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment) - 32;
    GtkTextIter end; gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, line, -1); gtk_text_buffer_insert(buffer, &end, "\n", 1);
    gint count = gtk_text_buffer_get_char_count(buffer);
    if (count > 262144) {
        GtkTextIter first, cutoff; gtk_text_buffer_get_start_iter(buffer, &first);
        gtk_text_buffer_get_iter_at_offset(buffer, &cutoff, count - 196608);
        gtk_text_buffer_delete(buffer, &first, &cutoff);
    }
    if (follow) { gtk_text_buffer_get_end_iter(buffer, &end); gtk_text_view_scroll_to_iter(panel->log, &end, 0, FALSE, 0, 0); }
}

static void output(const char *line, gpointer data)
{
    BuildPanel *panel = data; append_log(panel, line);
    if (panel->diagnostics->len >= 200) return;
    BuildDiagnostic *diagnostic = build_diagnostic_parse(line, panel->plan->directory, panel->project);
    if (!diagnostic) return;
    guint index = panel->diagnostics->len; g_ptr_array_add(panel->diagnostics, diagnostic);
    g_autofree char *text = g_strdup_printf("%s:%u%s · %s", diagnostic->file, diagnostic->line,
        diagnostic->warning ? " · предупреждение" : "", diagnostic->message);
    if (g_utf8_strlen(text, -1) > 600) *g_utf8_offset_to_pointer(text, 600) = 0;
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR); gtk_label_set_max_width_chars(GTK_LABEL(label), 80);
    gtk_widget_set_margin_start(label, 12); gtk_widget_set_margin_end(label, 12);
    gtk_widget_set_margin_top(label, 5); gtk_widget_set_margin_bottom(label, 5);
    gtk_list_box_append(panel->diagnostics_list, label);
    GtkWidget *row = gtk_widget_get_parent(label);
    g_object_set_data(G_OBJECT(row), "diagnostic-index", GUINT_TO_POINTER(index + 1));
    gtk_widget_set_tooltip_text(row, diagnostic->project_path ? "Открыть строку в редакторе" : "Файл находится за пределами карты проекта");
    g_autofree char *title = g_strdup_printf("Диагностика · %u", panel->diagnostics->len);
    gtk_label_set_text(panel->diagnostic_title, title);
}
static void finished(gint code, gboolean stopped, gpointer data)
{
    BuildPanel *panel = data;
    panel->exit_code = code; panel->stopped = stopped;
    g_autofree char *message = stopped ? g_strdup("Сборка остановлена") : code == 0 ? g_strdup("Сборка завершена успешно") :
        g_strdup_printf("Сборка завершилась с ошибкой · код %d", code);
    append_log(panel, message); gtk_label_set_text(panel->result, message);
    gtk_widget_set_sensitive(panel->stop, FALSE);
    build_run_unref(panel->run); panel->run = NULL;
    build_plan_free(panel->plan); panel->plan = NULL; panel->project = NULL;
    if (panel->finished) panel->finished(code, stopped, panel->data);
}
gboolean build_panel_open_diagnostic(BuildPanel *panel, guint index)
{
    if (index >= panel->diagnostics->len) return FALSE;
    BuildDiagnostic *diagnostic = g_ptr_array_index(panel->diagnostics, index);
    if (!diagnostic->project_path) { gtk_label_set_text(panel->result, "Файл не входит в карту проекта"); return FALSE; }
    if (panel->navigate) panel->navigate(diagnostic->project_path, diagnostic->line, panel->data);
    return TRUE;
}
static void activated(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    (void)list; guint encoded = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "diagnostic-index"));
    if (encoded) build_panel_open_diagnostic(data, encoded - 1);
}
void build_panel_stop(BuildPanel *panel)
{ if (panel->run) { gtk_label_set_text(panel->result, "Остановка сборки…"); build_run_stop(panel->run); } }
static void stop_clicked(GtkButton *button, gpointer data) { (void)button; build_panel_stop(data); }
static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), child); gtk_widget_set_vexpand(scroll, TRUE); return scroll;
}
BuildPanel *build_panel_new(BuildNavigate navigate, BuildFinished callback, gpointer data)
{
    BuildPanel *panel = g_new0(BuildPanel, 1); panel->navigate = navigate; panel->finished = callback; panel->data = data;
    panel->diagnostics = g_ptr_array_new_with_free_func(build_diagnostic_free); panel->exit_code = -1;
    panel->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_widget_set_size_request(panel->box, -1, 110);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_widget_add_css_class(toolbar, "toolbar");
    panel->result = GTK_LABEL(gtk_label_new("Сборка ещё не запускалась"));
    gtk_label_set_xalign(panel->result, 0); gtk_label_set_ellipsize(panel->result, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(panel->result), TRUE); gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(panel->result));
    panel->stop = gtk_button_new_with_label("Остановить"); gtk_widget_set_sensitive(panel->stop, FALSE);
    g_signal_connect(panel->stop, "clicked", G_CALLBACK(stop_clicked), panel); gtk_box_append(GTK_BOX(toolbar), panel->stop);
    gtk_box_append(GTK_BOX(panel->box), toolbar);
    GtkWidget *notebook = gtk_notebook_new(); gtk_widget_set_vexpand(notebook, TRUE);
    panel->log = GTK_TEXT_VIEW(gtk_text_view_new()); gtk_text_view_set_monospace(panel->log, TRUE);
    gtk_text_view_set_editable(panel->log, FALSE); gtk_text_view_set_left_margin(panel->log, 12); gtk_text_view_set_right_margin(panel->log, 12);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled(GTK_WIDGET(panel->log)), gtk_label_new("Журнал"));
    panel->diagnostics_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_activate_on_single_click(panel->diagnostics_list, TRUE);
    g_signal_connect(panel->diagnostics_list, "row-activated", G_CALLBACK(activated), panel);
    panel->diagnostic_title = GTK_LABEL(gtk_label_new("Диагностика"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled(GTK_WIDGET(panel->diagnostics_list)), GTK_WIDGET(panel->diagnostic_title));
    gtk_box_append(GTK_BOX(panel->box), notebook); return panel;
}
GtkWidget *build_panel_widget(BuildPanel *panel) { return panel->box; }
gboolean build_panel_running(BuildPanel *panel) { return panel && panel->run != NULL; }
gint build_panel_exit_code(BuildPanel *panel) { return panel->exit_code; }
gboolean build_panel_stopped(BuildPanel *panel) { return panel->stopped; }
guint build_panel_diagnostic_count(BuildPanel *panel) { return panel->diagnostics->len; }
gboolean build_panel_start(BuildPanel *panel, const Project *project, BuildPlan *plan)
{
    if (panel->run || !project || !plan) return FALSE;
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(panel->log), "", -1);
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(panel->diagnostics_list)))) gtk_list_box_remove(panel->diagnostics_list, child);
    g_ptr_array_set_size(panel->diagnostics, 0); gtk_label_set_text(panel->diagnostic_title, "Диагностика");
    panel->project = project; panel->plan = plan; panel->exit_code = -1; panel->stopped = FALSE;
    gtk_label_set_text(panel->result, "Сборка выполняется…"); gtk_widget_set_sensitive(panel->stop, TRUE);
    append_log(panel, plan->display);
    g_autofree char *directory = g_strdup_printf("Рабочий каталог: %s", plan->directory); append_log(panel, directory);
    panel->run = build_run_start((const char *const *)plan->argv, plan->directory, output, finished, panel);
    return panel->run != NULL;
}
void build_panel_free(BuildPanel *panel)
{
    if (!panel) return;
    g_assert(panel->run == NULL); g_ptr_array_unref(panel->diagnostics); build_plan_free(panel->plan); g_free(panel);
}
