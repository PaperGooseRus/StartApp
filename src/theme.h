#ifndef STARTAPP_THEME_H
#define STARTAPP_THEME_H

#include <gtk/gtk.h>

typedef enum {
    THEME_BACKGROUND, THEME_SURFACE, THEME_RAISED, THEME_BORDER,
    THEME_TEXT, THEME_MUTED, THEME_ACCENT, THEME_SELECTION, THEME_COLOR_COUNT
} ThemeColor;

void theme_install(void);
const char *theme_hex(ThemeColor color);
void theme_cairo(cairo_t *cr, ThemeColor color, double alpha);

#endif
