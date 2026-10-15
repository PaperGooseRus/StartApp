#include "extra_metadata.h"
#include "document.h"
#include <json-glib/json-glib.h>

static const char *string(JsonObject *o, const char *key)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : NULL;
}
static JsonArray *array(JsonObject *o, const char *key)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL;
}
static ProjectTarget *target_new(const char *name, const char *id, const char *type)
{
    ProjectTarget *t = g_new0(ProjectTarget, 1); t->name = g_strdup(name); t->id = g_strdup(id); t->type = g_strdup(type);
    t->sources = g_array_new(FALSE, FALSE, sizeof(guint)); return t;
}
void extra_metadata_import(Project *p)
{
    if (p->generator || p->targets->len) return;
    if (p->build_directory) {
        g_autofree char *path = g_build_filename(p->build_directory, "meson-info", "intro-targets.json", NULL);
        g_autoptr(GFile) file = g_file_new_for_path(path);
        g_autoptr(GFileInfo) info = g_file_query_info(file, "standard::type,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
        g_autoptr(JsonParser) parser = json_parser_new();
        if (info && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR && g_file_info_get_size(info) <= 16*1024*1024 && json_parser_load_from_file(parser, path, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            JsonArray *targets = JSON_NODE_HOLDS_ARRAY(root) ? json_node_get_array(root) : NULL;
            if (targets) p->generator = g_strdup("Meson (introspection)");
            for (guint i = 0; targets && i < MIN(json_array_get_length(targets), 2000u); i++) {
                JsonNode *n = json_array_get_element(targets, i); if (!JSON_NODE_HOLDS_OBJECT(n)) continue;
                JsonObject *o = json_node_get_object(n); const char *name = string(o, "name"), *type = string(o, "type");
                if (!name || !type) continue;
                ProjectTarget *t = target_new(name, string(o, "id"), g_str_equal(type, "executable") ? "EXECUTABLE" : type);
                JsonArray *filenames = array(o, "filename");
                JsonNode *first = filenames && json_array_get_length(filenames) ? json_array_get_element(filenames, 0) : NULL;
                if (first && JSON_NODE_HOLDS_VALUE(first) && json_node_get_value_type(first) == G_TYPE_STRING) t->artifact = g_canonicalize_filename(json_node_get_string(first), p->build_directory);
                const char *definition = string(o, "defined_in");
                gint def = definition ? project_find_absolute(p, definition) : -1;
                if (def >= 0) t->definition = g_strdup(((ProjectFile *)g_ptr_array_index(p->files, (guint)def))->path);
                t->line = 1;
                JsonArray *groups = array(o, "target_sources");
                for (guint j = 0; groups && j < MIN(json_array_get_length(groups), 2000u); j++) {
                    JsonNode *group = json_array_get_element(groups, j); if (!JSON_NODE_HOLDS_OBJECT(group)) continue;
                    JsonArray *sources = array(json_node_get_object(group), "sources");
                    for (guint k = 0; sources && k < MIN(json_array_get_length(sources), 10000u); k++) {
                        JsonNode *src = json_array_get_element(sources, k);
                        if (!JSON_NODE_HOLDS_VALUE(src) || json_node_get_value_type(src) != G_TYPE_STRING) continue;
                        gint index = project_find_absolute(p, json_node_get_string(src));
                        if (index >= 0) { guint value = (guint)index; g_array_append_val(t->sources, value); }
                    }
                }
                g_ptr_array_add(p->targets, t);
            }
            if (targets) { g_ptr_array_add(p->notes, g_strdup("Meson: цели/исходники из intro-targets.json; межцелевые зависимости не выводятся по именам библиотек.")); return; }
        }
    }
    const char *names[] = {"GNUmakefile", "makefile", "Makefile"};
    g_autoptr(GRegex) pattern = g_regex_new("^([A-Za-z_][A-Za-z0-9_./-]*)[ \\t]*:(?!=|:)(.*)$", 0, 0, NULL);
    g_autoptr(GHashTable) ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GPtrArray) deps = g_ptr_array_new_with_free_func(g_free);
    for (guint f = 0; f < G_N_ELEMENTS(names); f++) {
        if (project_find_file(p, names[f]) < 0) continue;
        g_autofree char *path = g_build_filename(p->root, names[f], NULL); Document *doc = document_open(path, NULL);
        if (!doc) continue;
        g_auto(GStrv) lines = g_strsplit(doc->text, "\n", -1);
        for (guint i = 0; lines[i] && p->targets->len < 2000; i++) {
            g_autoptr(GMatchInfo) match = NULL; if (!g_regex_match(pattern, lines[i], 0, &match)) continue;
            g_autofree char *name = g_match_info_fetch(match, 1), *rhs = g_match_info_fetch(match, 2);
            if (g_hash_table_contains(ids, name)) continue;
            ProjectTarget *t = target_new(name, name, "MAKE_STATIC"); t->definition = g_strdup(names[f]); t->line = i+1;
            g_ptr_array_add(p->targets, t); g_ptr_array_add(deps, g_strdup(rhs));
            g_hash_table_insert(ids, g_strdup(name), GUINT_TO_POINTER(p->targets->len));
        }
        document_free(doc); break; /* GNU make chooses one root makefile. */
    }
    for (guint i = 0; i < deps->len; i++) {
        g_auto(GStrv) words = g_strsplit_set(g_ptr_array_index(deps, i), " \t\r", -1);
        for (guint j = 0; words[j]; j++) {
            if (*words[j] == '#') break;
            guint found = GPOINTER_TO_UINT(g_hash_table_lookup(ids, words[j]));
            if (!found) continue;
            ProjectTarget *t = g_ptr_array_index(p->targets, i); ProjectTargetEdge *e = g_new0(ProjectTargetEdge, 1);
            e->from = i; e->to = found-1; e->definition = g_strdup(t->definition); e->line = t->line; g_ptr_array_add(p->target_edges, e);
        }
    }
    if (p->targets->len) { p->generator = g_strdup("Make (static approximation)"); g_ptr_array_add(p->notes, g_strdup("Make: только простые буквальные цели/зависимости корневого Makefile; переменные, include, шаблоны и условия не вычисляются.")); }
}
