#include "editor.h"
#include "theme.h"

typedef struct {
    GtkTextView *view; /* The text view owns the gutter and this context. */
    guint first_drawn, last_drawn;
    guint scroll_tick;
    guint scroll_frames;
    guint requested_line;
} Gutter;

static void draw_gutter(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                        gpointer user_data)
{
    Gutter *gutter = user_data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(gutter->view);
    GtkTextIter cursor, line;
    gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
    int selected = gtk_text_iter_get_line(&cursor);
    GdkRectangle visible;
    gtk_text_view_get_visible_rect(gutter->view, &visible);
    gtk_text_view_get_line_at_y(gutter->view, &line, visible.y, NULL);
    theme_cairo(cr, THEME_SURFACE, 1);
    cairo_paint(cr);
    theme_cairo(cr, THEME_BORDER, 1);
    cairo_rectangle(cr, width - 1, 0, 1, height);
    cairo_fill(cr);

    PangoLayout *label = gtk_widget_create_pango_layout(GTK_WIDGET(gutter->view), NULL);
    gutter->first_drawn = gutter->last_drawn = 0;
    while (TRUE) {
        int buffer_y, line_height, window_y;
        gtk_text_view_get_line_yrange(gutter->view, &line, &buffer_y, &line_height);
        gtk_text_view_buffer_to_window_coords(gutter->view, GTK_TEXT_WINDOW_LEFT,
                                              0, buffer_y, NULL, &window_y);
        if (window_y >= height) break;
        int number = gtk_text_iter_get_line(&line);
        if (window_y + line_height > 0) {
            if (!gutter->first_drawn) gutter->first_drawn = (guint)number + 1;
            gutter->last_drawn = (guint)number + 1;
            if (number == selected) {
                theme_cairo(cr, THEME_SELECTION, 1);
                cairo_rectangle(cr, 0, window_y, width - 1, line_height);
                cairo_fill(cr);
            }
            char text[16];
            g_snprintf(text, sizeof text, "%d", number + 1);
            pango_layout_set_text(label, text, -1);
            int text_width, text_height;
            pango_layout_get_pixel_size(label, &text_width, &text_height);
            theme_cairo(cr, number == selected ? THEME_ACCENT : THEME_MUTED, 1);
            cairo_move_to(cr, width - text_width - 10,
                          window_y + MAX(0, (line_height - text_height) / 2));
            pango_cairo_show_layout(cr, label);
        }
        gtk_text_iter_forward_line(&line);
        if (gtk_text_iter_get_line(&line) <= number) break;
    }
    g_object_unref(label);
    (void)area;
}

static void refresh_width(GtkTextBuffer *buffer, gpointer data)
{
    GtkWidget *area = data;
    Gutter *gutter = g_object_get_data(G_OBJECT(area), "gutter-context");
    char largest[16];
    g_snprintf(largest, sizeof largest, "%d", gtk_text_buffer_get_line_count(buffer));
    PangoLayout *label = gtk_widget_create_pango_layout(GTK_WIDGET(gutter->view), largest);
    int width;
    pango_layout_get_pixel_size(label, &width, NULL);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), MAX(42, width + 20));
    g_object_unref(label);
    gtk_widget_queue_draw(area);
}

static void mark_moved(GtkTextBuffer *buffer, const GtkTextIter *iter,
                       GtkTextMark *mark, gpointer data)
{
    (void)iter;
    if (mark == gtk_text_buffer_get_insert(buffer)) gtk_widget_queue_draw(data);
}

static void scroll_changed(GtkAdjustment *adjustment, gpointer data)
{
    (void)adjustment;
    gtk_widget_queue_draw(data);
}

void editor_attach_gutter(GtkTextView *view, GtkScrolledWindow *scroll)
{
    GtkWidget *area = gtk_drawing_area_new();
    Gutter *gutter = g_new0(Gutter, 1);
    gutter->view = view;
    g_object_set_data_full(G_OBJECT(view), "startapp-gutter-context", gutter, g_free);
    g_object_set_data(G_OBJECT(area), "gutter-context", gutter);
    gtk_widget_set_tooltip_text(area, "Номера строк · текущая строка выделена");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_gutter, gutter, NULL);
    gtk_text_view_set_gutter(view, GTK_TEXT_WINDOW_LEFT, area);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    g_signal_connect_object(buffer, "changed", G_CALLBACK(refresh_width), area, 0);
    g_signal_connect_object(buffer, "mark-set", G_CALLBACK(mark_moved), area, 0);
    GtkAdjustment *vertical = gtk_scrolled_window_get_vadjustment(scroll);
    g_signal_connect_object(vertical, "value-changed", G_CALLBACK(scroll_changed), area, 0);
    refresh_width(buffer, area);
}

static gboolean finish_scroll(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    (void)clock;
    Gutter *gutter = data;
    /* GTK 4.14 can finish paragraph layout after the initial scroll request. */
    if (++gutter->scroll_frames < 3) return G_SOURCE_CONTINUE;
    GtkTextView *view = GTK_TEXT_VIEW(widget);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
    if ((guint)gtk_text_iter_get_line(&cursor) + 1 == gutter->requested_line) {
        gtk_text_view_scroll_to_mark(view, gtk_text_buffer_get_insert(buffer), .15, TRUE, 0, .3);
        GtkWidget *area = gtk_text_view_get_gutter(view, GTK_TEXT_WINDOW_LEFT);
        if (area) gtk_widget_queue_draw(area);
    }
    gutter->scroll_tick = 0;
    return G_SOURCE_REMOVE;
}

gboolean editor_go_to_line(GtkTextView *view, guint line)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    if (!line || line > (guint)gtk_text_buffer_get_line_count(buffer)) return FALSE;
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_line(buffer, &start, (gint)line - 1);
    end = start;
    if (!gtk_text_iter_ends_line(&end)) gtk_text_iter_forward_to_line_end(&end);
    gtk_text_buffer_select_range(buffer, &start, &end);
    gtk_text_view_scroll_to_mark(view, gtk_text_buffer_get_insert(buffer), .15, TRUE, 0, .3);
    gtk_widget_grab_focus(GTK_WIDGET(view));
    Gutter *gutter = g_object_get_data(G_OBJECT(view), "startapp-gutter-context");
    if (gutter) {
        if (gutter->scroll_tick) gtk_widget_remove_tick_callback(GTK_WIDGET(view), gutter->scroll_tick);
        gutter->requested_line = line;
        gutter->scroll_frames = 0;
        gutter->scroll_tick = gtk_widget_add_tick_callback(GTK_WIDGET(view), finish_scroll, gutter, NULL);
    }
    return TRUE;
}

gboolean editor_gutter_visible_lines(GtkTextView *view, guint *first, guint *last)
{
    Gutter *gutter = g_object_get_data(G_OBJECT(view), "startapp-gutter-context");
    if (!gutter || !gutter->first_drawn) return FALSE;
    *first = gutter->first_drawn;
    *last = gutter->last_drawn;
    return TRUE;
}
