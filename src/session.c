#include "session.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <errno.h>

struct SessionStore { GKeyFile *keys; char *filename; };

static char *root_key(const char *root)
{
    g_autofree char *canonical = g_canonicalize_filename(root, NULL);
#ifdef G_OS_WIN32
    g_autofree char *folded = g_utf8_casefold(canonical, -1);
    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, folded, -1);
#else
    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, canonical, -1);
#endif
}
SessionStore *session_store_new(const char *filename)
{
    SessionStore *store = g_new0(SessionStore, 1); store->keys = g_key_file_new();
    store->filename = filename ? g_strdup(filename) : g_build_filename(g_get_user_config_dir(), "startapp", "workspace.ini", NULL);
    g_autoptr(GFile) file = g_file_new_for_path(store->filename);
    g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    if (info && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR && g_file_info_get_size(info) <= 1024 * 1024) {
        g_autofree char *text = NULL; gsize length;
        if (g_file_load_contents(file, NULL, &text, &length, NULL, NULL) && length <= 1024 * 1024 &&
            !memchr(text, 0, length) && g_utf8_validate(text, (gssize)length, NULL))
            g_key_file_load_from_data(store->keys, text, length, G_KEY_FILE_NONE, NULL);
    }
    return store;
}
void session_store_free(SessionStore *store)
{ if (store) { g_key_file_unref(store->keys); g_free(store->filename); g_free(store); } }
void session_state_free(SessionState *state)
{
    if (!state) return;
    g_free(state->file); g_free(state->build_directory); g_free(state->configuration); g_free(state->target); g_free(state->page);
    g_free(state->cmake);
    g_strfreev(state->expanded); g_free(state);
}
static gint integer(GKeyFile *keys, const char *group, const char *key, gint minimum, gint maximum, gint fallback)
{
    g_autoptr(GError) error = NULL; gint value = g_key_file_get_integer(keys, group, key, &error);
    return !error && value >= minimum && value <= maximum ? value : fallback;
}
static double number(GKeyFile *keys, const char *group, const char *key, double minimum, double maximum, double fallback)
{
    g_autoptr(GError) error = NULL; double value = g_key_file_get_double(keys, group, key, &error);
    return !error && isfinite(value) && value >= minimum && value <= maximum ? value : fallback;
}
static char *string(GKeyFile *keys, const char *group, const char *key)
{
    char *value = g_key_file_get_string(keys, group, key, NULL);
    if (value && (strlen(value) > 8192 || !g_utf8_validate(value, -1, NULL))) g_clear_pointer(&value, g_free);
    return value;
}
SessionState *session_load(SessionStore *store, const char *root)
{
    g_autofree char *key = root_key(root);
    if (!g_key_file_has_group(store->keys, key)) return NULL;
    SessionState *state = g_new0(SessionState, 1);
    state->file = string(store->keys, key, "file"); state->build_directory = string(store->keys, key, "build-directory");
    state->configuration = string(store->keys, key, "configuration"); state->target = string(store->keys, key, "target");
    state->page = string(store->keys, key, "page");
    state->cmake = string(store->keys, key, "cmake");
    state->line = integer(store->keys, key, "line", 1, G_MAXINT, 1); state->column = integer(store->keys, key, "column", 0, G_MAXINT, 0);
    state->width = integer(store->keys, key, "width", 800, 10000, 1420); state->height = integer(store->keys, key, "height", 600, 10000, 900);
    state->sidebar = integer(store->keys, key, "sidebar", 180, 3000, 250); state->editor = integer(store->keys, key, "editor", 200, 6000, 470);
    state->log = integer(store->keys, key, "log", 180, 10000, 520);
    state->zoom = number(store->keys, key, "zoom", .03, 3, 1);
    state->offset_x = number(store->keys, key, "offset-x", -100000, 100000, 0); state->offset_y = number(store->keys, key, "offset-y", -100000, 100000, 0);
    state->target_mode = g_key_file_get_boolean(store->keys, key, "target-mode", NULL);
    state->overview = g_key_file_get_boolean(store->keys, key, "overview", NULL);
    state->groups = g_key_file_get_boolean(store->keys, key, "groups", NULL);
    state->log_visible = g_key_file_get_boolean(store->keys, key, "log-visible", NULL);
    gsize count = 0; state->expanded = g_key_file_get_string_list(store->keys, key, "expanded", &count, NULL);
    if (count > 200) g_clear_pointer(&state->expanded, g_strfreev);
    return state;
}
char **session_recent(SessionStore *store)
{
    gsize count = 0; g_auto(GStrv) saved = g_key_file_get_string_list(store->keys, "application", "recent", &count, NULL);
    GPtrArray *recent = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; saved && i < MIN(count, 20u); i++)
        if (g_path_is_absolute(saved[i]) && strlen(saved[i]) <= 8192) g_ptr_array_add(recent, g_strdup(saved[i]));
    g_ptr_array_add(recent, NULL); return (char **)g_ptr_array_free(recent, FALSE);
}
static void set_string(GKeyFile *keys, const char *group, const char *key, const char *value)
{ if (value) g_key_file_set_string(keys, group, key, value); else g_key_file_remove_key(keys, group, key, NULL); }
gboolean session_save(SessionStore *store, const char *root, const SessionState *state, GError **error)
{
    g_autofree char *key = root_key(root), *canonical = g_canonicalize_filename(root, NULL);
    GKeyFile *keys = store->keys;
    set_string(keys, key, "root", canonical); set_string(keys, key, "file", state->file);
    set_string(keys, key, "build-directory", state->build_directory); set_string(keys, key, "configuration", state->configuration);
    set_string(keys, key, "target", state->target); set_string(keys, key, "page", state->page);
    set_string(keys, key, "cmake", state->cmake);
    const char *ints[] = {"line", "column", "width", "height", "sidebar", "editor", "log"};
    const gint values[] = {state->line, state->column, state->width, state->height, state->sidebar, state->editor, state->log};
    for (guint i = 0; i < G_N_ELEMENTS(ints); i++) g_key_file_set_integer(keys, key, ints[i], values[i]);
    g_key_file_set_boolean(keys, key, "target-mode", state->target_mode); g_key_file_set_boolean(keys, key, "overview", state->overview);
    g_key_file_set_boolean(keys, key, "groups", state->groups); g_key_file_set_boolean(keys, key, "log-visible", state->log_visible);
    g_key_file_set_double(keys, key, "zoom", state->zoom); g_key_file_set_double(keys, key, "offset-x", state->offset_x); g_key_file_set_double(keys, key, "offset-y", state->offset_y);
    if (state->expanded) g_key_file_set_string_list(keys, key, "expanded", (const char *const *)state->expanded, g_strv_length(state->expanded));
    else g_key_file_remove_key(keys, key, "expanded", NULL);
    g_auto(GStrv) old = session_recent(store);
    g_autoptr(GPtrArray) recent = g_ptr_array_new_with_free_func(g_free); g_ptr_array_add(recent, g_strdup(canonical));
    for (guint i = 0; old[i]; i++) {
        g_autofree char *old_key = root_key(old[i]);
        if (g_str_equal(old_key, key)) continue;
        if (recent->len < 20) g_ptr_array_add(recent, g_strdup(old[i]));
        else g_key_file_remove_group(keys, old_key, NULL);
    }
    g_key_file_set_string_list(keys, "application", "recent", (const char *const *)recent->pdata, recent->len);
    g_autofree char *parent = g_path_get_dirname(store->filename);
    if (g_mkdir_with_parents(parent, 0700) != 0) { g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Cannot create session directory: %s", g_strerror(errno)); return FALSE; }
    gsize length; g_autofree char *text = g_key_file_to_data(keys, &length, error);
    return text && g_file_set_contents_full(store->filename, text, (gssize)length, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}
