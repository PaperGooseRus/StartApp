#include "document.h"
#include <string.h>

#define MAX_TEXT_SIZE (2u * 1024u * 1024u)

Document *document_open(const char *path, GError **error)
{
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInfo) info = g_file_query_info(file,
        G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
    if (!info) return NULL;
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR ||
        g_file_info_get_size(info) > MAX_TEXT_SIZE) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Only regular text files up to 2 MiB can be edited");
        return NULL;
    }
    g_autofree char *text = NULL;
    g_autofree char *etag = NULL;
    gsize length = 0;
    if (!g_file_load_contents(file, NULL, &text, &length, &etag, error)) return NULL;
    if (length > MAX_TEXT_SIZE || memchr(text, '\0', length) ||
        !g_utf8_validate(text, (gssize)length, NULL)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "The editor supports UTF-8 text without NUL bytes");
        return NULL;
    }
    Document *document = g_new0(Document, 1);
    document->path = g_strdup(path);
    document->etag = g_steal_pointer(&etag);
    document->crlf = strstr(text, "\r\n") != NULL;
    for (gsize i = 0; i < length; i++)
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) document->crlf = FALSE;
    document->original = g_memdup2(text, length + 1);
    document->original_length = length;
    GString *normalized = g_string_sized_new(length);
    for (gsize i = 0; i < length; i++) {
        if (text[i] == '\r' && text[i + 1] == '\n') continue;
        g_string_append_c(normalized, text[i]);
    }
    document->text = g_string_free(normalized, FALSE);
    return document;
}

gboolean document_save(Document *document, const char *text, GError **error)
{
    if (!g_utf8_validate(text, -1, NULL) || strlen(text) > MAX_TEXT_SIZE) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "The document must be UTF-8 text, no larger than 2 MiB");
        return FALSE;
    }
    g_autoptr(GFile) file = g_file_new_for_path(document->path);
    g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
    if (!info) return FALSE;
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "The file was replaced by a directory or symbolic link");
        return FALSE;
    }
    /* Also compare content: some file systems expose coarse modification times. */
    g_autofree char *disk = NULL;
    g_autofree char *disk_etag = NULL;
    gsize length = 0;
    if (!g_file_load_contents(file, NULL, &disk, &length, &disk_etag, error)) return FALSE;
    gboolean same = length == document->original_length &&
                    memcmp(disk, document->original, length) == 0;
    if (!same || g_strcmp0(document->etag, disk_etag) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG,
                            "File changed outside the editor. Reload before saving.");
        return FALSE;
    }
    GString *output = g_string_new(NULL);
    for (const char *p = text; *p; p++) {
        if (document->crlf && *p == '\n') g_string_append_c(output, '\r');
        g_string_append_c(output, *p);
    }
    char *new_etag = NULL;
    gboolean saved = g_file_replace_contents(file, output->str, output->len,
        document->etag, FALSE, G_FILE_CREATE_NONE, &new_etag, NULL, error);
    if (saved) {
        g_free(document->etag);
        document->etag = new_etag;
        g_free(document->text);
        document->text = g_strdup(text);
        g_free(document->original);
        document->original = g_memdup2(output->str, output->len + 1);
        document->original_length = output->len;
    } else g_free(new_etag);
    g_string_free(output, TRUE);
    return saved;
}

void document_free(Document *document)
{
    if (!document) return;
    g_free(document->path);
    g_free(document->text);
    g_free(document->etag);
    g_free(document->original);
    g_free(document);
}
