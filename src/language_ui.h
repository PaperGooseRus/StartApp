#ifndef STARTAPP_LANGUAGE_UI_H
#define STARTAPP_LANGUAGE_UI_H
#include <gtk/gtk.h>
typedef struct LanguageUI LanguageUI;
typedef void (*LanguageNavigate)(const char *path, guint line, gpointer data);
typedef void (*LanguageStatus)(const char *message, gpointer data);
LanguageUI *language_ui_new(GtkApplication *application, GtkWindow *window, GtkTextView *editor,
    LanguageNavigate navigate, LanguageStatus status, gpointer data);
GtkWidget *language_ui_widget(LanguageUI *ui);
void language_ui_context(LanguageUI *ui, const char *root, const char *build_directory, const char *relative_file);
void language_ui_action(LanguageUI *ui, const char *method);
void language_ui_free(LanguageUI *ui);
#endif
