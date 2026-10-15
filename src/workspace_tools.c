#include "workspace_tools.h"
#include "document.h"
#include "graph_layout.h"
#include <glib/gstdio.h>
#include <string.h>

void text_match_free(gpointer data)
{ TextMatch *m = data; g_free(m->path); g_free(m->text); g_free(m); }

GPtrArray *workspace_search(const Project *p, const char *query, GCancellable *cancel, GError **error)
{
    GPtrArray *matches = g_ptr_array_new_with_free_func(text_match_free);
    if (!query || !*query) return matches;
    g_autofree char *needle = g_utf8_casefold(query, -1);
    for (guint i = 0; i < p->files->len && matches->len < 2000; i++) {
        if (g_cancellable_set_error_if_cancelled(cancel, error)) { g_ptr_array_unref(matches); return NULL; }
        ProjectFile *f = g_ptr_array_index(p->files, i);
        g_autofree char *path = g_build_filename(p->root, f->path, NULL);
        Document *doc = document_open(path, NULL);
        if (!doc) continue;
        g_auto(GStrv) lines = g_strsplit(doc->text, "\n", -1);
        for (guint j = 0; lines[j] && matches->len < 2000; j++) {
            g_autofree char *folded = g_utf8_casefold(lines[j], -1);
            if (!strstr(folded, needle)) continue;
            TextMatch *m = g_new0(TextMatch, 1); m->path = g_strdup(f->path); m->line = j + 1;
            m->text = g_utf8_substring(lines[j], 0, MIN(300, g_utf8_strlen(lines[j], -1)));
            g_ptr_array_add(matches, m);
        }
        document_free(doc);
    }
    return matches;
}

static void edge_at(const Project *p, gboolean targets, guint i, guint *from, gint *to)
{
    if (targets) { ProjectTargetEdge *e = g_ptr_array_index(p->target_edges, i); *from = e->from; *to = (gint)e->to; }
    else { ProjectEdge *e = g_ptr_array_index(p->edges, i); *from = e->from; *to = e->to; }
}
GArray *workspace_reachable(const Project *p, gboolean targets, gint focus, guint direction, guint depth)
{
    GArray *found = g_array_new(FALSE, FALSE, sizeof(guint));
    guint count = targets ? p->targets->len : p->files->len;
    if (focus < 0 || (guint)focus >= count) return found;
    g_autofree guint *distance = g_new(guint, count);
    for (guint i = 0; i < count; i++) distance[i] = G_MAXUINT;
    distance[focus] = 0; guint first = (guint)focus; g_array_append_val(found, first);
    guint ne = targets ? p->target_edges->len : p->edges->len;
    g_autoptr(GPtrArray) adjacency = g_ptr_array_new_with_free_func((GDestroyNotify)g_array_unref);
    for (guint i = 0; i < count; i++) g_ptr_array_add(adjacency, g_array_new(FALSE, FALSE, sizeof(guint)));
    for (guint i = 0; i < ne; i++) {
        guint a; gint b; edge_at(p, targets, i, &a, &b);
        if (a >= count || b < 0 || (guint)b >= count) continue;
        guint dest = (guint)b;
        if (direction != 2) g_array_append_val((GArray *)g_ptr_array_index(adjacency, a), dest);
        if (direction != 1) g_array_append_val((GArray *)g_ptr_array_index(adjacency, dest), a);
    }
    for (guint i = 0; i < found->len; i++) {
        guint at = g_array_index(found, guint, i);
        if (depth && distance[at] >= depth) continue;
        GArray *next = g_ptr_array_index(adjacency, at);
        for (guint j = 0; j < next->len; j++) {
            guint n = g_array_index(next, guint, j);
            if (distance[n] != G_MAXUINT) continue;
            distance[n] = distance[at] + 1; g_array_append_val(found, n);
        }
    }
    return found;
}

static const char *node_name(const Project *p, gboolean targets, guint i)
{ return targets ? ((ProjectTarget *)g_ptr_array_index(p->targets, i))->name : ((ProjectFile *)g_ptr_array_index(p->files, i))->path; }
char *workspace_explain(const Project *p, gboolean targets, guint from, guint to)
{
    GString *s = g_string_new(NULL);
    guint count = targets ? p->targets->len : p->files->len;
    if (from >= count || to >= count) return g_string_free(s, FALSE);
    g_string_append_printf(s, "%s → %s\n", node_name(p, targets, from), node_name(p, targets, to));
    guint n = targets ? p->target_edges->len : p->edges->len, shown = 0;
    for (guint i = 0; i < n && shown < 100; i++) {
        guint a; gint b; edge_at(p, targets, i, &a, &b);
        if (a != from || b != (gint)to) continue;
        shown++;
        if (targets) {
            ProjectTargetEdge *e = g_ptr_array_index(p->target_edges, i);
            g_string_append_printf(s, "Источник зависимости: %s:%u\n", e->definition ? e->definition : "строка не указана", e->line);
        } else {
            ProjectEdge *e = g_ptr_array_index(p->edges, i);
            g_string_append_printf(s, "%s:%u: #include %s%s%s\nРазрешение: %s\nКонтекст: %s\n",
                node_name(p, FALSE, from), e->line, e->system ? "<" : "\"", e->include, e->system ? ">" : "\"",
                e->metadata ? "пути компилятора" : "эвристика", e->settings_source ? e->settings_source : "нет метаданных");
        }
    }
    return g_string_free(s, FALSE);
}

char *workspace_architecture(const Project *p, const char *rules, GError **error)
{
    g_autoptr(GKeyFile) keys = g_key_file_new();
    if (rules && *rules && !g_key_file_load_from_data(keys, rules, strlen(rules), G_KEY_FILE_NONE, error)) return NULL;
    gsize count; g_auto(GStrv) groups = g_key_file_get_groups(keys, &count);
    if (count > 128) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Не более 128 правил"); return NULL; }
    GString *s = g_string_new("Проверка статических включений (включая неактивные #if).\n");
    guint violations = 0;
    for (guint g = 0; g < count; g++) {
        g_autofree char *from = g_key_file_get_string(keys, groups[g], "from", NULL);
        g_autofree char *to = g_key_file_get_string(keys, groups[g], "to", NULL);
        if (!from || !*from || !to || !*to) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Правилу %s нужны from и to", groups[g]); g_string_free(s, TRUE); return NULL; }
        for (guint i = 0; i < p->edges->len && violations < 500; i++) {
            ProjectEdge *e = g_ptr_array_index(p->edges, i);
            if (e->to < 0) continue;
            const char *a = node_name(p, FALSE, e->from), *b = node_name(p, FALSE, (guint)e->to);
            if (g_pattern_match_simple(from, a) && g_pattern_match_simple(to, b)) {
                g_string_append_printf(s, "%s: %s:%u → %s\n", groups[g], a, e->line, b); violations++;
            }
        }
    }
    /* Iterative DFS, so deep real-world include chains cannot exhaust the C stack. */
    guint n = p->files->len, cycles = 0;
    g_autofree guint8 *color = g_new0(guint8, n);
    g_autofree guint *position = g_new0(guint, n);
    g_autoptr(GPtrArray) adj = g_ptr_array_new_with_free_func((GDestroyNotify)g_array_unref);
    for (guint i = 0; i < n; i++) g_ptr_array_add(adj, g_array_new(FALSE, FALSE, sizeof(guint)));
    for (guint i = 0; i < p->edges->len; i++) {
        ProjectEdge *e = g_ptr_array_index(p->edges, i);
        if (e->to >= 0) { guint b = (guint)e->to; g_array_append_val((GArray *)g_ptr_array_index(adj, e->from), b); }
    }
    g_autoptr(GArray) stack = g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint root = 0; root < n; root++) {
        if (color[root]) continue;
        color[root] = 1; g_array_append_val(stack, root);
        while (stack->len) {
            guint a = g_array_index(stack, guint, stack->len - 1); GArray *next = g_ptr_array_index(adj, a);
            if (position[a] == next->len) { color[a] = 2; g_array_set_size(stack, stack->len - 1); continue; }
            guint b = g_array_index(next, guint, position[a]++);
            if (color[b] == 1) { if (cycles++ < 100) g_string_append_printf(s, "Цикл: %s → %s (обратная связь DFS)\n", node_name(p, FALSE, a), node_name(p, FALSE, b)); }
            else if (!color[b]) { color[b] = 1; g_array_append_val(stack, b); }
        }
    }
    g_string_append_printf(s, "Нарушений: %u%s · обратных связей циклов: %u\n", violations, violations == 500 ? "+ (лимит)" : "", cycles);
    return g_string_free(s, FALSE);
}

static char *file_signature(const ProjectFile *f)
{
    GString *s = g_string_new(f->compile_options ? f->compile_options : "");
    GPtrArray *groups[] = {f->quote_dirs, f->include_dirs, f->system_dirs, f->after_dirs};
    for (guint g = 0; g < 4; g++) { g_string_append_printf(s, "\n[%u]", g); for (guint i = 0; groups[g] && i < groups[g]->len; i++) g_string_append_printf(s, "\n%s", (char *)g_ptr_array_index(groups[g], i)); }
    return g_string_free(s, FALSE);
}
static gint compare_string(gconstpointer a, gconstpointer b) { return g_strcmp0(*(char *const *)a, *(char *const *)b); }
static GHashTable *identities(const Project *p)
{
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < p->files->len; i++) g_hash_table_add(set, g_strdup_printf("Файл %s", node_name(p, FALSE, i)));
    for (guint i = 0; i < p->targets->len; i++) {
        ProjectTarget *t = g_ptr_array_index(p->targets, i);
        g_hash_table_add(set, g_strdup_printf("Цель %s [%s] → %s", t->name, t->type, t->artifact ? t->artifact : ""));
        for (guint j = 0; j < t->sources->len; j++) g_hash_table_add(set, g_strdup_printf("Исходник цели %s: %s", t->name, node_name(p, FALSE, g_array_index(t->sources, guint, j))));
    }
    for (guint i = 0; i < p->edges->len; i++) {
        ProjectEdge *e = g_ptr_array_index(p->edges, i);
        g_hash_table_add(set, g_strdup_printf("Include %s → %s [%s]", node_name(p, FALSE, e->from), e->to >= 0 ? node_name(p, FALSE, (guint)e->to) : "?", e->include));
    }
    for (guint i = 0; i < p->target_edges->len; i++) {
        ProjectTargetEdge *e = g_ptr_array_index(p->target_edges, i);
        g_hash_table_add(set, g_strdup_printf("Зависимость цели %s → %s", node_name(p, TRUE, e->from), node_name(p, TRUE, e->to)));
    }
    return set;
}
char *workspace_compare(const Project *left, const Project *right)
{
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "A: %s · %s · %s\nB: %s · %s · %s\n", left->root, left->configuration ? left->configuration : "?", left->generator ? left->generator : "?", right->root, right->configuration ? right->configuration : "?", right->generator ? right->generator : "?");
    g_autoptr(GHashTable) a = identities(left), b = identities(right);
    g_autoptr(GPtrArray) changes = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter iter; gpointer key;
    g_hash_table_iter_init(&iter, a); while (g_hash_table_iter_next(&iter, &key, NULL)) if (!g_hash_table_contains(b, key)) g_ptr_array_add(changes, g_strdup_printf("− %s\n", (char *)key));
    g_hash_table_iter_init(&iter, b); while (g_hash_table_iter_next(&iter, &key, NULL)) if (!g_hash_table_contains(a, key)) g_ptr_array_add(changes, g_strdup_printf("+ %s\n", (char *)key));
    for (guint i = 0; i < left->files->len; i++) {
        ProjectFile *f = g_ptr_array_index(left->files, i); gint j = project_find_file(right, f->path);
        if (j < 0) continue;
        g_autofree char *x = file_signature(f), *y = file_signature(g_ptr_array_index(right->files, (guint)j));
        if (g_strcmp0(x, y)) g_ptr_array_add(changes, g_strdup_printf("~ Параметры %s\nA: %s\nB: %s\n", f->path, x, y));
    }
    g_ptr_array_sort(changes, compare_string);
    for (guint i = 0; i < MIN(changes->len, 2000u); i++) g_string_append(s, g_ptr_array_index(changes, i));
    g_string_append_printf(s, "Изменений: %u%s\n", changes->len, changes->len > 2000 ? " · показаны первые 2000" : "");
    return g_string_free(s, FALSE);
}

char *workspace_export(const Project *p, gboolean html, gboolean targets)
{
    guint total = targets ? p->targets->len : p->files->len, n = MIN(total, GRAPH_LAYOUT_LIMIT);
    g_autoptr(GArray) edges = g_array_new(FALSE, FALSE, sizeof(LayoutEdge));
    guint ne = targets ? p->target_edges->len : p->edges->len;
    for (guint i = 0; i < ne; i++) { guint a; gint b; edge_at(p, targets, i, &a, &b); if (a < n && b >= 0 && (guint)b < n) { LayoutEdge e = {a, (guint)b}; g_array_append_val(edges, e); } }
    GraphLayout *layout = graph_layout_build(n, edges);
    GString *s = g_string_new(html ? "<!doctype html><html lang=\"ru\"><meta charset=\"utf-8\"><title>StartApp · карта</title><style>body{background:#181818;color:#ece9e4;font:16px sans-serif}input{padding:10px;background:#303030;color:inherit}svg{display:block;max-width:none}g.node{cursor:pointer}.dim{opacity:.12}</style><h1>Карта проекта</h1><p>Поиск выделяет узлы; нажмите узел для выделения соседей. Колесо браузера прокручивает карту, Ctrl+колесо меняет масштаб страницы.</p><input id=\"query\" placeholder=\"Поиск узла\"><button id=\"reset\">Сбросить</button>" : "");
    g_string_append_printf(s, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" height=\"%.0f\" viewBox=\"0 0 %.0f %.0f\"><rect width=\"100%%\" height=\"100%%\" fill=\"#181818\"/><text x=\"20\" y=\"25\" fill=\"#ece9e4\">%u из %u узлов · %s</text><defs><marker id=\"arrow\" markerWidth=\"8\" markerHeight=\"8\" refX=\"7\" refY=\"4\" orient=\"auto\"><path d=\"M0,0 L8,4 L0,8\" fill=\"#e79873\"/></marker></defs>", layout->width, layout->height + 40, layout->width, layout->height + 40, n, total, targets ? "цели" : "файлы");
    for (guint i = 0; i < edges->len; i++) {
        LayoutEdge e = g_array_index(edges, LayoutEdge, i); LayoutNode a = g_array_index(layout->nodes, LayoutNode, e.from), b = g_array_index(layout->nodes, LayoutNode, e.to);
        g_string_append_printf(s, "<path class=\"edge\" data-a=\"%u\" data-b=\"%u\" d=\"M%.0f,%.0f C%.0f,%.0f %.0f,%.0f %.0f,%.0f\" fill=\"none\" stroke=\"#e79873\" opacity=\".6\" marker-end=\"url(#arrow)\"/>", e.from, e.to, a.x+200, a.y+31, a.x+240, a.y+31, b.x-40, b.y+31, b.x, b.y+31);
    }
    for (guint i = 0; i < n; i++) {
        LayoutNode a = g_array_index(layout->nodes, LayoutNode, i);
        g_autofree char *name = g_markup_escape_text(node_name(p, targets, i), -1);
        g_string_append_printf(s, "<g class=\"node\" data-id=\"%u\"><title>%s</title><rect x=\"%.0f\" y=\"%.0f\" width=\"200\" height=\"62\" rx=\"8\" fill=\"#303030\" stroke=\"#e79873\"/><svg x=\"%.0f\" y=\"%.0f\" width=\"184\" height=\"50\"><text x=\"0\" y=\"25\" font-size=\"12\" fill=\"#ece9e4\">%s</text></svg></g>", i, name, a.x, a.y, a.x+8, a.y+8, name);
    }
    g_string_append(s, "</svg>");
    if (html) g_string_append(s, "<script>const ns=[...document.querySelectorAll('.node')],es=[...document.querySelectorAll('.edge')];function reset(){[...ns,...es].forEach(n=>n.classList.remove('dim'))}document.querySelector('#reset').onclick=reset;document.querySelector('#query').oninput=e=>{reset();const q=e.target.value.toLocaleLowerCase();ns.forEach(n=>n.classList.toggle('dim',!n.textContent.toLocaleLowerCase().includes(q)))};ns.forEach(n=>n.onclick=()=>{reset();const id=n.dataset.id,ids=new Set([id]);es.forEach(e=>{const yes=e.dataset.a===id||e.dataset.b===id;e.classList.toggle('dim',!yes);if(yes){ids.add(e.dataset.a);ids.add(e.dataset.b)}});ns.forEach(x=>x.classList.toggle('dim',!ids.has(x.dataset.id)))})</script></html>");
    graph_layout_free(layout); return g_string_free(s, FALSE);
}

char *workspace_environment(const Project *p)
{
    GString *s = g_string_new("Доступность инструментов в PATH (версии компиляторов берутся из метаданных).\n");
    const char *names[] = {"cmake", "ninja", "make", "meson", "cc", "gcc", "clang", "clangd", "gdb", "lldb", "git", NULL};
    for (guint i = 0; names[i]; i++) { g_autofree char *path = g_find_program_in_path(names[i]); g_string_append_printf(s, "%s: %s\n", names[i], path ? path : "не найден · установите инструмент или настройте PATH"); }
    for (guint i = 0; i < p->compilers->len; i++) {
        ProjectCompiler *c = g_ptr_array_index(p->compilers, i);
        g_autofree char *path = c->path ? g_find_program_in_path(c->path) : NULL;
        g_string_append_printf(s, "Компилятор %s %s %s: %s · %s\n", c->language, c->id ? c->id : "?", c->version ? c->version : "?", c->path ? c->path : "?", path ? "найден" : "проверьте путь/SDK");
    }
    for (guint i = 0; i < p->files->len; i++) {
        ProjectFile *f = g_ptr_array_index(p->files, i); GPtrArray *dirs[] = {f->include_dirs, f->system_dirs};
        for (guint g = 0; g < 2; g++) for (guint j = 0; dirs[g] && j < dirs[g]->len && s->len < 100000; j++) {
            const char *path = g_ptr_array_index(dirs[g], j);
            if (!g_file_test(path, G_FILE_TEST_IS_DIR)) g_string_append_printf(s, "%s: отсутствует каталог заголовков %s\n", f->path, path);
        }
    }
    g_string_append(s, "Наличие exe не доказывает совместимость SDK/библиотек. Для проверки версий и зависимостей выполните конфигурацию и сборку.\n");
    return g_string_free(s, FALSE);
}

char *workspace_catalog(const char *filename, const Project *p, const char *last, GError **error)
{
    g_autofree char *default_path = g_build_filename(g_get_user_config_dir(), "startapp", "catalog.ini", NULL);
    const char *path = filename ? filename : default_path;
    g_autoptr(GKeyFile) keys = g_key_file_new();
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInfo) info = g_file_query_info(file, "standard::type,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    if (info && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR && g_file_info_get_size(info) <= 1024*1024) g_key_file_load_from_file(keys, path, G_KEY_FILE_NONE, NULL);
    if (p) {
        g_autofree char *id = g_compute_checksum_for_string(G_CHECKSUM_SHA256, p->root, -1);
        g_autofree char *summary = project_summary(p);
        g_key_file_set_string(keys, id, "root", p->root); g_key_file_set_string(keys, id, "summary", summary);
        g_key_file_set_string(keys, id, "build", p->generator ? p->generator : "инструмент сборки не настроен");
        g_key_file_set_string(keys, id, "file", last ? last : "");
        g_key_file_set_int64(keys, id, "opened", g_get_real_time());
    }
    gsize count; g_auto(GStrv) groups = g_key_file_get_groups(keys, &count);
    /* Keep the most recently updated 100 project cards. */
    while (count > 100) {
        gint64 oldest = G_MAXINT64; guint at = 0;
        for (guint i = 0; groups[i]; i++) { gint64 t = g_key_file_get_int64(keys, groups[i], "opened", NULL); if (t < oldest) { oldest = t; at = i; } }
        g_key_file_remove_group(keys, groups[at], NULL); g_strfreev(groups); groups = g_key_file_get_groups(keys, &count);
    }
    GString *s = g_string_new("Каталог локальных проектов · до 100 записей\n");
    for (guint i = 0; i < count; i++) {
        g_autofree char *root = g_key_file_get_string(keys, groups[i], "root", NULL), *summary = g_key_file_get_string(keys, groups[i], "summary", NULL);
        g_autofree char *build = g_key_file_get_string(keys, groups[i], "build", NULL), *last_file = g_key_file_get_string(keys, groups[i], "file", NULL);
        if (root) g_string_append_printf(s, "\n%s%s\n%s\n%s\nПоследний файл: %s\n", root, g_file_test(root, G_FILE_TEST_IS_DIR) ? "" : " [папка недоступна]", summary ? summary : "", build ? build : "", last_file ? last_file : "");
    }
    if (p) {
        g_autofree char *parent = g_path_get_dirname(path); g_mkdir_with_parents(parent, 0700);
        gsize length; g_autofree char *text = g_key_file_to_data(keys, &length, NULL);
        if (length > 1024*1024 || !g_file_set_contents_full(path, text, (gssize)length, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error)) { g_string_free(s, TRUE); return NULL; }
    }
    return g_string_free(s, FALSE);
}

BuildPlan *workspace_command(const char *directory, const char *const *argv)
{
    BuildPlan *plan = g_new0(BuildPlan, 1); plan->argv = g_strdupv((char **)argv); plan->directory = g_strdup(directory);
    GString *display = g_string_new(NULL);
    for (guint i = 0; argv[i]; i++) { g_autofree char *q = g_shell_quote(argv[i]); if (i) g_string_append_c(display, ' '); g_string_append(display, q); }
    plan->display = g_string_free(display, FALSE); return plan;
}
static gboolean value_valid(const char *value)
{ return value && *value && *value != '-' && strlen(value) <= 8192 && !strchr(value, '\n') && !strchr(value, '\r'); }
BuildPlan *workspace_configure(const Project *p, const char *system, const char *tool, const char *build_dir,
    const char *generator, const char *config, const char *preset, GError **error)
{
    if (!p || !value_valid(tool) || !value_valid(build_dir) || (preset && *preset && !value_valid(preset)) || (generator && *generator && !value_valid(generator))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Укажите инструмент и каталог сборки; значения не должны начинаться с '-'."); return NULL;
    }
    g_autofree char *dir = g_canonicalize_filename(build_dir, p->root);
    if (g_str_equal(system, "CMake")) {
        {
            /* File API queries are data only; written only on explicit configuration. */
            g_autofree char *query = g_build_filename(dir, ".cmake", "api", "v1", "query", NULL);
            if (g_mkdir_with_parents(query, 0700) != 0) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Не удалось создать каталог File API query"); return NULL; }
            const char *names[] = {"codemodel-v2", "toolchains-v1"};
            for (guint i = 0; i < 2; i++) { g_autofree char *f = g_build_filename(query, names[i], NULL); if (!g_file_set_contents(f, "", 0, error)) return NULL; }
        }
        g_autofree char *type = g_strdup_printf("-DCMAKE_BUILD_TYPE=%s", config && *config ? config : "Debug");
        const char *args[16] = {tool, "-S", p->root, "-B", dir, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", NULL}; guint n = 6;
        if (preset && *preset) { args[n++] = "--preset"; args[n++] = preset; }
        else { args[n++] = type; if (generator && *generator) { args[n++] = "-G"; args[n++] = generator; } }
        args[n] = NULL; return workspace_command(p->root, args);
    }
    if (g_str_equal(system, "Meson")) {
        const char *args[] = {tool, "setup", dir, p->root, "--buildtype", config && g_ascii_strcasecmp(config, "Release") == 0 ? "release" : "debug", NULL};
        return workspace_command(p->root, args);
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Make не имеет универсального этапа конфигурации. Выберите существующий Makefile."); return NULL;
}
BuildPlan *workspace_build(const Project *p, const char *system, const char *tool, const char *target, GError **error)
{
    if (!p || !value_valid(tool) || (target && *target && !value_valid(target))) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Недопустимый инструмент или цель"); return NULL; }
    if (g_str_equal(system, "CMake")) {
        gint index = -1;
        if (target && *target) { for (guint i = 0; i < p->targets->len; i++) if (g_str_equal(node_name(p, TRUE, i), target)) index = (gint)i;
            if (index < 0) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Цель отсутствует в метаданных"); return NULL; } }
        return build_plan_new(p, index, tool, error);
    }
    if (g_str_equal(system, "Meson")) {
        if (!p->build_directory) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Выберите каталог Meson-сборки"); return NULL; }
        const char *args[] = {tool, "compile", "-C", p->build_directory, "-j", "2", target && *target ? target : NULL, NULL}; return workspace_command(p->root, args);
    }
    const char *args[] = {tool, "-j2", target && *target ? target : NULL, NULL}; return workspace_command(p->root, args);
}
BuildPlan *workspace_launch(const Project *p, gint target, const char *arguments, const char *directory,
    const char *debugger, const char *breakpoint, GError **error)
{
    if (!p || target < 0 || (guint)target >= p->targets->len) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Выберите исполняемую цель"); return NULL;
    }
    ProjectTarget *t = g_ptr_array_index(p->targets, (guint)target);
    if (!g_str_equal(t->type, "EXECUTABLE") || !t->artifact) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "У цели нет исполняемого артефакта"); return NULL;
    }
    g_autofree char *exe = g_canonicalize_filename(t->artifact, p->build_directory ? p->build_directory : p->root);
    if (!g_file_test(exe, G_FILE_TEST_IS_REGULAR)) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Артефакт отсутствует. Соберите выбранную цель."); return NULL; }
    g_auto(GStrv) parsed = NULL;
    if (arguments && *arguments && !g_shell_parse_argv(arguments, NULL, &parsed, error)) return NULL;
    g_autoptr(GPtrArray) args = g_ptr_array_new();
    if (debugger && *debugger) {
        if (!value_valid(debugger) || (breakpoint && *breakpoint && !value_valid(breakpoint))) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Недопустимый отладчик/точка останова"); return NULL; }
        g_ptr_array_add(args, (gpointer)debugger);
        g_autofree char *name = g_path_get_basename(debugger);
        /* Batch sessions intentionally need no terminal; captures variables and stack at the stop. */
        g_autofree char *stop = NULL;
        if (g_str_has_prefix(name, "lldb")) {
            g_ptr_array_add(args, "--batch");
            const char *bp = breakpoint && *breakpoint ? breakpoint : "main";
            g_autofree char *quoted = g_shell_quote(bp);
            const char *colon = strrchr(bp, ':');
            if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
                g_autofree char *file = g_strndup(bp, (gsize)(colon - bp));
                g_autofree char *qfile = g_shell_quote(file);
                stop = g_strdup_printf("breakpoint set --file %s --line %s", qfile, colon + 1);
            } else stop = g_strdup_printf("breakpoint set --name %s", quoted);
            g_ptr_array_add(args, "--no-lldbinit");
            g_ptr_array_add(args, "-o"); g_ptr_array_add(args, stop);
            g_ptr_array_add(args, "-o"); g_ptr_array_add(args, "run");
            g_ptr_array_add(args, "-o"); g_ptr_array_add(args, "thread backtrace all");
            g_ptr_array_add(args, "-o"); g_ptr_array_add(args, "frame variable"); g_ptr_array_add(args, "--");
        } else {
            g_ptr_array_add(args, "--batch"); g_ptr_array_add(args, "-nx"); g_ptr_array_add(args, "-iex"); g_ptr_array_add(args, "set auto-load safe-path /nonexistent-startapp");
            g_ptr_array_add(args, "-ex"); g_ptr_array_add(args, "set pagination off");
            stop = g_strdup_printf("break %s", breakpoint && *breakpoint ? breakpoint : "main");
            g_ptr_array_add(args, "-ex"); g_ptr_array_add(args, stop); g_ptr_array_add(args, "-ex"); g_ptr_array_add(args, "run");
            g_ptr_array_add(args, "-ex"); g_ptr_array_add(args, "thread apply all bt");
            g_ptr_array_add(args, "-ex"); g_ptr_array_add(args, "info locals"); g_ptr_array_add(args, "--args");
        }
        g_ptr_array_add(args, exe); for (guint i = 0; parsed && parsed[i]; i++) g_ptr_array_add(args, parsed[i]); g_ptr_array_add(args, NULL);
        return workspace_command(directory && *directory ? directory : p->root, (const char *const *)args->pdata);
    }
    g_ptr_array_add(args, exe); for (guint i = 0; parsed && parsed[i]; i++) g_ptr_array_add(args, parsed[i]); g_ptr_array_add(args, NULL);
    return workspace_command(directory && *directory ? directory : p->root, (const char *const *)args->pdata);
}
