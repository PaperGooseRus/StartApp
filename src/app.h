#ifndef STARTAPP_APP_H
#define STARTAPP_APP_H

#include <glib.h>

int app_run(const char *project_path, const char *build_directory, const char *configuration, gboolean smoke_test);

#endif
