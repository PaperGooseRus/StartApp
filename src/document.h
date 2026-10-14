#ifndef STARTAPP_DOCUMENT_H
#define STARTAPP_DOCUMENT_H

#include <gio/gio.h>

typedef struct {
    char *path;
    char *text;
    char *etag;
    char *original;
    gsize original_length;
    gboolean crlf;
} Document;

Document *document_open(const char *path, GError **error);
gboolean document_save(Document *document, const char *text, GError **error);
void document_free(Document *document);

#endif
