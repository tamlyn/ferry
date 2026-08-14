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
    int32_t acc_x, acc_y;// sub-point remainder from acceleration scaling (1/256 pt)
    int32_t push;        // overshoot accumulated against push_edge
    edge_t  push_edge;   // the edge currently being pushed into
} cursor_t;

// What one report of motion did. `raw_x`/`raw_y` is where the motion would have
// landed had the display's rectangle not clamped it — the position to resume at on
// the far side of a seam the host crosses under its own steam. What a relative host
// gets sent is deliberately *not* here: it is the model's own displacement over the
// whole report, crossings included, which only the KVM can measure. Sending the
// pre-clamp delta instead lets the host's cursor travel where the model would not,
// and the two then disagree permanently (a model wall that isn't a real wall slides
// the whole model sideways one shove at a time).
typedef struct {
    int32_t raw_x, raw_y;
    edge_t  edge;   // edge being pushed into, or EDGE_NONE
    int32_t push;   // overshoot accumulated against `edge` so far
} cursor_step_t;

void cursor_init(cursor_t *c, int32_t x, int32_t y);

// Apply a relative mouse delta, clamped to `bounds`, and report what happened. How
// much of a push counts as a deliberate crossing is the caller's policy, not ours —
// it varies by what lies across the edge (see kvm.c).
void cursor_move(cursor_t *c, int dx, int dy, rect_t bounds, cursor_step_t *out);

#endif
