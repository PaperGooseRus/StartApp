#include "build_metadata.h"
#include <json-glib/json-glib.h>
#include <string.h>

#define JSON_LIMIT (16u * 1024u * 1024u)
#define ENTRY_LIMIT 20000u
#define TARGET_LIMIT 2000u

static JsonNode *member(JsonObject *object, const char *name)
{ return object && json_object_has_member(object, name) ? json_object_get_member(object, name) : NULL; }
static JsonObject *object_node(JsonNode *node)
{ return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL; }
static JsonArray *array_node(JsonNode *node)
{ return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL; }
static const char *string_node(JsonNode *node)
{ return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL; }
static const char *string_member(JsonObject *object, const char *name)
{ return string_node(member(object, name)); }
static gint64 number_member(JsonObject *object, const char *name, gint64 fallback)
{
    JsonNode *node = member(object, name);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 ?
        json_node_get_int(node) : fallback;
}
static gboolean boolean_member(JsonObject *object, const char *name)
{
    JsonNode *node = member(object, name);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}
static JsonObject *element(JsonArray *array, guint index)
{ return array && index < json_array_get_length(array) ? object_node(json_array_get_element(array, index)) : NULL; }

void build_metadata_compiler_free(gpointer data)
{
    ProjectCompiler *compiler = data;
    g_free(compiler->language); g_free(compiler->id); g_free(compiler->version);
    g_free(compiler->path); g_free(compiler->source); g_free(compiler);
}
void build_metadata_preset_free(gpointer data)
{
    ProjectPreset *preset = data;
    g_free(preset->name); g_free(preset->display_name); g_free(preset->kind); g_free(preset->generator);
    g_free(preset->binary_directory); g_free(preset->configuration); g_free(preset->configure_preset);
    g_free(preset->source); g_free(preset);
}

static void note(Project *project, const char *path, const char *message)
{ g_ptr_array_add(project->notes, g_strdup_printf("%s: %s", path, message)); }

static JsonParser *read_json(Project *project, const char *path)
{
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GError) error = NULL;
    g_autoptr(GFileInfo) info = g_file_query_info(file,
        G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
    if (!info) { note(project, path, error->message); return NULL; }
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR || g_file_info_get_size(info) > JSON_LIMIT) {
        note(project, path, "metadata must be a regular JSON file up to 16 MiB"); return NULL;
    }
    g_autofree char *data = NULL;
    gsize length;
    if (!g_file_load_contents(file, NULL, &data, &length, NULL, &error)) {
        note(project, path, error->message); return NULL;
    }
    if (length > JSON_LIMIT || memchr(data, 0, length) || !g_utf8_validate(data, (gssize)length, NULL)) {
        note(project, path, "invalid UTF-8 JSON metadata"); return NULL;
    }
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, (gssize)length, &error)) {
        note(project, path, error->message); g_object_unref(parser); return NULL;
    }
    return parser;
}

char **build_metadata_split_command(const char *command, gboolean windows, GError **error)
{
    if (!windows) {
        char **arguments = NULL;
        if (!g_shell_parse_argv(command, NULL, &arguments, error)) return NULL;
        return arguments;
    }
    /* Windows CRT quoting: backslashes escape quotes only, not path characters. */
    GPtrArray *arguments = g_ptr_array_new_with_free_func(g_free);
    const char *p = command;
    while (*p) {
        while (g_ascii_isspace(*p)) p++;
        if (!*p) break;
        GString *word = g_string_new(NULL);
        gboolean quoted = FALSE;
        while (*p && (quoted || !g_ascii_isspace(*p))) {
            guint backslashes = 0;
            while (*p == '\\') { backslashes++; p++; }
            if (*p == '"') {
                for (guint i = 0; i < backslashes / 2; i++) g_string_append_c(word, '\\');
                if (backslashes % 2) g_string_append_c(word, '"');
                else if (quoted && p[1] == '"') { g_string_append_c(word, '"'); p++; }
                else quoted = !quoted;
                p++;
            } else {
                for (guint i = 0; i < backslashes; i++) g_string_append_c(word, '\\');
                if (*p && (quoted || !g_ascii_isspace(*p))) g_string_append_c(word, *p++);
            }
        }
        if (quoted) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "unclosed quote in Windows command");
            g_string_free(word, TRUE); g_ptr_array_unref(arguments); return NULL;
        }
        g_ptr_array_add(arguments, g_string_free(word, FALSE));
    }
    g_ptr_array_add(arguments, NULL);
    return (char **)g_ptr_array_free(arguments, FALSE);
}

static void ensure_settings(ProjectFile *file, const char *source, gboolean replace)
{
    if (!file->quote_dirs) {
        file->quote_dirs = g_ptr_array_new_with_free_func(g_free);
        file->include_dirs = g_ptr_array_new_with_free_func(g_free);
        file->system_dirs = g_ptr_array_new_with_free_func(g_free);
        file->after_dirs = g_ptr_array_new_with_free_func(g_free);
    }
    if (replace) {
        g_ptr_array_set_size(file->quote_dirs, 0);
        g_ptr_array_set_size(file->include_dirs, 0);
        g_ptr_array_set_size(file->system_dirs, 0);
        g_ptr_array_set_size(file->after_dirs, 0);
    }
    file->compile_settings = TRUE;
    g_free(file->settings_source);
    file->settings_source = g_strdup(source);
}

static void add_directory(GPtrArray *array, const char *path, const char *base)
{
    g_autofree char *canonical = g_canonicalize_filename(path, base);
    for (guint i = 0; i < array->len; i++)
        if (g_str_equal(canonical, g_ptr_array_index(array, i))) return;
    g_ptr_array_add(array, g_steal_pointer(&canonical));
}

static const char *command_configuration(char **args)
{
    for (guint i = 0; args[i]; i++) {
        const char *p = args[i];
        if (g_str_has_prefix(p, "-DCMAKE_INTDIR=")) return p + 15;
        if (g_str_has_prefix(p, "/DCMAKE_INTDIR=")) return p + 15;
    }
    return NULL;
}

static void import_commands(Project *project, const char *path)
{
    g_autoptr(JsonParser) parser = read_json(project, path);
    if (!parser) return;
    JsonArray *entries = array_node(json_parser_get_root(parser));
    if (!entries) { note(project, path, "compile_commands root must be an array"); return; }
    g_autofree char *parent = g_path_get_dirname(path);
    g_autoptr(GHashTable) imported = g_hash_table_new(g_direct_hash, g_direct_equal);
    guint length = json_array_get_length(entries);
    gboolean command_compilers = project->compilers->len == 0;
    if (length > ENTRY_LIMIT) note(project, path, "compile command entry limit reached");
    for (guint i = 0; i < MIN(length, ENTRY_LIMIT); i++) {
        JsonObject *entry = element(entries, i);
        const char *file_path = string_member(entry, "file");
        const char *directory = string_member(entry, "directory");
        if (!file_path || !directory) { note(project, path, "entry lacks string file/directory"); continue; }
        g_autofree char *base = g_canonicalize_filename(directory, parent);
        g_autofree char *absolute = g_canonicalize_filename(file_path, base);
        gint index = project_find_absolute(project, absolute);
        if (index < 0) continue; /* Generated and external sources are not editable project files. */
        g_auto(GStrv) args = NULL;
        JsonArray *array = array_node(member(entry, "arguments"));
        if (array) {
            guint count = json_array_get_length(array);
            if (!count || count > 10000) { note(project, path, "invalid arguments count"); continue; }
            args = g_new0(char *, count + 1);
            gboolean valid = TRUE;
            for (guint j = 0; j < count; j++) {
                const char *value = string_node(json_array_get_element(array, j));
                if (!value) { valid = FALSE; break; }
                args[j] = g_strdup(value);
            }
            if (!valid) { note(project, path, "arguments must contain only strings"); continue; }
        } else {
            const char *command = string_member(entry, "command");
            g_autoptr(GError) error = NULL;
            if (!command) { note(project, path, "entry lacks arguments/command"); continue; }
            gboolean windows = strchr(directory, '\\') != NULL ||
                (g_ascii_isalpha(directory[0]) && directory[1] == ':');
#ifdef G_OS_WIN32
            windows = TRUE;
#endif
            args = build_metadata_split_command(command, windows, &error);
            if (!args) { note(project, path, error->message); continue; }
        }
        if (!args[0] || !*args[0]) { note(project, path, "empty compile command"); continue; }
        const char *configuration = command_configuration(args);
        if (configuration && project->configuration && *project->configuration) {
            g_autofree char *name = g_strdup(configuration);
            g_strdelimit(name, "\"", ' ');
            g_strstrip(name);
            if (!g_str_equal(name, project->configuration)) continue;
        }
        ProjectFile *file = g_ptr_array_index(project->files, (guint)index);
        if (!configuration && file->compile_settings && project->configurations->len > 1) continue;
        if (g_hash_table_contains(imported, GUINT_TO_POINTER((guint)index + 1))) {
            note(project, file->path, "multiple compile commands; first matching entry used"); continue;
        }
        g_hash_table_add(imported, GUINT_TO_POINTER((guint)index + 1));
        if (command_compilers) {
            const char *command = args[0];
            g_autofree char *exe = g_path_get_basename(command);
            if ((g_str_equal(exe, "ccache") || g_str_equal(exe, "sccache")) && args[1]) command = args[1];
            g_autofree char *base_name = g_path_get_basename(command);
            const char *language = g_str_has_suffix(file->path, ".cpp") || g_str_has_suffix(file->path, ".cc") ||
                g_str_has_suffix(file->path, ".cxx") ? "CXX" : "C";
            gboolean duplicate = FALSE;
            for (guint k = 0; k < project->compilers->len; k++) {
                ProjectCompiler *old = g_ptr_array_index(project->compilers, k);
                if (g_strcmp0(old->path, command) == 0 && g_str_equal(old->language, language)) duplicate = TRUE;
            }
            if (!duplicate && project->compilers->len < 32) {
                ProjectCompiler *compiler = g_new0(ProjectCompiler, 1);
                compiler->language = g_strdup(language); compiler->path = g_strdup(command);
                compiler->id = g_strdup(base_name); compiler->source = g_strdup("compile_commands.json (command name)");
                g_ptr_array_add(project->compilers, compiler);
            }
        }
        ensure_settings(file, path, TRUE);
        g_free(file->compile_options); file->compile_options = g_strjoinv(" ", args);
        for (guint j = 1; args[j]; j++) {
            const char *flag = args[j], *value = NULL;
            if (g_str_equal(flag, "-I-")) {
                note(project, file->path, "legacy -I- search semantics are not supported"); continue;
            }
            GPtrArray *destination = NULL;
            const struct { const char *name; guint kind; } flags[] = {
                {"-iquote", 0}, {"-isystem", 2}, {"-idirafter", 3},
                {"/external:I", 2}, {"/imsvc", 2}, {"-I", 1}, {"/I", 1}
            };
            for (guint k = 0; k < G_N_ELEMENTS(flags); k++) {
                if (!g_str_has_prefix(flag, flags[k].name)) continue;
                value = flag + strlen(flags[k].name);
                destination = flags[k].kind == 0 ? file->quote_dirs :
                              flags[k].kind == 1 ? file->include_dirs :
                              flags[k].kind == 2 ? file->system_dirs : file->after_dirs;
                if (!*value && args[j + 1]) value = args[++j];
                break;
            }
            if (destination && value && *value) {
                if (*value == '=') note(project, file->path, "sysroot-relative include path is not expanded");
                else add_directory(destination, value, base);
            } else if (*flag == '@') note(project, file->path, "response files are not expanded");
        }
    }
}

void build_metadata_target_free(gpointer data)
{
    ProjectTarget *target = data;
    g_free(target->id); g_free(target->name); g_free(target->type);
    g_free(target->artifact); g_free(target->definition);
    g_array_unref(target->sources); g_free(target);
}
void build_metadata_edge_free(gpointer data)
{
    ProjectTargetEdge *edge = data;
    g_free(edge->definition); g_free(edge);
}

static JsonParser *reply_json(Project *project, const char *reply, const char *name)
{
    if (!name || !*name || strchr(name, '/') || strchr(name, '\\') ||
        g_str_equal(name, ".") || g_str_equal(name, "..") || !g_str_has_suffix(name, ".json")) {
        note(project, reply, "invalid reply filename; path traversal rejected"); return NULL;
    }
    g_autofree char *path = g_build_filename(reply, name, NULL);
    return read_json(project, path);
}

static char *backtrace(Project *project, JsonObject *target, gint64 index,
                       const char *source_root, guint *line)
{
    JsonObject *graph = object_node(member(target, "backtraceGraph"));
    JsonArray *nodes = array_node(member(graph, "nodes"));
    JsonArray *files = array_node(member(graph, "files"));
    if (index < 0 || !nodes || (guint64)index >= json_array_get_length(nodes)) return NULL;
    JsonObject *node = element(nodes, (guint)index);
    gint64 file_index = number_member(node, "file", -1);
    gint64 source_line = number_member(node, "line", 0);
    if (!files || file_index < 0 || (guint64)file_index >= json_array_get_length(files)) return NULL;
    const char *name = string_node(json_array_get_element(files, (guint)file_index));
    if (!name) return NULL;
    g_autofree char *absolute = g_canonicalize_filename(name, source_root);
    gint found = project_find_absolute(project, absolute);
    if (found < 0) return NULL;
    ProjectFile *file = g_ptr_array_index(project->files, (guint)found);
    *line = source_line > 0 && source_line <= G_MAXUINT ? (guint)source_line : 1;
    return g_strdup(file->path);
}

static void import_toolchains(Project *project, const char *reply, const char *filename)
{
    g_autoptr(JsonParser) parser = reply_json(project, reply, filename);
    if (!parser) return;
    JsonArray *toolchains = array_node(member(object_node(json_parser_get_root(parser)), "toolchains"));
    for (guint i = 0; toolchains && i < MIN(32u, json_array_get_length(toolchains)); i++) {
        JsonObject *toolchain = element(toolchains, i), *data = object_node(member(toolchain, "compiler"));
        const char *language = string_member(toolchain, "language");
        if (!language || !data) continue;
        ProjectCompiler *compiler = g_new0(ProjectCompiler, 1);
        compiler->language = g_strdup(language); compiler->id = g_strdup(string_member(data, "id"));
        compiler->version = g_strdup(string_member(data, "version")); compiler->path = g_strdup(string_member(data, "path"));
        compiler->source = g_strdup("CMake File API toolchains-v1");
        g_ptr_array_add(project->compilers, compiler);
    }
}

typedef struct { guint from; char *id, *definition; guint line; } PendingEdge;
static void pending_free(gpointer data)
{
    PendingEdge *edge = data;
    g_free(edge->id); g_free(edge->definition); g_free(edge);
}

static void import_cmake(Project *project, const char *directory, const char *configuration)
{
    g_autofree char *reply = g_build_filename(directory, ".cmake", "api", "v1", "reply", NULL);
    g_autoptr(GDir) dir = g_dir_open(reply, 0, NULL);
    if (!dir) return;
    g_autofree char *latest = NULL;
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        if (g_str_has_prefix(name, "index-") && g_str_has_suffix(name, ".json") &&
            (!latest || strcmp(name, latest) > 0)) { g_free(latest); latest = g_strdup(name); }
    }
    if (!latest) { note(project, reply, "no File API reply index"); return; }
    g_autoptr(JsonParser) index = reply_json(project, reply, latest);
    if (!index) return;
    JsonObject *root = object_node(json_parser_get_root(index));
    JsonObject *cmake = object_node(member(root, "cmake"));
    JsonObject *generator = object_node(member(cmake, "generator"));
    project->generator = g_strdup(string_member(generator, "name"));
    project->multi_config = boolean_member(generator, "multiConfig");
    project->cmake_version = g_strdup(string_member(object_node(member(cmake, "version")), "string"));
    JsonArray *objects = array_node(member(root, "objects"));
    const char *codemodel_file = NULL;
    for (guint i = 0; objects && i < json_array_get_length(objects); i++) {
        JsonObject *object = element(objects, i);
        if (g_strcmp0(string_member(object, "kind"), "toolchains") == 0 &&
            number_member(object_node(member(object, "version")), "major", 0) == 1)
            import_toolchains(project, reply, string_member(object, "jsonFile"));
        if (g_strcmp0(string_member(object, "kind"), "codemodel") == 0 &&
            number_member(object_node(member(object, "version")), "major", 0) == 2)
            codemodel_file = string_member(object, "jsonFile");
    }
    if (!codemodel_file) { note(project, reply, "request codemodel-v2 and reconfigure CMake"); return; }
    g_autoptr(JsonParser) model = reply_json(project, reply, codemodel_file);
    if (!model) return;
    JsonObject *model_root = object_node(json_parser_get_root(model));
    JsonObject *paths = object_node(member(model_root, "paths"));
    const char *source_path = string_member(paths, "source");
    if (!source_path) { note(project, reply, "codemodel lacks source root"); return; }
    g_autofree char *source_root = g_canonicalize_filename(source_path, directory);
    JsonArray *configurations = array_node(member(model_root, "configurations"));
    JsonObject *selected = NULL;
    for (guint i = 0; configurations && i < json_array_get_length(configurations); i++) {
        JsonObject *config = element(configurations, i);
        const char *config_name = string_member(config, "name");
        if (!config_name) continue;
        g_ptr_array_add(project->configurations, g_strdup(config_name));
        if ((!configuration && !selected) || g_strcmp0(configuration, config_name) == 0) selected = config;
    }
    if (!selected) { note(project, reply, "requested CMake configuration is unavailable"); return; }
    project->configuration = g_strdup(string_member(selected, "name"));
    JsonArray *targets = array_node(member(selected, "targets"));
    g_autoptr(GHashTable) target_index_map = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GPtrArray) pending = g_ptr_array_new_with_free_func(pending_free);
    guint count = targets ? json_array_get_length(targets) : 0;
    if (count > TARGET_LIMIT) note(project, reply, "target limit reached");
    for (guint i = 0; i < MIN(count, TARGET_LIMIT); i++) {
        JsonObject *entry = element(targets, i);
        g_autoptr(JsonParser) parser = reply_json(project, reply, string_member(entry, "jsonFile"));
        if (!parser) continue;
        JsonObject *data = object_node(json_parser_get_root(parser));
        const char *id = string_member(data, "id"), *target_name = string_member(data, "name");
        const char *type = string_member(data, "type");
        if (!id || !target_name || !type) { note(project, reply, "target lacks id/name/type"); continue; }
        if (g_hash_table_contains(target_index_map, id)) { note(project, reply, "duplicate target id"); continue; }
        ProjectTarget *target = g_new0(ProjectTarget, 1);
        target->id = g_strdup(id); target->name = g_strdup(target_name); target->type = g_strdup(type);
        target->sources = g_array_new(FALSE, FALSE, sizeof(guint));
        target->definition = backtrace(project, data, number_member(data, "backtrace", -1), source_root, &target->line);
        JsonArray *artifacts = array_node(member(data, "artifacts"));
        const char *artifact = string_member(element(artifacts, 0), "path");
        if (artifact) target->artifact = g_canonicalize_filename(artifact, directory);
        guint target_index = project->targets->len;
        g_ptr_array_add(project->targets, target);
        g_hash_table_insert(target_index_map, target->id, GUINT_TO_POINTER(target_index + 1));
        JsonArray *dependencies = array_node(member(data, "dependencies"));
        for (guint j = 0; dependencies && j < json_array_get_length(dependencies) && j < TARGET_LIMIT; j++) {
            JsonObject *dependency = element(dependencies, j);
            const char *dependency_id = string_member(dependency, "id");
            if (!dependency_id) continue;
            PendingEdge *edge = g_new0(PendingEdge, 1);
            edge->from = target_index; edge->id = g_strdup(dependency_id);
            edge->definition = backtrace(project, data, number_member(dependency, "backtrace", -1), source_root, &edge->line);
            g_ptr_array_add(pending, edge);
        }
        JsonArray *sources = array_node(member(data, "sources"));
        JsonArray *groups = array_node(member(data, "compileGroups"));
        for (guint j = 0; sources && j < json_array_get_length(sources) && j < ENTRY_LIMIT; j++) {
            JsonObject *source = element(sources, j);
            const char *source_file = string_member(source, "path");
            if (!source_file) continue;
            g_autofree char *absolute = g_canonicalize_filename(source_file, source_root);
            gint file_index = project_find_absolute(project, absolute);
            if (file_index < 0) continue;
            guint value = (guint)file_index;
            g_array_append_val(target->sources, value);
            gint64 group_index = number_member(source, "compileGroupIndex", -1);
            if (group_index < 0 || !groups || (guint64)group_index >= json_array_get_length(groups)) continue;
            ProjectFile *file = g_ptr_array_index(project->files, value);
            if (file->compile_settings) {
                note(project, file->path, "multiple CMake compile groups; first group used"); continue;
            }
            ensure_settings(file, "CMake File API", FALSE);
            JsonObject *cg = element(groups, (guint)group_index);
            GString *options = g_string_new(NULL);
            JsonArray *fragments = array_node(member(cg, "compileCommandFragments"));
            for (guint k = 0; fragments && k < MIN(json_array_get_length(fragments), ENTRY_LIMIT); k++) {
                const char *fragment = string_member(element(fragments, k), "fragment");
                if (fragment && options->len < 65536) g_string_append_printf(options, "%s ", fragment);
            }
            JsonArray *defines = array_node(member(cg, "defines"));
            for (guint k = 0; defines && k < MIN(json_array_get_length(defines), ENTRY_LIMIT); k++) {
                const char *define = string_member(element(defines, k), "define");
                if (define && options->len < 65536) g_string_append_printf(options, "-D%s ", define);
            }
            file->compile_options = g_string_free(options, FALSE);
            JsonArray *includes = array_node(member(cg, "includes"));
            for (guint k = 0; includes && k < json_array_get_length(includes) && k < ENTRY_LIMIT; k++) {
                JsonObject *include = element(includes, k);
                const char *include_path = string_member(include, "path");
                JsonNode *system = member(include, "isSystem");
                gboolean is_system = system && JSON_NODE_HOLDS_VALUE(system) &&
                    json_node_get_value_type(system) == G_TYPE_BOOLEAN && json_node_get_boolean(system);
                if (include_path) add_directory(is_system ? file->system_dirs : file->include_dirs, include_path, source_root);
            }
        }
    }
    for (guint i = 0; i < pending->len; i++) {
        PendingEdge *edge = g_ptr_array_index(pending, i);
        guint encoded = GPOINTER_TO_UINT(g_hash_table_lookup(target_index_map, edge->id));
        gint found = encoded ? (gint)encoded - 1 : -1;
        if (found < 0) { note(project, edge->id, "CMake dependency target is unavailable"); continue; }
        ProjectTargetEdge *result = g_new0(ProjectTargetEdge, 1);
        result->from = edge->from; result->to = (guint)found;
        result->definition = g_strdup(edge->definition); result->line = edge->line;
        g_ptr_array_add(project->target_edges, result);
    }
}

static gboolean has_metadata(const char *directory)
{
    g_autofree char *commands = g_build_filename(directory, "compile_commands.json", NULL);
    g_autofree char *reply = g_build_filename(directory, ".cmake", "api", "v1", "reply", NULL);
    g_autofree char *cache = g_build_filename(directory, "CMakeCache.txt", NULL);
    g_autofree char *meson = g_build_filename(directory, "meson-info", "intro-targets.json", NULL);
    return g_file_test(meson, G_FILE_TEST_IS_REGULAR) || g_file_test(commands, G_FILE_TEST_IS_REGULAR) || g_file_test(reply, G_FILE_TEST_IS_DIR) || g_file_test(cache, G_FILE_TEST_IS_REGULAR);
}

static void import_cache(Project *project)
{
    g_autofree char *path = g_build_filename(project->build_directory, "CMakeCache.txt", NULL);
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    if (!info) return;
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR || g_file_info_get_size(info) > 2 * 1024 * 1024) {
        note(project, path, "cache must be a regular UTF-8 file up to 2 MiB"); return;
    }
    g_autofree char *text = NULL; gsize length;
    if (!g_file_load_contents(file, NULL, &text, &length, NULL, NULL) || length > 2 * 1024 * 1024 ||
        memchr(text, 0, length) || !g_utf8_validate(text, (gssize)length, NULL)) { note(project, path, "invalid CMake cache"); return; }
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    gboolean compiler_fallback = project->compilers->len == 0;
    for (guint i = 0; lines[i]; i++) {
        char *colon = strchr(lines[i], ':'), *equal = colon ? strchr(colon, '=') : NULL;
        if (!colon || !equal || lines[i][0] == '#' || lines[i][0] == '/') continue;
        *colon = 0; char *value = g_strstrip(equal + 1);
        if (!project->generator && g_str_equal(lines[i], "CMAKE_GENERATOR")) project->generator = g_strdup(value);
        if (!project->configuration && g_str_equal(lines[i], "CMAKE_BUILD_TYPE")) project->configuration = g_strdup(value);
        if (!project->configurations->len && g_str_equal(lines[i], "CMAKE_CONFIGURATION_TYPES") && *value) {
            g_auto(GStrv) configs = g_strsplit(value, ";", -1);
            for (guint j = 0; configs[j] && j < 32; j++) if (*configs[j]) g_ptr_array_add(project->configurations, g_strdup(configs[j]));
            project->multi_config = TRUE;
        }
        if (compiler_fallback && project->compilers->len < 32 && (g_str_equal(lines[i], "CMAKE_C_COMPILER") || g_str_equal(lines[i], "CMAKE_CXX_COMPILER"))) {
            ProjectCompiler *compiler = g_new0(ProjectCompiler, 1);
            compiler->language = g_strdup(g_str_equal(lines[i], "CMAKE_C_COMPILER") ? "C" : "CXX");
            compiler->path = g_strdup(value); compiler->source = g_strdup("CMakeCache.txt");
            g_ptr_array_add(project->compilers, compiler);
        }
    }
    if (!project->configurations->len && project->configuration) g_ptr_array_add(project->configurations, g_strdup(project->configuration));
}

typedef struct { JsonObject *data; char *kind, *source; } PresetSpec;
static void spec_free(gpointer data)
{
    PresetSpec *spec = data; json_object_unref(spec->data); g_free(spec->kind); g_free(spec->source); g_free(spec);
}
static void collect_presets(Project *project, const char *path, GPtrArray *specs, GHashTable *visited, guint depth)
{
    if (g_hash_table_contains(visited, path)) return;
    if (depth > 8 || g_hash_table_size(visited) >= 32) { note(project, path, "preset include limit reached"); return; }
    g_hash_table_add(visited, g_strdup(path));
    g_autoptr(JsonParser) parser = read_json(project, path); if (!parser) return;
    JsonObject *root = object_node(json_parser_get_root(parser));
    if (!root) { note(project, path, "preset root must be an object"); return; }
    const char *keys[] = {"configurePresets", "buildPresets"};
    for (guint k = 0; k < G_N_ELEMENTS(keys); k++) {
        JsonArray *array = array_node(member(root, keys[k]));
        for (guint i = 0; array && i < json_array_get_length(array) && specs->len < 1000; i++) {
            JsonObject *data = element(array, i);
            if (!string_member(data, "name")) { note(project, path, "preset lacks a string name"); continue; }
            PresetSpec *spec = g_new0(PresetSpec, 1); spec->data = json_object_ref(data);
            spec->kind = g_strdup(k ? "build" : "configure"); spec->source = g_strdup(path); g_ptr_array_add(specs, spec);
        }
    }
    JsonArray *includes = array_node(member(root, "include"));
    g_autofree char *parent = g_path_get_dirname(path);
    for (guint i = 0; includes && i < MIN(32u, json_array_get_length(includes)); i++) {
        const char *name = string_node(json_array_get_element(includes, i));
        if (!name || strchr(name, '$')) { note(project, path, "preset include macros are not expanded"); continue; }
        g_autofree char *absolute = g_canonicalize_filename(name, parent);
        g_autoptr(GFile) root_file = g_file_new_for_path(project->root), child = g_file_new_for_path(absolute);
        g_autofree char *relative = g_file_get_relative_path(root_file, child);
        if (!relative) { note(project, path, "preset include outside project rejected"); continue; }
        collect_presets(project, absolute, specs, visited, depth + 1);
    }
}
static PresetSpec *preset_named(GPtrArray *specs, const char *kind, const char *name)
{
    for (guint i = 0; name && i < specs->len; i++) {
        PresetSpec *spec = g_ptr_array_index(specs, i);
        if (g_str_equal(spec->kind, kind) && g_str_equal(string_member(spec->data, "name"), name)) return spec;
    }
    return NULL;
}
static const char *resolve_preset(Project *project, GPtrArray *specs, PresetSpec *spec, const char *key,
                                  GHashTable *visiting, guint *steps, guint depth)
{
    if (!spec) return NULL;
    if (++*steps > 2000 || depth > 32 || g_hash_table_contains(visiting, spec)) {
        if (*steps <= 2001) note(project, spec->source, "preset inheritance cycle or traversal limit");
        return NULL;
    }
    const char *value;
    if (g_str_equal(key, "CMAKE_BUILD_TYPE")) {
        JsonNode *node = member(object_node(member(spec->data, "cacheVariables")), key);
        value = string_node(node);
        if (!value) value = string_member(object_node(node), "value");
    } else value = string_member(spec->data, key);
    if (value) return value;
    g_hash_table_add(visiting, spec);
    JsonNode *inherits = member(spec->data, "inherits");
    const char *single = string_node(inherits);
    if (single) value = resolve_preset(project, specs, preset_named(specs, spec->kind, single), key, visiting, steps, depth + 1);
    JsonArray *parents = array_node(inherits);
    for (guint i = 0; parents && i < MIN(32u, json_array_get_length(parents)); i++) {
        const char *name = string_node(json_array_get_element(parents, i));
        value = resolve_preset(project, specs, preset_named(specs, spec->kind, name), key, visiting, steps, depth + 1);
        if (value || *steps > 2000) break;
    }
    g_hash_table_remove(visiting, spec);
    return value;
}
static const char *preset_value(Project *project, GPtrArray *specs, PresetSpec *spec, const char *key, guint depth)
{
    g_autoptr(GHashTable) visiting = g_hash_table_new(g_direct_hash, g_direct_equal);
    guint steps = 0;
    return resolve_preset(project, specs, spec, key, visiting, &steps, depth);
}
static void import_presets(Project *project)
{
    g_autoptr(GPtrArray) specs = g_ptr_array_new_with_free_func(spec_free);
    g_autoptr(GHashTable) visited = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    const char *files[] = {"CMakePresets.json", "CMakeUserPresets.json"};
    for (guint i = 0; i < G_N_ELEMENTS(files); i++) {
        g_autofree char *path = g_build_filename(project->root, files[i], NULL);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) collect_presets(project, path, specs, visited, 0);
    }
    for (guint i = 0; i < specs->len; i++) {
        PresetSpec *spec = g_ptr_array_index(specs, i);
        ProjectPreset *preset = g_new0(ProjectPreset, 1);
        preset->name = g_strdup(string_member(spec->data, "name")); preset->kind = g_strdup(spec->kind);
        preset->display_name = g_strdup(string_member(spec->data, "displayName"));
        preset->hidden = boolean_member(spec->data, "hidden"); preset->source = g_strdup(spec->source);
        preset->configure_preset = g_strdup(preset_value(project, specs, spec, "configurePreset", 0));
        PresetSpec *configure = g_str_equal(spec->kind, "configure") ? spec : preset_named(specs, "configure", preset->configure_preset);
        preset->generator = g_strdup(preset_value(project, specs, configure, "generator", 0));
        preset->binary_directory = g_strdup(preset_value(project, specs, configure, "binaryDir", 0));
        preset->configuration = g_strdup(g_str_equal(spec->kind, "build") ? preset_value(project, specs, spec, "configuration", 0) :
            preset_value(project, specs, configure, "CMAKE_BUILD_TYPE", 0));
        g_ptr_array_add(project->presets, preset);
    }
}

void build_metadata_import(Project *project, const char *directory, const char *configuration)
{
    import_presets(project);
    if (directory) project->build_directory = g_canonicalize_filename(directory, project->root);
    else {
        const char *candidates[] = {"", "build", "build/windows", "build/linux", NULL};
        for (guint i = 0; candidates[i]; i++) {
            g_autofree char *path = g_build_filename(project->root, candidates[i], NULL);
            if (has_metadata(path)) { project->build_directory = g_steal_pointer(&path); break; }
        }
    }
    if (!project->build_directory) return;
    if (!has_metadata(project->build_directory)) {
        note(project, project->build_directory, "no compile_commands.json or CMake File API reply"); return;
    }
    import_cmake(project, project->build_directory, configuration);
    if (!project->configuration && configuration) project->configuration = g_strdup(configuration);
    import_cache(project);
    g_autofree char *commands = g_build_filename(project->build_directory, "compile_commands.json", NULL);
    if (g_file_test(commands, G_FILE_TEST_IS_REGULAR)) import_commands(project, commands);
    for (guint i = 0; i < project->files->len; i++) {
        ProjectFile *file = g_ptr_array_index(project->files, i);
        project->compile_units += file->compile_settings;
    }
}
