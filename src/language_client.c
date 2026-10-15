#include "language_client.h"
#include <string.h>

struct LanguageClient {
    gint refs;
    GMutex mutex;
    char *tool, *root, *build, *open_uri;
    GSubprocess *process;
    GBufferedInputStream *input;
    GCancellable *cancel;
    gboolean busy, initialized;
    guint serial, version, timeout;
};
typedef struct { LanguageClient *client; char *path, *text, *method; guint line, column; } Query;
JsonNode *language_member(JsonNode *node, const char *key)
{ return node && JSON_NODE_HOLDS_OBJECT(node) ? json_object_get_member(json_node_get_object(node), key) : NULL; }
const char *language_string(JsonNode *node)
{ return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL; }
gint64 language_integer(JsonNode *node, gint64 fallback)
{ return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 ? json_node_get_int(node) : fallback; }
guint language_utf16_offset(const char *text, guint chars)
{ guint result = 0; for (const char *p = text; *p && chars; chars--, p = g_utf8_next_char(p)) result += g_utf8_get_char(p) > 0xffff ? 2 : 1; return result; }
guint language_char_offset(const char *text, guint units)
{ guint result = 0; for (const char *p = text; *p; p = g_utf8_next_char(p)) { guint n = g_utf8_get_char(p) > 0xffff ? 2 : 1; if (n > units) break; units -= n; result++; } return result; }
static LanguageClient *client_ref(LanguageClient *c) { g_atomic_int_inc(&c->refs); return c; }
LanguageClient *language_client_new(const char *tool, const char *root, const char *build)
{
    LanguageClient *c = g_new0(LanguageClient, 1); c->refs = 1; g_mutex_init(&c->mutex);
    c->tool = g_strdup(tool); c->root = g_strdup(root); c->build = g_strdup(build); c->cancel = g_cancellable_new(); return c;
}
void language_client_stop(LanguageClient *c)
{
    if (!c) return;
    g_mutex_lock(&c->mutex); g_cancellable_cancel(c->cancel);
    if (c->process) g_subprocess_force_exit(c->process);
    g_mutex_unlock(&c->mutex);
}
void language_client_unref(LanguageClient *c)
{
    if (!c || !g_atomic_int_dec_and_test(&c->refs)) return;
    language_client_stop(c); if (c->process) g_subprocess_wait(c->process, NULL, NULL);
    g_clear_object(&c->process); g_clear_object(&c->input); g_clear_object(&c->cancel);
    g_free(c->tool); g_free(c->root); g_free(c->build); g_free(c->open_uri); g_mutex_clear(&c->mutex); g_free(c);
}
static gboolean expired(gpointer data)
{ LanguageClient *c = data; c->timeout = 0; language_client_stop(c); return G_SOURCE_REMOVE; }
static void query_free(gpointer data)
{ Query *q = data; language_client_unref(q->client); g_free(q->path); g_free(q->text); g_free(q->method); g_free(q); }
static gboolean send(LanguageClient *c, const char *method, guint id, JsonNode *params, GError **error)
{
    g_autoptr(JsonBuilder) b = json_builder_new(); json_builder_begin_object(b);
    json_builder_set_member_name(b, "jsonrpc"); json_builder_add_string_value(b, "2.0");
    if (id) { json_builder_set_member_name(b, "id"); json_builder_add_int_value(b, id); }
    json_builder_set_member_name(b, "method"); json_builder_add_string_value(b, method);
    if (params) { json_builder_set_member_name(b, "params"); json_builder_add_value(b, json_node_copy(params)); }
    json_builder_end_object(b); g_autoptr(JsonNode) root = json_builder_get_root(b); g_autofree char *json = json_to_string(root, FALSE);
    g_autofree char *frame = g_strdup_printf("Content-Length: %" G_GSIZE_FORMAT "\r\n\r\n%s", strlen(json), json);
    return g_output_stream_write_all(g_subprocess_get_stdin_pipe(c->process), frame, strlen(frame), NULL, c->cancel, error);
}
static JsonNode *read_message(LanguageClient *c, GError **error)
{
    GString *header = g_string_new(NULL); gint byte;
    while (header->len < 8192) {
        byte = g_buffered_input_stream_read_byte(c->input, c->cancel, error);
        if (byte < 0) { g_string_free(header, TRUE); if (!error || !*error) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "clangd закрыл соединение"); return NULL; }
        g_string_append_c(header, (char)byte);
        if (g_str_has_suffix(header->str, "\r\n\r\n")) break;
    }
    const char *length = strstr(header->str, "Content-Length:");
    guint64 size = length ? g_ascii_strtoull(length + 15, NULL, 10) : 0;
    g_string_free(header, TRUE);
    if (!size || size > 8*1024*1024) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Некорректный размер LSP-сообщения"); return NULL; }
    g_autofree char *body = g_malloc((gsize)size + 1); gsize actual;
    if (!g_input_stream_read_all(G_INPUT_STREAM(c->input), body, (gsize)size, &actual, c->cancel, error)) return NULL;
    if (actual != size) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT, "Неполное LSP-сообщение"); return NULL; }
    body[size] = 0;
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, body, (gssize)size, error)) return NULL;
    return json_node_copy(json_parser_get_root(parser));
}
static JsonNode *await_message(LanguageClient *c, guint id, const char *diagnostic_uri, GError **error)
{
    for (guint i = 0; i < 500; i++) {
        g_autoptr(JsonNode) message = read_message(c, error); if (!message) return NULL;
        if (diagnostic_uri) {
            const char *method = language_string(language_member(message, "method"));
            JsonNode *params = language_member(message, "params");
            if (g_strcmp0(method, "textDocument/publishDiagnostics") == 0 && g_strcmp0(language_string(language_member(params, "uri")), diagnostic_uri) == 0 &&
                language_integer(language_member(params, "version"), c->version) == c->version) return json_node_copy(params);
        } else if (language_integer(language_member(message, "id"), -1) == id) {
            JsonNode *failure = language_member(message, "error");
            if (failure) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "clangd: %s", language_string(language_member(failure, "message"))); return NULL; }
            JsonNode *result = language_member(message, "result"); return result ? json_node_copy(result) : json_node_new(JSON_NODE_NULL);
        }
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Превышен лимит LSP-сообщений"); return NULL;
}
static void run_query(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)cancel; Query *q = data; LanguageClient *c = q->client; g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) response = NULL;
    if (!c->process) {
        g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
        g_subprocess_launcher_set_cwd(launcher, c->root);
        g_autofree char *build = c->build ? g_strdup_printf("--compile-commands-dir=%s", c->build) : NULL;
        const char *args[] = {c->tool, "--background-index=false", "--clang-tidy=false", "--enable-config=false", "--log=error", build, NULL};
        g_mutex_lock(&c->mutex);
        if (!g_cancellable_is_cancelled(c->cancel)) c->process = g_subprocess_launcher_spawnv(launcher, args, &error);
        g_mutex_unlock(&c->mutex);
        if (!c->process) { if (!error) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "clangd остановлен"); goto finish; }
        c->input = G_BUFFERED_INPUT_STREAM(g_buffered_input_stream_new(g_subprocess_get_stdout_pipe(c->process)));
    }
    if (!c->initialized) {
        g_autoptr(GFile) root = g_file_new_for_path(c->root); g_autofree char *uri = g_file_get_uri(root);
        g_autoptr(JsonObject) init = json_object_new(); json_object_set_string_member(init, "rootUri", uri);
        g_autoptr(JsonParser) caps = json_parser_new(); json_parser_load_from_data(caps, "{\"general\":{\"positionEncodings\":[\"utf-16\"]},\"textDocument\":{\"completion\":{\"completionItem\":{\"snippetSupport\":false}},\"publishDiagnostics\":{\"versionSupport\":true}}}", -1, NULL);
        json_object_set_member(init, "capabilities", json_node_copy(json_parser_get_root(caps)));
        g_autoptr(JsonNode) params = json_node_new(JSON_NODE_OBJECT); json_node_set_object(params, init);
        g_autoptr(JsonNode) ready = NULL;
        guint id = ++c->serial;
        if (!send(c, "initialize", id, params, &error)) goto finish;
        ready = await_message(c, id, NULL, &error); if (!ready) goto finish;
        if (!send(c, "initialized", 0, NULL, &error)) goto finish;
        c->initialized = TRUE;
    }
    {
        g_autoptr(GFile) file = g_file_new_for_path(q->path); g_autofree char *uri = g_file_get_uri(file);
        /* Reopen snapshots: diagnostics match version and no unsaved state leaks to disk. */
        gboolean same = g_strcmp0(c->open_uri, uri) == 0;
        if (!same && c->open_uri) {
            g_autoptr(JsonObject) doc = json_object_new(); json_object_set_string_member(doc, "uri", c->open_uri);
            g_autoptr(JsonObject) params = json_object_new(); json_object_set_object_member(params, "textDocument", json_object_ref(doc));
            g_autoptr(JsonNode) n = json_node_new(JSON_NODE_OBJECT); json_node_set_object(n, params);
            if (!send(c, "textDocument/didClose", 0, n, &error)) goto finish;
        }
        g_free(c->open_uri); c->open_uri = g_strdup(uri); c->version++;
        g_autoptr(JsonObject) doc = json_object_new(); json_object_set_string_member(doc, "uri", uri);
        json_object_set_int_member(doc, "version", c->version);
        g_autoptr(JsonObject) open = json_object_new(); json_object_set_object_member(open, "textDocument", json_object_ref(doc));
        if (same) {
            JsonObject *content = json_object_new(); json_object_set_string_member(content, "text", q->text);
            JsonArray *changes = json_array_new(); json_array_add_object_element(changes, content); json_object_set_array_member(open, "contentChanges", changes);
        } else {
            json_object_set_string_member(doc, "languageId", g_str_has_suffix(q->path, ".c") || g_str_has_suffix(q->path, ".h") ? "c" : "cpp");
            json_object_set_string_member(doc, "text", q->text);
        }
        g_autoptr(JsonNode) opened = json_node_new(JSON_NODE_OBJECT); json_node_set_object(opened, open);
        if (!send(c, same ? "textDocument/didChange" : "textDocument/didOpen", 0, opened, &error)) goto finish;
        if (g_str_equal(q->method, "diagnostics")) response = await_message(c, 0, uri, &error);
        else {
            g_autoptr(JsonObject) position = json_object_new(); json_object_set_int_member(position, "line", q->line); json_object_set_int_member(position, "character", q->column);
            g_autoptr(JsonObject) which = json_object_new(); json_object_set_string_member(which, "uri", uri);
            g_autoptr(JsonObject) params = json_object_new(); json_object_set_object_member(params, "textDocument", json_object_ref(which)); json_object_set_object_member(params, "position", json_object_ref(position));
            g_autoptr(JsonNode) n = json_node_new(JSON_NODE_OBJECT); json_node_set_object(n, params);
            guint id = ++c->serial;
            if (send(c, q->method, id, n, &error)) response = await_message(c, id, NULL, &error);
        }
    }
finish:
    if (response) g_task_return_pointer(task, g_steal_pointer(&response), (GDestroyNotify)json_node_free);
    else { if (!error) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "clangd не вернул результат"); g_task_return_error(task, g_steal_pointer(&error)); }
}
typedef struct { GAsyncReadyCallback callback; gpointer data; LanguageClient *client; } Callback;
static void completed(GObject *source, GAsyncResult *result, gpointer data)
{
    Callback *cb = data; LanguageClient *c = cb->client;
    if (c->timeout) { g_source_remove(c->timeout); c->timeout = 0; }
    c->busy = FALSE;
    if (cb->callback) cb->callback(source, result, cb->data);
    language_client_unref(c); g_free(cb);
}
gboolean language_client_request(LanguageClient *c, const char *path, const char *text, guint line, guint column,
    const char *method, GAsyncReadyCallback callback, gpointer data)
{
    if (c->busy || g_cancellable_is_cancelled(c->cancel)) return FALSE;
    c->busy = TRUE; Query *q = g_new0(Query, 1); q->client = client_ref(c); q->path = g_strdup(path); q->text = g_strdup(text); q->method = g_strdup(method); q->line = line; q->column = column;
    Callback *cb = g_new0(Callback, 1); cb->callback = callback; cb->data = data; cb->client = client_ref(c);
    c->timeout = g_timeout_add_seconds(15, expired, c);
    GTask *task = g_task_new(NULL, c->cancel, completed, cb); g_task_set_task_data(task, q, query_free);
    g_task_run_in_thread(task, run_query); g_object_unref(task); return TRUE;
}
JsonNode *language_client_finish(GAsyncResult *result, GError **error)
{ return g_task_propagate_pointer(G_TASK(result), error); }
