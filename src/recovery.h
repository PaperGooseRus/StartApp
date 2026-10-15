#ifndef STARTAPP_RECOVERY_H
#define STARTAPP_RECOVERY_H
#include "document.h"
gboolean recovery_save(const char *directory, const Document *document, const char *text, GError **error);
char *recovery_load(const char *directory, const Document *document, GError **error);
void recovery_remove(const char *directory, const char *path);
#endif
