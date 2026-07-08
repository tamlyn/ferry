#ifndef CURSOR_H
#define CURSOR_H

#include <stdint.h>

#include "layout.h"   // rect_t, edge_t

// The single active cursor, tracked in global desk points. Relative mouse counts
// are scaled into that space and clamped to the current display's rectangle; a
// sustained shove past one edge is reported so the KVM can decide whether to cross
// to a neighbouring display.
typedef struct {
    int32_t x, y;        // position in global desk points
    int32_t push;        // overshoot accumulated against push_edge
    edge_t  push_edge;   // the edge currently being pushed into
} cursor_t;

void   cursor_init(cursor_t *c, int32_t x, int32_t y);

// Apply a relative mouse delta, clamped to `bounds`. Returns the edge that has been
// decisively pushed past (a sustained, directed shove — a fast flick that merely
// reaches the edge won't trigger it), or EDGE_NONE.
edge_t cursor_move(cursor_t *c, int dx, int dy, rect_t bounds);

#endif
