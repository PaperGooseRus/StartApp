#ifndef STARTAPP_BUILD_METADATA_H
#define STARTAPP_BUILD_METADATA_H

#include "project.h"

void build_metadata_import(Project *project, const char *directory, const char *configuration);
void build_metadata_target_free(gpointer data);
void build_metadata_edge_free(gpointer data);
void build_metadata_compiler_free(gpointer data);
void build_metadata_preset_free(gpointer data);
char **build_metadata_split_command(const char *command, gboolean windows, GError **error);

#endif
