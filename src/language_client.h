#ifndef STARTAPP_LANGUAGE_CLIENT_H
#define STARTAPP_LANGUAGE_CLIENT_H
#include <gio/gio.h>
#include <json-glib/json-glib.h>
typedef struct LanguageClient LanguageClient;
LanguageClient *language_client_new(const char *tool, const char *root, const char *build_directory);
/* One in-flight request; callbacks always complete, including cancellation. */
gboolean language_client_request(LanguageClient *client, const char *path, const char *text,
    guint line, guint utf16_column, const char *method, GAsyncReadyCallback callback, gpointer data);
JsonNode *language_client_finish(GAsyncResult *result, GError **error);
void language_client_stop(LanguageClient *client);
void language_client_unref(LanguageClient *client);
guint language_utf16_offset(const char *text, guint chars);
guint language_char_offset(const char *text, guint utf16);
JsonNode *language_member(JsonNode *node, const char *key);
const char *language_string(JsonNode *node);
gint64 language_integer(JsonNode *node, gint64 fallback);
#endif
