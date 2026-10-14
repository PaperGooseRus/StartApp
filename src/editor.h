#ifndef STARTAPP_EDITOR_H
#define STARTAPP_EDITOR_H

#include <gtk/gtk.h>

void editor_attach_gutter(GtkTextView *view, GtkScrolledWindow *scroll);
gboolean editor_go_to_line(GtkTextView *view, guint line);
/* Last actually rendered range; useful for checking scroll synchronization. */
gboolean editor_gutter_visible_lines(GtkTextView *view, guint *first, guint *last);

#endif
