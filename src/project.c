#include "project.h"

#include <string.h>

#define MAX_FILES 10000u
#define MAX_DEPTH 64u
#define MAX_TEXT_SIZE (2u * 1024u * 1024u)

/* Model paths use '/', even on Windows. Physical paths are handled by GIO. */
static char *model_path(const char *path)
{
    char *normalized = g_strdup(path);
#ifdef G_OS_WIN32
    for (char *p = normalized; *p; p++) if (*p == '\\') *p = '/';
#endif
    return normalized;
}

static gboolean same_path(const char *left, const char *right)
{
#ifdef G_OS_WIN32
    g_autofree char *a = g_utf8_casefold(left, -1);
    g_autofree char *b = g_utf8_casefold(right, -1);
    return g_str_equal(a, b);
#else
    return g_str_equal(left, right);
#endif
}

static void file_free(gpointer data)
{
    ProjectFile *file = data;
    g_free(file->path);
    g_free(file);
}

static void edge_free(gpointer data)
{
    ProjectEdge *edge = data;
    g_free(edge->include);
    g_free(edge);
}

void project_free(Project *project)
{
    if (!project) return;
    g_free(project->root);
    g_ptr_array_unref(project->files);
    g_ptr_array_unref(project->edges);
    g_ptr_array_unref(project->notes);
    g_free(project);
}

static gboolean is_build_file(const char *name)
{
    return g_str_equal(name, "CMakeLists.txt") ||
           g_str_equal(name, "Makefile") || g_str_equal(name, "makefile") ||
           g_str_equal(name, "GNUmakefile") || g_str_equal(name, "meson.build") ||
           g_str_equal(name, "build.ninja") || g_str_has_suffix(name, ".cmake") ||
           g_str_has_suffix(name, ".mk");
}

static gboolean is_source(const char *name)
{
    static const char *extensions[] = {".c", ".h", ".cc", ".cpp", ".cxx",
                                       ".hpp", ".hh", ".hxx", NULL};
    for (guint i = 0; extensions[i]; i++)
        if (g_str_has_suffix(name, extensions[i])) return TRUE;
    return FALSE;
}

static gboolean is_text(const char *name)
{
    static const char *extensions[] = {".md", ".txt", ".json", ".toml",
        ".yaml", ".yml", ".py", ".sh", ".rs", ".go", ".js", ".ts",
        ".css", ".xml", ".ui", ".ini", ".conf", NULL};
    if (is_source(name) || is_build_file(name) || g_str_equal(name, ".gitignore"))
        return TRUE;
    for (guint i = 0; extensions[i]; i++)
        if (g_str_has_suffix(name, extensions[i])) return TRUE;
    return FALSE;
}

static gboolean ignored_directory(const char *name)
{
    static const char *ignored[] = {".git", ".hg", ".svn", "build", "dist",
        "node_modules", "target", ".venv", "venv", "__pycache__", NULL};
    for (guint i = 0; ignored[i]; i++)
        if (g_str_equal(name, ignored[i])) return TRUE;
    return g_str_has_prefix(name, "cmake-build-");
}

static gboolean walk(Project *project, const char *relative, guint depth,
                     GError **error)
{
    if (depth > MAX_DEPTH || project->files->len >= MAX_FILES) {
        project->skipped++;
        return TRUE;
    }
    g_autofree char *absolute = g_build_filename(project->root, relative, NULL);
    g_autoptr(GFile) directory = g_file_new_for_path(absolute);
    g_autoptr(GFileEnumerator) iterator = g_file_enumerate_children(directory,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
    if (!iterator) return FALSE;

    while (TRUE) {
        g_autoptr(GFileInfo) info = g_file_enumerator_next_file(iterator, NULL, error);
        if (!info) return !error || !*error;
        const char *name = g_file_info_get_name(info);
        if (!g_utf8_validate(name, -1, NULL) || g_file_info_get_is_symlink(info)) {
            project->skipped++;
            continue;
        }
        g_autofree char *path = *relative ? g_strconcat(relative, "/", name, NULL) : g_strdup(name);
        GFileType type = g_file_info_get_file_type(info);
        if (type == G_FILE_TYPE_DIRECTORY && !ignored_directory(name)) {
            g_autoptr(GError) child_error = NULL;
            if (!walk(project, path, depth + 1, &child_error)) {
                g_ptr_array_add(project->notes, g_strdup_printf("%s: %s",
                    path, child_error->message));
            }
        } else if (type == G_FILE_TYPE_REGULAR && is_text(name)) {
            if (project->files->len >= MAX_FILES ||
                g_file_info_get_size(info) > MAX_TEXT_SIZE) {
                project->skipped++;
                continue;
            }
            ProjectFile *file = g_new0(ProjectFile, 1);
            file->path = g_strdup(path);
            file->source = is_source(name);
            file->build_file = is_build_file(name);
            g_ptr_array_add(project->files, file);
        }
    }
}

static gint file_compare(gconstpointer left, gconstpointer right)
{
    const ProjectFile *a = *(ProjectFile * const *)left;
    const ProjectFile *b = *(ProjectFile * const *)right;
    return g_strcmp0(a->path, b->path);
}

gint project_find_file(const Project *project, const char *path)
{
    g_autofree char *normalized = model_path(path);
    for (guint i = 0; i < project->files->len; i++) {
        ProjectFile *file = g_ptr_array_index(project->files, i);
        if (same_path(file->path, normalized)) return (gint)i;
    }
    return -1;
}

gboolean project_path_matches(const char *path, const char *query)
{
    g_autofree char *trimmed = g_strdup(query);
    g_strstrip(trimmed);
    if (!*trimmed) return TRUE;
    g_autofree char *folded_path = g_utf8_casefold(path, -1);
    g_autofree char *folded_query = g_utf8_casefold(trimmed, -1);
    g_autofree char *normalized_path = g_utf8_normalize(folded_path, -1, G_NORMALIZE_ALL_COMPOSE);
    g_autofree char *normalized_query = g_utf8_normalize(folded_query, -1, G_NORMALIZE_ALL_COMPOSE);
    return strstr(normalized_path, normalized_query) != NULL;
}

static gint resolve_include(Project *project, const char *from, const char *name,
                            gboolean system, gboolean *ambiguous)
{
    if (g_path_is_absolute(name)) return -1;
    g_autofree char *normalized = model_path(name);
    if (!system) {
        g_autofree char *directory = g_path_get_dirname(from);
        g_autofree char *combined = g_build_filename(project->root, directory, name, NULL);
        g_autofree char *canonical = g_canonicalize_filename(combined, NULL);
        g_autoptr(GFile) root = g_file_new_for_path(project->root);
        g_autoptr(GFile) candidate = g_file_new_for_path(canonical);
        g_autofree char *relative = g_file_get_relative_path(root, candidate);
        if (relative) {
            gint found = project_find_file(project, relative);
            if (found >= 0) return found;
        }
    }
    gint found = project_find_file(project, normalized);
    if (found >= 0) return found;
    g_autofree char *suffix = g_strconcat("/", normalized, NULL);
    for (guint i = 0; i < project->files->len; i++) {
        ProjectFile *file = g_ptr_array_index(project->files, i);
        gsize path_len = strlen(file->path), suffix_len = strlen(suffix);
        if (path_len < suffix_len || !same_path(file->path + path_len - suffix_len, suffix)) continue;
        if (found >= 0) {
            *ambiguous = TRUE;
            return -1;
        }
        found = (gint)i;
    }
    return found;
}

/* Replace comments with spaces, retaining line numbers, strings and characters. */
static char *without_comments(const char *text)
{
    char *result = g_strdup(text);
    enum { NORMAL, BLOCK, LINE, STRING, CHARACTER } state = NORMAL;
    for (gsize i = 0; result[i]; i++) {
        char c = result[i], next = result[i + 1];
        if (state == BLOCK) {
            if (c == '*' && next == '/') {
                result[i] = result[i + 1] = ' ';
                i++;
                state = NORMAL;
            } else if (c != '\n') result[i] = ' ';
        } else if (state == LINE) {
            if (c == '\n') state = NORMAL;
            else result[i] = ' ';
        } else if (state == STRING || state == CHARACTER) {
            if (c == '\\' && next) i++;
            else if ((state == STRING && c == '"') ||
                     (state == CHARACTER && c == '\'')) state = NORMAL;
        } else if (c == '/' && (next == '*' || next == '/')) {
            state = next == '*' ? BLOCK : LINE;
            result[i] = result[i + 1] = ' ';
            i++;
        } else if (c == '"') state = STRING;
        else if (c == '\'') state = CHARACTER;
    }
    return result;
}

static void parse_includes(Project *project, guint index)
{
    ProjectFile *file = g_ptr_array_index(project->files, index);
    g_autofree char *absolute = g_build_filename(project->root, file->path, NULL);
    g_autofree char *text = NULL;
    gsize length = 0;
    g_autoptr(GError) error = NULL;
    if (!g_file_get_contents(absolute, &text, &length, &error)) {
        g_ptr_array_add(project->notes, g_strdup_printf("%s: %s", file->path, error->message));
        return;
    }
    if (length > MAX_TEXT_SIZE || memchr(text, '\0', length) ||
        !g_utf8_validate(text, (gssize)length, NULL)) {
        g_ptr_array_add(project->notes, g_strdup_printf("%s: not supported UTF-8 text", file->path));
        return;
    }
    g_autofree char *clean = without_comments(text);
    g_auto(GStrv) lines = g_strsplit(clean, "\n", -1);
    for (guint line = 0; lines[line]; line++) {
        char *cursor = lines[line];
        while (g_ascii_isspace(*cursor)) cursor++;
        if (*cursor++ != '#') continue;
        while (g_ascii_isspace(*cursor)) cursor++;
        if (!g_str_has_prefix(cursor, "include")) continue;
        cursor += 7;
        if (*cursor && !g_ascii_isspace(*cursor) && *cursor != '"' && *cursor != '<') continue;
        while (g_ascii_isspace(*cursor)) cursor++;
        gboolean system = *cursor == '<';
        if (*cursor != '"' && !system) continue; /* Macro includes need a compiler. */
        cursor++;
        char *end = strchr(cursor, system ? '>' : '"');
        if (!end || end == cursor) continue;
        ProjectEdge *edge = g_new0(ProjectEdge, 1);
        edge->from = index;
        edge->include = g_strndup(cursor, (gsize)(end - cursor));
        edge->line = line + 1;
        edge->system = system;
        edge->to = resolve_include(project, file->path, edge->include,
                                   system, &edge->ambiguous);
        g_ptr_array_add(project->edges, edge);
    }
}

Project *project_scan(const char *directory, GError **error)
{
    g_autofree char *canonical = g_canonicalize_filename(directory, NULL);
    g_autoptr(GFile) root = g_file_new_for_path(canonical);
    g_autoptr(GFileInfo) info = g_file_query_info(root, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                               G_FILE_QUERY_INFO_NONE, NULL, error);
    if (!info) return NULL;
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                            "Select a project directory");
        return NULL;
    }
    Project *project = g_new0(Project, 1);
    project->root = g_strdup(canonical);
    project->files = g_ptr_array_new_with_free_func(file_free);
    project->edges = g_ptr_array_new_with_free_func(edge_free);
    project->notes = g_ptr_array_new_with_free_func(g_free);
    if (!walk(project, "", 0, error)) {
        project_free(project);
        return NULL;
    }
    g_ptr_array_sort(project->files, file_compare);
    for (guint i = 0; i < project->files->len; i++) {
        ProjectFile *file = g_ptr_array_index(project->files, i);
        if (file->source) parse_includes(project, i);
    }
    return project;
}

char *project_summary(const Project *project)
{
    guint source_count = 0, build_count = 0, local = 0, unresolved = 0;
    for (guint i = 0; i < project->files->len; i++) {
        ProjectFile *file = g_ptr_array_index(project->files, i);
        source_count += file->source;
        build_count += file->build_file;
    }
    for (guint i = 0; i < project->edges->len; i++) {
        ProjectEdge *edge = g_ptr_array_index(project->edges, i);
        local += edge->to >= 0;
        unresolved += edge->to < 0 && !edge->system;
    }
    return g_strdup_printf("%u файлов · %u C/C++ · %u файлов сборки\n"
        "%u локальных связей · %u неразрешённых #include\n"
        "%u пропущено · %u замечаний", project->files->len, source_count,
        build_count, local, unresolved, project->skipped, project->notes->len);
}

static void json_string(GString *output, const char *value)
{
    g_string_append_c(output, '"');
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') g_string_append_printf(output, "\\%c", *p);
        else if (*p < 32) g_string_append_printf(output, "\\u%04x", *p);
        else g_string_append_c(output, (char)*p);
    }
    g_string_append_c(output, '"');
}

char *project_to_json(const Project *project)
{
    GString *output = g_string_new("{\"root\":");
    json_string(output, project->root);
    g_string_append(output, ",\"files\":[");
    for (guint i = 0; i < project->files->len; i++) {
        ProjectFile *file = g_ptr_array_index(project->files, i);
        if (i) g_string_append_c(output, ',');
        g_string_append(output, "{\"path\":");
        json_string(output, file->path);
        g_string_append_printf(output, ",\"source\":%s,\"build_file\":%s}",
                               file->source ? "true" : "false",
                               file->build_file ? "true" : "false");
    }
    g_string_append(output, "],\"includes\":[");
    for (guint i = 0; i < project->edges->len; i++) {
        ProjectEdge *edge = g_ptr_array_index(project->edges, i);
        ProjectFile *from = g_ptr_array_index(project->files, edge->from);
        if (i) g_string_append_c(output, ',');
        g_string_append(output, "{\"from\":");
        json_string(output, from->path);
        g_string_append(output, ",\"to\":");
        if (edge->to >= 0) {
            ProjectFile *to = g_ptr_array_index(project->files, edge->to);
            json_string(output, to->path);
        } else g_string_append(output, "null");
        g_string_append(output, ",\"include\":");
        json_string(output, edge->include);
        g_string_append_printf(output,
            ",\"line\":%u,\"system\":%s,\"ambiguous\":%s}", edge->line,
            edge->system ? "true" : "false", edge->ambiguous ? "true" : "false");
    }
    g_string_append(output, "],\"notes\":[");
    for (guint i = 0; i < project->notes->len; i++) {
        if (i) g_string_append_c(output, ',');
        json_string(output, g_ptr_array_index(project->notes, i));
    }
    g_string_append_printf(output, "],\"skipped\":%u}\n", project->skipped);
    return g_string_free(output, FALSE);
}
