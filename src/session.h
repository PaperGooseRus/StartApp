#ifndef STARTAPP_SESSION_H
#define STARTAPP_SESSION_H
#include <gio/gio.h>

typedef struct SessionStore SessionStore;
typedef struct {
    char *file, *build_directory, *configuration, *target, *page, *cmake;
    char **expanded;
    gint line, column, width, height, sidebar, editor, log;
    gboolean target_mode, overview, groups, log_visible;
    guint view;
    gboolean split, inspector;
    double zoom, offset_x, offset_y;
} SessionState;

SessionStore *session_store_new(const char *filename);
void session_store_free(SessionStore *store);
SessionState *session_load(SessionStore *store, const char *root);
void session_state_free(SessionState *state);
gboolean session_save(SessionStore *store, const char *root, const SessionState *state, GError **error);
char **session_recent(SessionStore *store);

#endif
