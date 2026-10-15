#include "recovery.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <string.h>

static char *filename(const char *directory, const char *path)
{
    g_autofree char *canonical = g_canonicalize_filename(path, NULL);
    g_autofree char *id = g_compute_checksum_for_string(G_CHECKSUM_SHA256, canonical, -1);
    g_autofree char *base = g_strconcat(id, ".json", NULL); return g_build_filename(directory, base, NULL);
}
gboolean recovery_save(const char *directory, const Document *doc, const char *text, GError **error)
{
    if (strlen(text) > 2*1024*1024 || !g_utf8_validate(text, -1, NULL)) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Черновик превышает 2 MiB или не является UTF-8"); return FALSE; }
    if (g_mkdir_with_parents(directory, 0700) != 0) { g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "%s", g_strerror(errno)); return FALSE; }
    g_autofree char *base = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)doc->original, doc->original_length);
    g_autoptr(JsonObject) object = json_object_new(); json_object_set_string_member(object, "path", doc->path); json_object_set_string_member(object, "base", base); json_object_set_string_member(object, "text", text);
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, object); g_autofree char *json = json_to_string(node, FALSE), *path = filename(directory, doc->path);
    return g_file_set_contents_full(path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}
static const char *string(JsonObject *o, const char *key)
{
    JsonNode *n = json_object_get_member(o, key);
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : NULL;
}
char *recovery_load(const char *directory, const Document *doc, GError **error)
{
    g_autofree char *path = filename(directory, doc->path);
    g_autoptr(GFile) file = g_file_new_for_path(path); g_autoptr(GFileInfo) info = g_file_query_info(file, "standard::type,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    if (!info) return NULL;
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR || g_file_info_get_size(info) > 16*1024*1024) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Недопустимый файл черновика"); return NULL; }
    g_autoptr(JsonParser) parser = json_parser_new(); if (!json_parser_load_from_file(parser, path, error)) return NULL;
    JsonNode *node = json_parser_get_root(parser); if (!JSON_NODE_HOLDS_OBJECT(node)) return NULL;
    JsonObject *o = json_node_get_object(node); const char *text = string(o, "text"), *original = string(o, "base"), *stored_path = string(o, "path");
    if (!text || !original || !stored_path || g_strcmp0(stored_path, doc->path) || strlen(text) > 2*1024*1024) return NULL;
    g_autofree char *base = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)doc->original, doc->original_length);
    if (g_strcmp0(base, original)) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG, "Файл изменился после создания черновика. Черновик сохранён отдельно: %s", path); return NULL; }
    return g_strdup(text);
}
void recovery_remove(const char *directory, const char *path)
{ g_autofree char *file = filename(directory, path); g_unlink(file); }
