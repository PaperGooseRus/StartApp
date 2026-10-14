#include "project.h"
#include "build_metadata.h"

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
    g_free(file->settings_source);
    g_clear_pointer(&file->quote_dirs, g_ptr_array_unref);
    g_clear_pointer(&file->include_dirs, g_ptr_array_unref);
    g_clear_pointer(&file->system_dirs, g_ptr_array_unref);
    g_clear_pointer(&file->after_dirs, g_ptr_array_unref);
    g_free(file);
}

static void edge_free(gpointer data)
{
    ProjectEdge *edge = data;
    g_free(edge->include);
    g_free(edge->settings_source);
    g_free(edge);
}

void project_free(Project *project)
{
    if (!project) return;
    g_free(project->root);
    g_ptr_array_unref(project->files);
    g_ptr_array_unref(project->edges);
    g_ptr_array_unref(project->notes);
    g_free(project->build_directory);
    g_free(project->configuration);
    g_ptr_array_unref(project->configurations);
    g_ptr_array_unref(project->targets);
    g_ptr_array_unref(project->target_edges);
    g_free(project->cmake_version); g_free(project->generator);
    g_ptr_array_unref(project->compilers); g_ptr_array_unref(project->presets);
    g_hash_table_unref(project->file_index);
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
#ifdef G_OS_WIN32
    g_autofree char *folded = g_utf8_casefold(normalized, -1);
    const char *key = folded;
#else
    const char *key = normalized;
#endif
    guint encoded = GPOINTER_TO_UINT(g_hash_table_lookup(project->file_index, key));
    return encoded ? (gint)encoded - 1 : -1;
}

gint project_find_absolute(const Project *project, const char *path)
{
    g_autofree char *canonical = g_canonicalize_filename(path, NULL);
    g_autoptr(GFile) root = g_file_new_for_path(project->root);
    g_autoptr(GFile) candidate = g_file_new_for_path(canonical);
    g_autofree char *relative = g_file_get_relative_path(root, candidate);
    return relative ? project_find_file(project, relative) : -1;
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
    file->include_start = project->edges->len;
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
    file->include_count = project->edges->len - file->include_start;
}

static gint candidate_file(Project *project, const char *directory, const char *name,
                            gboolean *outside)
{
    g_autofree char *path = g_build_filename(directory, name, NULL);
    gint index = project_find_absolute(project, path);
    if (index >= 0) return index;
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) *outside = TRUE;
    return -1;
}

static gint resolve_context(Project *project, ProjectFile *from, ProjectFile *settings,
                            ProjectEdge *edge, gboolean *outside)
{
    if (g_path_is_absolute(edge->include)) {
        gint found = project_find_absolute(project, edge->include);
        if (found < 0 && g_file_test(edge->include, G_FILE_TEST_IS_REGULAR)) *outside = TRUE;
        return found;
    }
    if (!edge->system) {
        g_autofree char *file_path = g_build_filename(project->root, from->path, NULL);
        g_autofree char *directory = g_path_get_dirname(file_path);
        gint found = candidate_file(project, directory, edge->include, outside);
        if (found >= 0 || *outside) return found;
    }
    GPtrArray *groups[] = {settings->quote_dirs, settings->include_dirs, settings->system_dirs, settings->after_dirs};
    for (guint i = edge->system ? 1 : 0; i < G_N_ELEMENTS(groups); i++) {
        for (guint j = 0; groups[i] && j < groups[i]->len; j++) {
            gint found = candidate_file(project, g_ptr_array_index(groups[i], j), edge->include, outside);
            if (found >= 0 || *outside) return found;
        }
    }
    return -1; /* Known settings never fall back to guessing a matching basename. */
}

static void apply_compile_contexts(Project *project)
{
    guint visited_count = 0;
    for (guint root = 0; root < project->files->len; root++) {
        ProjectFile *settings = g_ptr_array_index(project->files, root);
        if (!settings->compile_settings) continue;
        g_autoptr(GHashTable) visited = g_hash_table_new(g_direct_hash, g_direct_equal);
        GQueue queue = G_QUEUE_INIT;
        g_queue_push_tail(&queue, GUINT_TO_POINTER(root + 1));
        while (!g_queue_is_empty(&queue)) {
            guint index = GPOINTER_TO_UINT(g_queue_pop_head(&queue)) - 1;
            if (g_hash_table_contains(visited, GUINT_TO_POINTER(index + 1))) continue;
            g_hash_table_add(visited, GUINT_TO_POINTER(index + 1));
            if (++visited_count > 200000) {
                g_queue_clear(&queue);
                g_ptr_array_add(project->notes, g_strdup("Compile context traversal limit reached; some header edges remain heuristic"));
                return;
            }
            ProjectFile *file = g_ptr_array_index(project->files, index);
            for (guint i = file->include_start; i < file->include_start + file->include_count; i++) {
                ProjectEdge *edge = g_ptr_array_index(project->edges, i);
                gboolean outside = FALSE;
                gint found = resolve_context(project, file, settings, edge, &outside);
                if (!edge->metadata) {
                    edge->to = found; edge->outside_project = outside; edge->ambiguous = FALSE;
                    edge->metadata = TRUE; edge->settings_source = g_strdup(settings->settings_source);
                } else if (edge->to != found || edge->outside_project != outside) {
                    edge->to = -1; edge->ambiguous = TRUE; edge->outside_project = FALSE;
                }
                if (found >= 0) g_queue_push_tail(&queue, GUINT_TO_POINTER((guint)found + 1));
            }
        }
    }
}

Project *project_scan_build(const char *directory, const char *build_directory,
                            const char *configuration, GError **error)
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
    project->file_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    project->configurations = g_ptr_array_new_with_free_func(g_free);
    project->targets = g_ptr_array_new_with_free_func(build_metadata_target_free);
    project->target_edges = g_ptr_array_new_with_free_func(build_metadata_edge_free);
    project->compilers = g_ptr_array_new_with_free_func(build_metadata_compiler_free);
    project->presets = g_ptr_array_new_with_free_func(build_metadata_preset_free);
    if (!walk(project, "", 0, error)) {
        project_free(project);
        return NULL;
    }
    g_ptr_array_sort(project->files, file_compare);
    for (guint i = 0; i < project->files->len; i++) {
        ProjectFile *file = g_ptr_array_index(project->files, i);
#ifdef G_OS_WIN32
        char *key = g_utf8_casefold(file->path, -1);
#else
        char *key = g_strdup(file->path);
#endif
        g_hash_table_insert(project->file_index, key, GUINT_TO_POINTER(i + 1));
    }
    build_metadata_import(project, build_directory, configuration);
    for (guint i = 0; i < project->files->len; i++) {
        ProjectFile *file = g_ptr_array_index(project->files, i);
        if (file->source) parse_includes(project, i);
    }
    apply_compile_contexts(project);
    return project;
}

Project *project_scan(const char *directory, GError **error)
{
    return project_scan_build(directory, NULL, NULL, error);
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
        "%u команд/групп · %u целей · %u связей целей\n"
        "%u пропущено · %u замечаний", project->files->len, source_count,
        build_count, local, unresolved, project->compile_units, project->targets->len,
        project->target_edges->len, project->skipped, project->notes->len);
}

static void json_string(GString *output, const char *value)
{
    if (!value) { g_string_append(output, "null"); return; }
    g_string_append_c(output, '"');
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') g_string_append_printf(output, "\\%c", *p);
        else if (*p < 32) g_string_append_printf(output, "\\u%04x", *p);
        else g_string_append_c(output, (char)*p);
    }
    g_string_append_c(output, '"');
}

static void json_paths(GString *output, GPtrArray *paths)
{
    g_string_append_c(output, '[');
    for (guint i = 0; paths && i < paths->len; i++) {
        if (i) g_string_append_c(output, ',');
        json_string(output, g_ptr_array_index(paths, i));
    }
    g_string_append_c(output, ']');
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
        g_string_append_printf(output, ",\"source\":%s,\"build_file\":%s,\"compile_settings\":%s,\"settings_source\":",
                               file->source ? "true" : "false",
                               file->build_file ? "true" : "false", file->compile_settings ? "true" : "false");
        json_string(output, file->settings_source);
        g_string_append(output, ",\"quote_dirs\":"); json_paths(output, file->quote_dirs);
        g_string_append(output, ",\"include_dirs\":"); json_paths(output, file->include_dirs);
        g_string_append(output, ",\"system_dirs\":"); json_paths(output, file->system_dirs);
        g_string_append(output, ",\"after_dirs\":"); json_paths(output, file->after_dirs);
        g_string_append_c(output, '}');
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
            ",\"line\":%u,\"system\":%s,\"ambiguous\":%s,\"metadata\":%s,\"outside_project\":%s,\"settings_source\":", edge->line,
            edge->system ? "true" : "false", edge->ambiguous ? "true" : "false",
            edge->metadata ? "true" : "false", edge->outside_project ? "true" : "false");
        json_string(output, edge->settings_source);
        g_string_append_c(output, '}');
    }
    g_string_append(output, "],\"build_directory\":"); json_string(output, project->build_directory);
    g_string_append(output, ",\"configuration\":"); json_string(output, project->configuration);
    g_string_append(output, ",\"configurations\":"); json_paths(output, project->configurations);
    g_string_append(output, ",\"targets\":[");
    for (guint i = 0; i < project->targets->len; i++) {
        ProjectTarget *target = g_ptr_array_index(project->targets, i);
        if (i) g_string_append_c(output, ',');
        g_string_append(output, "{\"id\":"); json_string(output, target->id);
        g_string_append(output, ",\"name\":"); json_string(output, target->name);
        g_string_append(output, ",\"type\":"); json_string(output, target->type);
        g_string_append(output, ",\"artifact\":"); json_string(output, target->artifact);
        g_string_append(output, ",\"definition\":"); json_string(output, target->definition);
        g_string_append_printf(output, ",\"line\":%u,\"sources\":[", target->line);
        for (guint j = 0; j < target->sources->len; j++) {
            if (j) g_string_append_c(output, ',');
            ProjectFile *file = g_ptr_array_index(project->files, g_array_index(target->sources, guint, j));
            json_string(output, file->path);
        }
        g_string_append(output, "]}");
    }
    g_string_append(output, "],\"target_dependencies\":[");
    for (guint i = 0; i < project->target_edges->len; i++) {
        ProjectTargetEdge *edge = g_ptr_array_index(project->target_edges, i);
        ProjectTarget *from = g_ptr_array_index(project->targets, edge->from);
        ProjectTarget *to = g_ptr_array_index(project->targets, edge->to);
        if (i) g_string_append_c(output, ',');
        g_string_append(output, "{\"from\":"); json_string(output, from->id);
        g_string_append(output, ",\"to\":"); json_string(output, to->id);
        g_string_append(output, ",\"definition\":"); json_string(output, edge->definition);
        g_string_append_printf(output, ",\"line\":%u}", edge->line);
    }
    g_string_append(output, "],\"build_profile\":{\"cmake_version\":"); json_string(output, project->cmake_version);
    g_string_append(output, ",\"generator\":"); json_string(output, project->generator);
    g_string_append_printf(output, ",\"multi_config\":%s,\"compilers\":[", project->multi_config ? "true" : "false");
    for (guint i = 0; i < project->compilers->len; i++) {
        ProjectCompiler *compiler = g_ptr_array_index(project->compilers, i);
        if (i) g_string_append_c(output, ',');
        g_string_append(output, "{\"language\":"); json_string(output, compiler->language);
        g_string_append(output, ",\"id\":"); json_string(output, compiler->id);
        g_string_append(output, ",\"version\":"); json_string(output, compiler->version);
        g_string_append(output, ",\"path\":"); json_string(output, compiler->path);
        g_string_append(output, ",\"source\":"); json_string(output, compiler->source); g_string_append_c(output, '}');
    }
    g_string_append(output, "],\"presets\":[");
    for (guint i = 0; i < project->presets->len; i++) {
        ProjectPreset *preset = g_ptr_array_index(project->presets, i);
        if (i) g_string_append_c(output, ',');
        g_string_append(output, "{\"name\":"); json_string(output, preset->name);
        g_string_append(output, ",\"display_name\":"); json_string(output, preset->display_name);
        g_string_append(output, ",\"kind\":"); json_string(output, preset->kind);
        g_string_append(output, ",\"generator\":"); json_string(output, preset->generator);
        g_string_append(output, ",\"binary_directory\":"); json_string(output, preset->binary_directory);
        g_string_append(output, ",\"configuration\":"); json_string(output, preset->configuration);
        g_string_append(output, ",\"configure_preset\":"); json_string(output, preset->configure_preset);
        g_string_append(output, ",\"source\":"); json_string(output, preset->source);
        g_string_append_printf(output, ",\"hidden\":%s}", preset->hidden ? "true" : "false");
    }
    g_string_append(output, "]},\"notes\":[");
    for (guint i = 0; i < project->notes->len; i++) {
        if (i) g_string_append_c(output, ',');
        json_string(output, g_ptr_array_index(project->notes, i));
    }
    g_string_append_printf(output, "],\"skipped\":%u}\n", project->skipped);
    return g_string_free(output, FALSE);
}

char *project_build_summary(const Project *project, gint target_index)
{
    GString *text = g_string_new("Файлы инструментов в проекте:\n");
    gboolean cmake = FALSE, make = FALSE, ninja = FALSE, meson = FALSE;
    for (guint i = 0; i < project->files->len; i++) {
        ProjectFile *file = g_ptr_array_index(project->files, i);
        g_autofree char *name = g_path_get_basename(file->path);
        cmake |= g_str_equal(name, "CMakeLists.txt") || g_str_has_suffix(name, ".cmake");
        make |= g_ascii_strcasecmp(name, "Makefile") == 0 || g_str_equal(name, "GNUmakefile") || g_str_has_suffix(name, ".mk");
        ninja |= g_str_equal(name, "build.ninja"); meson |= g_str_equal(name, "meson.build");
    }
    if (cmake) g_string_append(text, "• CMake\n");
    if (make) g_string_append(text, "• Make\n");
    if (ninja) g_string_append(text, "• Ninja\n");
    if (meson) g_string_append(text, "• Meson\n");
    if (!cmake && !make && !ninja && !meson) g_string_append(text, "Не найдены\n");
    g_string_append_printf(text, "\nНастроенная сборка:\nГенератор: %s\nCMake: %s\nРежим: %s\nКаталог: %s\nКонфигурация: %s\n",
        project->generator ? project->generator : "неизвестен", project->cmake_version ? project->cmake_version : "версия неизвестна",
        project->generator ? project->multi_config ? "несколько конфигураций" : "одна конфигурация" : "неизвестен",
        project->build_directory ? project->build_directory : "не выбран",
        project->configuration && *project->configuration ? project->configuration : "не задана");
    if (project->configurations->len) {
        g_string_append(text, "Доступны: ");
        for (guint i = 0; i < project->configurations->len; i++) {
            if (i) g_string_append(text, ", ");
            const char *name = g_ptr_array_index(project->configurations, i); g_string_append(text, *name ? name : "по умолчанию");
        }
        g_string_append_c(text, '\n');
    }
    g_string_append(text, "\nКомпиляторы:\n");
    if (!project->compilers->len) g_string_append(text, "Неизвестны — запросите toolchains-v1\n");
    for (guint i = 0; i < project->compilers->len; i++) {
        ProjectCompiler *compiler = g_ptr_array_index(project->compilers, i);
        g_string_append_printf(text, "%s: %s · %s\n%s\nИсточник: %s\n", compiler->language,
            compiler->id ? compiler->id : "тип неизвестен", compiler->version ? compiler->version : "версия неизвестна",
            compiler->path ? compiler->path : "путь неизвестен", compiler->source);
    }
    if (target_index >= 0 && (guint)target_index < project->targets->len) {
        ProjectTarget *target = g_ptr_array_index(project->targets, (guint)target_index);
        g_string_append_printf(text, "\nЦель: %s\nТип: %s\nРезультат: %s\n", target->name, target->type,
            target->artifact ? target->artifact : "не указан");
    }
    g_string_append(text, "\nПресеты (объявления, не активная сборка):\n");
    guint shown = 0;
    for (guint i = 0; i < project->presets->len; i++) {
        ProjectPreset *preset = g_ptr_array_index(project->presets, i); if (preset->hidden) continue;
        if (++shown > 100) { g_string_append(text, "Остальные доступны в JSON\n"); break; }
        g_string_append_printf(text, "%s [%s]\n%s · %s\nКаталог: %s\n", preset->name, preset->kind,
            preset->generator ? preset->generator : "генератор не указан", preset->configuration ? preset->configuration : "конфигурация не указана",
            preset->binary_directory ? preset->binary_directory : "не указан");
    }
    if (!shown) g_string_append(text, "Не найдены\n");
    g_string_append(text, "\nМетаданные читаются с диска. Для обновления сначала повторите конфигурацию своего проекта.");
    return g_string_free(text, FALSE);
}
