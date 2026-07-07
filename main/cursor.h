#ifndef CURSOR_H
#define CURSOR_H

#include <stdint.h>

// A virtual absolute cursor. Relative mouse motion is accumulated into a fixed
// logical coordinate space (0..ABS_AXIS_MAX on each axis) and clamped to bounds;
// the absolute position is what we emit to the host. This is what lets the host
// track exactly where the cursor is — the basis for hopping between machines.
//
// The KVM owns one of these per host, so the struct is caller-owned (no globals):
// each host remembers where its cursor was left.

// Which screen edge the cursor was pushed off, if any. Returned by
// cursor_apply_delta so the KVM can decide whether to hop to an adjacent host.
typedef enum {
    CURSOR_EDGE_NONE = 0,
    CURSOR_EDGE_LEFT,
    CURSOR_EDGE_RIGHT,
} cursor_edge_t;

typedef struct {
    int32_t x;
    int32_t y;
    // Accumulated overshoot from sustained pushing against the left/right edge.
    // Reset once motion pulls away from the edge or after a hop. This is what
    // distinguishes a deliberate "push off the screen" from a fast flick that
    // merely reaches the edge.
    int32_t edge_push;
} cursor_t;

// Centre the cursor and clear any edge push. Call once per host at startup.
void cursor_reset(cursor_t *c);

// Accumulate one relative motion report (raw mouse counts) into the position,
// applying the sensitivity gain and clamping to the coordinate bounds. Returns
// the edge the cursor was *sustainedly* pushed off (past the clamp, beyond a
// threshold) this report, or CURSOR_EDGE_NONE.
cursor_edge_t cursor_apply_delta(cursor_t *c, int dx, int dy);

// Place the cursor as if it entered the screen from the given edge at height y —
// used on a hop so the pointer appears at the opposite side it left the last
// host, at the same vertical position. Clears the edge push.
void cursor_enter_from(cursor_t *c, cursor_edge_t entry_edge, uint16_t y);

uint16_t cursor_x(const cursor_t *c);
uint16_t cursor_y(const cursor_t *c);

#endif
