#include "workspace_tools.h"
#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>

/* Extract the bounded regular-file subset of Git's tar output. Never create links. */
gboolean workspace_extract_snapshot(const char *archive, const char *destination, GError **error)
{
    g_autoptr(GFile) file = g_file_new_for_path(archive);
    g_autoptr(GFileInfo) info = g_file_query_info(file, "standard::type,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
    if (!info) return FALSE;
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR || g_file_info_get_size(info) > 64*1024*1024) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Git-снимок должен быть обычным tar до 64 MiB"); return FALSE;
    }
    g_autofree char *tar = NULL; gsize length; if (!g_file_get_contents(archive, &tar, &length, error)) return FALSE;
    if (length < 1024 || length > 64*1024*1024 || length % 512) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Неполный tar-снимок"); return FALSE; }
    g_autofree char *pax_path = NULL; guint count = 0;
    for (gsize at = 0; at + 512 <= length; ) {
        const char *h = tar + at; if (!*h) return TRUE;
        g_autofree char *octal = g_strndup(h+124, 12);
        char *end = NULL; guint64 size = g_ascii_strtoull(octal, &end, 8);
        if (end == octal || size > length - at - 512) goto invalid;
        gsize padding = ((gsize)size + 511) / 512 * 512;
        if (padding > length - at - 512) goto invalid;
        const char *body = h+512; char type = h[156];
        if (type == 'x' || type == 'g') {
            for (gsize i = 0; i < size; ) {
                char *after; guint64 span = g_ascii_strtoull(body+i, &after, 10);
                if (!span || span > size - i || after == body+i || *after != ' ' || after >= body+i+span-1) goto invalid;
                const char *key = after+1;
                if (body[i+span-1] != '\n') goto invalid;
                gsize value_length = (gsize)(body+i+span-1-key);
                if (type == 'x' && value_length >= 5 && memcmp(key, "path=", 5) == 0) { g_free(pax_path); pax_path = g_strndup(key+5, value_length-5); }
                i += (gsize)span;
            }
        } else {
            g_autofree char *name = g_strndup(h, 100), *prefix = g_strndup(h+345, 155);
            g_autofree char *path = pax_path ? g_steal_pointer(&pax_path) : *prefix ? g_strconcat(prefix, "/", name, NULL) : g_strdup(name);
            if (!g_utf8_validate(path, -1, NULL) || !*path || g_path_is_absolute(path) || strchr(path, '\\') || strchr(path, ':')) goto invalid;
            g_auto(GStrv) parts = g_strsplit(path, "/", -1);
            for (guint i = 0; parts[i]; i++) if (g_str_equal(parts[i], "..") || g_str_equal(parts[i], ".git")) goto invalid;
            if ((type == '0' || type == 0) && size <= 2*1024*1024) {
                if (++count > 10000) goto limit;
                g_autofree char *target = g_build_filename(destination, path, NULL), *parent = g_path_get_dirname(target);
                if (g_mkdir_with_parents(parent, 0700) != 0) {
                    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Cannot create snapshot directory: %s", g_strerror(errno)); return FALSE;
                }
                if (!g_file_set_contents(target, body, (gssize)size, error)) return FALSE;
            }
        }
        at += 512 + padding;
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT, "В tar-снимке отсутствует завершающий блок"); return FALSE;
invalid:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Неподдерживаемый или небезопасный tar-путь/заголовок Git-снимка"); return FALSE;
limit:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Более 10 000 файлов в Git-снимке"); return FALSE;
}
