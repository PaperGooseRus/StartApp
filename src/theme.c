#include "theme.h"

static const char *colors[THEME_COLOR_COUNT] = {
    "#181818", "#202020", "#303030", "#383838",
    "#ece9e4", "#aaa6a0", "#e79873", "#49342b"
};
static const char *names[THEME_COLOR_COUNT] = {
    "app_bg", "app_surface", "app_raised", "app_border",
    "app_text", "app_muted", "app_accent", "app_selection"
};

const char *theme_hex(ThemeColor color) { return colors[color]; }

void theme_cairo(cairo_t *cr, ThemeColor color, double alpha)
{
    GdkRGBA rgba;
    gdk_rgba_parse(&rgba, colors[color]);
    cairo_set_source_rgba(cr, rgba.red, rgba.green, rgba.blue, alpha);
}

void theme_install(void)
{
    /* The fallback GTK decorations and dialogs use the dark variant as well. */
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    GString *css = g_string_new(NULL);
    for (guint i = 0; i < THEME_COLOR_COUNT; i++)
        g_string_append_printf(css, "@define-color %s %s;", names[i], colors[i]);
    g_string_append(css,
        "window, popover contents { background: @app_bg; color: @app_text; }"
        "headerbar { background: @app_surface; color: @app_text; box-shadow: none; }"
        "button { background: transparent; background-image: none; color: @app_text;"
        " border: 1px solid transparent; border-radius: 7px; box-shadow: none; text-shadow: none;"
        " min-height: 26px; padding: 5px 10px; }"
        "button:hover { background: @app_raised; }"
        "button:active, button:checked { background: @app_selection; color: @app_accent; }"
        "button:checked:hover { background: #584035; }"
        "button:focus-visible { outline: 2px solid @app_accent; outline-offset: -2px; }"
        "button:disabled { color: @app_muted; opacity: .45; }"
        "button.suggested-action { color: @app_accent; }"
        ".sidebar { background: @app_surface; padding: 16px 12px; }"
        ".title { font-size: 17px; font-weight: bold; }"
        ".brand { color: @app_accent; }"
        ".muted { color: @app_muted; font-size: 11px; }"
        ".section { color: @app_muted; font-size: 10px; font-weight: bold; letter-spacing: 1px; }"
        ".toolbar { padding: 8px 12px; background: @app_surface; }"
        ".status { padding: 8px 12px; background: @app_surface; color: @app_muted; }"
        "listview { background: transparent; color: @app_text; }"
        "listview row { border-radius: 6px; }"
        "listview row:hover { background: @app_raised; }"
        "listview row:selected { background: @app_selection; color: @app_text; }"
        "listview row:focus-visible { outline: 1px solid @app_accent; outline-offset: -1px; }"
        "textview, textview text { background: @app_bg; color: @app_text; caret-color: @app_accent; }"
        "textview text selection, entry selection { background: @app_selection; color: @app_text; }"
        "entry, searchentry { background: @app_bg; color: @app_text; border: 1px solid @app_border;"
        " border-radius: 7px; box-shadow: none; }"
        "entry:focus-within, searchentry:focus-within { border-color: @app_accent; }"
        "dropdown button { background: @app_raised; }"
        "dropdown button:hover { background: #3b3b3b; }"
        "popover contents { border: 1px solid @app_border; }"
        "paned > separator { background: @app_border; min-width: 4px; min-height: 4px; }"
        "paned > separator:hover { background: @app_accent; }"
        "scrollbar { background: @app_bg; }"
        "scrollbar slider { background: #505050; border: none; }"
        "scrollbar slider:hover { background: #77716b; }"
        "label link { color: @app_accent; }"
        ".details { background: @app_surface; padding: 12px; color: @app_muted; font-size: 11px; }");
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css->str);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    g_string_free(css, TRUE);
}
