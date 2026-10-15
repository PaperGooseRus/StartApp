#ifndef STARTAPP_GRAPH_LAYOUT_H
#define STARTAPP_GRAPH_LAYOUT_H

#include <glib.h>

#define GRAPH_LAYOUT_LIMIT 200u
typedef struct { guint from, to; } LayoutEdge;
typedef struct { double x, y; guint layer, component; gboolean cycle; } LayoutNode;
typedef struct { GArray *nodes; double width, height; guint cycles; } GraphLayout;

/* Nodes retain their input indices; invalid edges are ignored. */
GraphLayout *graph_layout_build(guint count, const GArray *edges);
/* Place directory/type groups in disjoint horizontal bands, preserving layers. */
void graph_layout_group(GraphLayout *layout, const char *const *groups);
void graph_layout_free(GraphLayout *layout);

#endif
