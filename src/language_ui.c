#include "language_ui.h"
#include "language_client.h"
#include <string.h>

struct LanguageUI {
    gint refs;
    gboolean closed, busy;
    GtkApplication *application;
    GtkWindow *window;
    GtkTextView *editor;
    GtkTextBuffer *observed;
    GtkWidget *box;
    GtkEntry *tool;
    GtkToggleButton *enabled;
    GtkTextTag *error_tag, *warning_tag;
    GtkPopover *completion;
    char *root, *build, *file, *snapshot;
    guint epoch, timer, diagnostic_count, diagnostic_generation, smoke_generation;
    LanguageClient *client;
    LanguageNavigate navigate;
    LanguageStatus status;
    gpointer data;
};
typedef struct { LanguageUI *ui; char *text, *path, *method; guint epoch; } Response;
static void unref(LanguageUI *u)
{
    if (!g_atomic_int_dec_and_test(&u->refs)) return;
    language_client_unref(u->client); g_free(u->root); g_free(u->build); g_free(u->file); g_free(u->snapshot); g_clear_object(&u->observed); g_object_unref(u->editor); g_free(u);
}
static char *buffer_text(LanguageUI *u)
{ GtkTextBuffer *b = gtk_text_view_get_buffer(u->editor); GtkTextIter a, z; gtk_text_buffer_get_bounds(b, &a, &z); return gtk_text_buffer_get_text(b, &a, &z, FALSE); }
static gboolean position(GtkTextBuffer *b, JsonNode *point, GtkTextIter *out)
{
    gint64 row = language_integer(language_member(point, "line"), -1), column = language_integer(language_member(point, "character"), -1);
    if (row < 0 || row >= gtk_text_buffer_get_line_count(b) || column < 0 || column > G_MAXINT) return FALSE;
    gtk_text_buffer_get_iter_at_line(b, out, (gint)row); GtkTextIter end = *out; gtk_text_iter_forward_to_line_end(&end);
    g_autofree char *line = gtk_text_buffer_get_text(b, out, &end, FALSE); guint chars = language_char_offset(line, (guint)column);
    if (language_utf16_offset(line, chars) != column) return FALSE;
    gtk_text_iter_forward_chars(out, (gint)chars); return TRUE;
}
static void diagnostic_tags(LanguageUI *u, JsonNode *result)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(u->editor); GtkTextIter a, z; gtk_text_buffer_get_bounds(b, &a, &z);
    gtk_text_buffer_remove_tag(b, u->error_tag, &a, &z); gtk_text_buffer_remove_tag(b, u->warning_tag, &a, &z);
    JsonNode *node = language_member(result, "diagnostics"); JsonArray *list = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
    GString *report = g_string_new(NULL); guint count = list ? json_array_get_length(list) : 0;
    u->diagnostic_count = count; u->diagnostic_generation++;
    for (guint i = 0; i < MIN(count, 200u); i++) {
        JsonNode *d = json_array_get_element(list, i), *range = language_member(d, "range");
        if (!position(b, language_member(range, "start"), &a) || !position(b, language_member(range, "end"), &z) || gtk_text_iter_compare(&a, &z) > 0) continue;
        if (gtk_text_iter_equal(&a, &z)) gtk_text_iter_forward_char(&z);
        gtk_text_buffer_apply_tag(b, language_integer(language_member(d, "severity"), 1) == 1 ? u->error_tag : u->warning_tag, &a, &z);
        const char *message = language_string(language_member(d, "message"));
        if (i < 30) g_string_append_printf(report, "%d:%d · %s\n", gtk_text_iter_get_line(&a)+1, gtk_text_iter_get_line_offset(&a)+1, message ? message : "");
    }
    g_autofree char *status = g_strdup_printf("clangd · %u сообщений%s%s", count, report->len ? " · " : "", report->len ? report->str : "");
    u->status(status, u->data); gtk_widget_set_tooltip_text(GTK_WIDGET(u->editor), report->len ? report->str : NULL); g_string_free(report, TRUE);
}
static void complete_item(GtkButton *button, gpointer data)
{
    LanguageUI *u = data; g_autofree char *current = buffer_text(u);
    if (g_strcmp0(current, u->snapshot)) { gtk_popover_popdown(u->completion); u->status("Текст изменился · запросите дополнение повторно", u->data); return; }
    JsonNode *item = g_object_get_data(G_OBJECT(button), "item"); JsonNode *edit = language_member(item, "textEdit"), *range = language_member(edit, "range");
    if (!range) range = language_member(edit, "replace");
    GtkTextBuffer *b = gtk_text_view_get_buffer(u->editor); GtkTextIter a, z;
    const char *text = language_string(language_member(edit, "newText"));
    if (range) { if (!position(b, language_member(range, "start"), &a) || !position(b, language_member(range, "end"), &z) || gtk_text_iter_compare(&a, &z) > 0) return; }
    else {
        gtk_text_buffer_get_iter_at_mark(b, &z, gtk_text_buffer_get_insert(b)); a = z;
        while (!gtk_text_iter_is_start(&a)) { GtkTextIter prev = a; gtk_text_iter_backward_char(&prev); gunichar c = gtk_text_iter_get_char(&prev); if (!(g_unichar_isalnum(c) || c == '_')) break; a = prev; }
    }
    if (!text) text = language_string(language_member(item, "insertText"));
    if (!text) text = language_string(language_member(item, "label"));
    if (text) { gtk_text_buffer_begin_user_action(b); gtk_text_buffer_delete(b, &a, &z); gtk_text_buffer_insert(b, &a, text, -1); gtk_text_buffer_end_user_action(b); }
    gtk_popover_popdown(u->completion);
}
static void completions(LanguageUI *u, JsonNode *result)
{
    JsonNode *items = language_member(result, "items"); if (!items) items = result;
    JsonArray *list = items && JSON_NODE_HOLDS_ARRAY(items) ? json_node_get_array(items) : NULL;
    if (u->completion) { gtk_widget_unparent(GTK_WIDGET(u->completion)); u->completion = NULL; }
    u->completion = GTK_POPOVER(gtk_popover_new()); gtk_widget_set_parent(GTK_WIDGET(u->completion), GTK_WIDGET(u->editor));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box); gtk_widget_set_size_request(scroll, 360, 260); gtk_popover_set_child(u->completion, scroll);
    guint shown = 0;
    for (guint i = 0; list && i < json_array_get_length(list) && shown < 100; i++) {
        JsonNode *item = json_array_get_element(list, i); const char *label = language_string(language_member(item, "label"));
        JsonNode *extra = language_member(item, "additionalTextEdits");
        if (!label || language_integer(language_member(item, "insertTextFormat"), 1) == 2 || (extra && JSON_NODE_HOLDS_ARRAY(extra) && json_array_get_length(json_node_get_array(extra)))) continue;
        GtkWidget *b = gtk_button_new_with_label(label); g_object_set_data_full(G_OBJECT(b), "item", json_node_copy(item), (GDestroyNotify)json_node_free);
        g_signal_connect(b, "clicked", G_CALLBACK(complete_item), u); gtk_box_append(GTK_BOX(box), b); shown++;
    }
    if (!shown) { u->status("clangd · вариантов дополнения нет", u->data); return; }
    GtkTextIter at; GtkTextBuffer *b = gtk_text_view_get_buffer(u->editor); gtk_text_buffer_get_iter_at_mark(b, &at, gtk_text_buffer_get_insert(b)); GdkRectangle rect;
    gtk_text_view_get_iter_location(u->editor, &at, &rect); gtk_text_view_buffer_to_window_coords(u->editor, GTK_TEXT_WINDOW_WIDGET, rect.x, rect.y, &rect.x, &rect.y);
    gtk_popover_set_pointing_to(u->completion, &rect); gtk_popover_popup(u->completion);
    g_autofree char *message = g_strdup_printf("clangd · %u вариантов дополнения", shown); u->status(message, u->data);
}
static void response(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; Response *r = data; LanguageUI *u = r->ui; u->busy = FALSE;
    g_autoptr(GError) error = NULL; g_autoptr(JsonNode) value = language_client_finish(result, &error);
    if (!u->closed && r->epoch == u->epoch) {
        g_autofree char *text = buffer_text(u);
        if (error) { u->status(error->message, u->data); language_client_stop(u->client); language_client_unref(u->client); u->client = NULL; }
        else if (g_strcmp0(text, r->text) == 0 && g_strcmp0(r->path, u->file) == 0) {
            if (g_str_equal(r->method, "diagnostics")) diagnostic_tags(u, value);
            else if (g_str_equal(r->method, "textDocument/completion")) { g_free(u->snapshot); u->snapshot = g_strdup(text); completions(u, value); }
            else {
                JsonNode *location = value;
                if (JSON_NODE_HOLDS_ARRAY(value)) { JsonArray *a = json_node_get_array(value); location = json_array_get_length(a) ? json_array_get_element(a, 0) : NULL; }
                const char *uri = language_string(language_member(location, "uri")); JsonNode *range = language_member(location, "range");
                if (!uri) { uri = language_string(language_member(location, "targetUri")); range = language_member(location, "targetSelectionRange"); }
                if (uri) {
                    g_autoptr(GFile) file = g_file_new_for_uri(uri), root = g_file_new_for_path(u->root); g_autofree char *path = g_file_get_relative_path(root, file);
                    if (path) {
                        guint line = (guint)CLAMP(language_integer(language_member(language_member(range, "start"), "line"), 0), 0, G_MAXINT-1)+1;
                        g_autofree char *message = g_strdup_printf("clangd · определение %s:%u", path, line); u->status(message, u->data); u->navigate(path, line, u->data);
                    }
                    else u->status("Определение находится вне проекта", u->data);
                } else u->status("clangd · определение не найдено", u->data);
            }
        } else u->status("Текст изменился · устаревший ответ clangd пропущен", u->data);
    }
    g_application_release(G_APPLICATION(u->application)); g_free(r->text); g_free(r->path); g_free(r->method); g_free(r); unref(u);
}
void language_ui_action(LanguageUI *u, const char *method)
{
    if (u->closed || u->busy || !u->root || !u->file || !gtk_toggle_button_get_active(u->enabled)) return;
    if (!(g_str_has_suffix(u->file, ".c") || g_str_has_suffix(u->file, ".h") || g_str_has_suffix(u->file, ".cpp") || g_str_has_suffix(u->file, ".cc") || g_str_has_suffix(u->file, ".hpp") || g_str_has_suffix(u->file, ".cxx") || g_str_has_suffix(u->file, ".hxx"))) return;
    if (!u->client) u->client = language_client_new(gtk_editable_get_text(GTK_EDITABLE(u->tool)), u->root, u->build);
    Response *r = g_new0(Response, 1); r->ui = u; r->text = buffer_text(u); r->path = g_strdup(u->file); r->method = g_strdup(method); r->epoch = u->epoch;
    GtkTextBuffer *b = gtk_text_view_get_buffer(u->editor); GtkTextIter at, start;
    gtk_text_buffer_get_iter_at_mark(b, &at, gtk_text_buffer_get_insert(b)); start = at; gtk_text_iter_set_line_offset(&start, 0);
    g_autofree char *prefix = gtk_text_buffer_get_text(b, &start, &at, FALSE); g_autofree char *path = g_build_filename(u->root, u->file, NULL);
    if (language_client_request(u->client, path, r->text, gtk_text_iter_get_line(&at), language_utf16_offset(prefix, (guint)g_utf8_strlen(prefix, -1)), method, response, r)) {
        u->busy = TRUE; g_atomic_int_inc(&u->refs); g_application_hold(G_APPLICATION(u->application)); u->status("clangd · запрос…", u->data);
    } else { g_free(r->text); g_free(r->path); g_free(r->method); g_free(r); }
}
static gboolean delayed(gpointer data)
{ LanguageUI *u = data; if (u->busy) return G_SOURCE_CONTINUE; u->timer = 0; language_ui_action(u, "diagnostics"); return G_SOURCE_REMOVE; }
static void changed(GtkTextBuffer *buffer, gpointer data)
{
    (void)buffer; LanguageUI *u = data;
    if (!u->closed && gtk_toggle_button_get_active(u->enabled)) { if (u->timer) g_source_remove(u->timer); u->timer = g_timeout_add(800, delayed, u); }
}
static void reset(LanguageUI *u)
{ if (u->completion) { gtk_widget_unparent(GTK_WIDGET(u->completion)); u->completion = NULL; } u->epoch++; language_client_stop(u->client); language_client_unref(u->client); u->client = NULL; }
static void enabled(GtkToggleButton *b, gpointer data)
{
    LanguageUI *u = data; reset(u);
    if (gtk_toggle_button_get_active(b)) changed(NULL, u);
    else { GtkTextBuffer *buffer = gtk_text_view_get_buffer(u->editor); GtkTextIter a, z; gtk_text_buffer_get_bounds(buffer, &a, &z); gtk_text_buffer_remove_tag(buffer, u->error_tag, &a, &z); gtk_text_buffer_remove_tag(buffer, u->warning_tag, &a, &z); gtk_widget_set_tooltip_text(GTK_WIDGET(u->editor), NULL); }
}
static void tool_changed(GtkEditable *e, gpointer data) { (void)e; reset(data); }
static void action_clicked(GtkButton *b, gpointer data)
{ language_ui_action(data, g_object_get_data(G_OBJECT(b), "method")); }
LanguageUI *language_ui_new(GtkApplication *application, GtkWindow *window, GtkTextView *editor, LanguageNavigate navigate, LanguageStatus status, gpointer data)
{
    LanguageUI *u = g_new0(LanguageUI, 1); u->refs = 1; u->application = application; u->window = window; u->editor = g_object_ref(editor); u->navigate = navigate; u->status = status; u->data = data;
    u->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4); u->enabled = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("clangd"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(u->enabled), "Включить диагностику C/C++, F12 и Ctrl+Space. Требуется установленный clangd."); gtk_box_append(GTK_BOX(u->box), GTK_WIDGET(u->enabled));
    u->tool = GTK_ENTRY(gtk_entry_new()); gtk_editable_set_text(GTK_EDITABLE(u->tool), "clangd"); gtk_widget_set_size_request(GTK_WIDGET(u->tool), 280, -1); gtk_entry_set_placeholder_text(u->tool, "Путь к clangd");
    GtkWidget *settings = gtk_menu_button_new(); gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(settings), "emblem-system-symbolic"); gtk_widget_set_tooltip_text(settings, "Путь к clangd");
    GtkWidget *popover = gtk_popover_new(); gtk_popover_set_child(GTK_POPOVER(popover), GTK_WIDGET(u->tool)); gtk_menu_button_set_popover(GTK_MENU_BUTTON(settings), popover); gtk_box_append(GTK_BOX(u->box), settings);
    const char *labels[] = {"F12", "Дополнить", "Проверить"}, *methods[] = {"textDocument/definition", "textDocument/completion", "diagnostics"};
    for (guint i = 0; i < 3; i++) { GtkWidget *b = gtk_button_new_with_label(labels[i]); g_object_set_data(G_OBJECT(b), "method", (gpointer)methods[i]); g_signal_connect(b, "clicked", G_CALLBACK(action_clicked), u); gtk_box_append(GTK_BOX(u->box), b); }
    GtkTextBuffer *b = gtk_text_view_get_buffer(editor); u->observed = g_object_ref(b);
    u->error_tag = gtk_text_buffer_create_tag(b, "clangd-error", "underline", PANGO_UNDERLINE_ERROR, "underline-rgba", &(GdkRGBA){.9,.4,.3,1}, NULL);
    u->warning_tag = gtk_text_buffer_create_tag(b, "clangd-warning", "underline", PANGO_UNDERLINE_SINGLE, NULL);
    g_signal_connect(b, "changed", G_CALLBACK(changed), u); g_signal_connect(u->enabled, "toggled", G_CALLBACK(enabled), u); g_signal_connect(u->tool, "changed", G_CALLBACK(tool_changed), u); return u;
}
GtkWidget *language_ui_widget(LanguageUI *u) { return u->box; }
void language_ui_context(LanguageUI *u, const char *root, const char *build, const char *file)
{
    GtkTextBuffer *current = gtk_text_view_get_buffer(u->editor);
    if (current != u->observed) {
        g_signal_handlers_disconnect_by_data(u->observed, u); g_set_object(&u->observed, current);
        GtkTextTagTable *table = gtk_text_buffer_get_tag_table(current);
        u->error_tag = gtk_text_tag_table_lookup(table, "clangd-error");
        if (!u->error_tag) u->error_tag = gtk_text_buffer_create_tag(current, "clangd-error", "underline", PANGO_UNDERLINE_ERROR, NULL);
        u->warning_tag = gtk_text_tag_table_lookup(table, "clangd-warning");
        if (!u->warning_tag) u->warning_tag = gtk_text_buffer_create_tag(current, "clangd-warning", "underline", PANGO_UNDERLINE_SINGLE, NULL);
        g_signal_connect(current, "changed", G_CALLBACK(changed), u);
    }
    if (g_strcmp0(u->root, root) || g_strcmp0(u->build, build) || g_strcmp0(u->file, file)) reset(u);
    g_free(u->root); g_free(u->build); g_free(u->file); u->root = g_strdup(root); u->build = g_strdup(build); u->file = g_strdup(file);
    GtkTextBuffer *b = gtk_text_view_get_buffer(u->editor); GtkTextIter a, z; gtk_text_buffer_get_bounds(b, &a, &z); gtk_text_buffer_remove_tag(b, u->error_tag, &a, &z); gtk_text_buffer_remove_tag(b, u->warning_tag, &a, &z); changed(b, u);
}
void language_ui_free(LanguageUI *u)
{
    if (!u) return;
    u->closed = TRUE; if (u->timer) { g_source_remove(u->timer); u->timer = 0; }
    if (u->completion) gtk_widget_unparent(GTK_WIDGET(u->completion));
    g_signal_handlers_disconnect_by_data(u->observed, u);
    language_client_stop(u->client); unref(u);
}

/* Called only by the owned GUI smoke fixture. Portable runs skip absent clangd. */
gint language_ui_smoke_step(LanguageUI *u, guint step)
{
    g_autofree char *program = g_find_program_in_path("clangd");
    if (!program) return 1;
    GtkTextBuffer *b = gtk_text_view_get_buffer(u->editor);
    if (step == 0) {
        gtk_text_buffer_set_text(b, "int sample_function(void) { return 42; }\nint main(void) { return broken_token; }\n", -1);
        u->smoke_generation = u->diagnostic_generation;
        gtk_toggle_button_set_active(u->enabled, TRUE); language_ui_action(u, "diagnostics"); return 1;
    }
    if (u->busy) return 0;
    if (step == 1) {
        if (u->diagnostic_generation == u->smoke_generation) return -1;
        if (!u->diagnostic_count) return -1;
        gtk_text_buffer_set_text(b, "int sample_function(void) { return 42; }\nint main(void) { return sam }\n", -1);
        GtkTextIter cursor; gtk_text_buffer_get_iter_at_line_offset(b, &cursor, 1, 27); gtk_text_buffer_place_cursor(b, &cursor);
        language_ui_action(u, "textDocument/completion"); return 1;
    }
    if (step == 2) {
        if (!u->completion) { g_printerr("clangd GUI: no completion popup\n"); return -1; }
        GtkWidget *scroll = gtk_popover_get_child(u->completion), *box = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroll));
        if (GTK_IS_VIEWPORT(box)) box = gtk_viewport_get_child(GTK_VIEWPORT(box));
        GtkWidget *found = NULL;
        for (GtkWidget *item = gtk_widget_get_first_child(box); item; item = gtk_widget_get_next_sibling(item)) {
            if (GTK_IS_BUTTON(item) && strstr(gtk_button_get_label(GTK_BUTTON(item)), "sample_function")) { found = item; break; }
        }
        if (!found) { g_printerr("clangd GUI: function missing from completion list\n"); return -1; }
        g_signal_emit_by_name(found, "clicked"); g_autofree char *text = buffer_text(u);
        if (!strstr(text, "return sample_function")) { g_autofree char *item = json_to_string(g_object_get_data(G_OBJECT(found), "item"), FALSE); g_printerr("clangd GUI: text=%s item=%s\n", text, item); return -1; }
        GtkTextIter cursor; gtk_text_buffer_get_iter_at_line_offset(b, &cursor, 1, 25); gtk_text_buffer_place_cursor(b, &cursor);
        language_ui_action(u, "textDocument/definition"); return 1;
    }
    if (step == 3) {
        GtkTextIter cursor; gtk_text_buffer_get_iter_at_mark(b, &cursor, gtk_text_buffer_get_insert(b));
        if (gtk_text_iter_get_line(&cursor) != 0) return -1;
        gtk_toggle_button_set_active(u->enabled, FALSE); return 1;
    }
    return -1;
}
