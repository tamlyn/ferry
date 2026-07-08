#include "cursor.h"

#include <stdbool.h>

// Desk points per mouse count. An absolute pointer gets no host acceleration, so
// this is our sensitivity gain — higher is faster. 1 keeps roughly the feel of the
// pre-multi-monitor build (which used 20 units per count over a 32767-wide display,
// ~= 1 point per count over the Mac's ~1512-point-wide screen). Tune to taste.
#define CURSOR_GAIN 1

// Points of overshoot that must accumulate against one edge before a push counts as
// a deliberate crossing rather than a fast flick that merely reaches the edge.
#define EDGE_PUSH_THRESHOLD 250

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void cursor_init(cursor_t *c, int32_t x, int32_t y)
{
    c->x = x;
    c->y = y;
    c->push = 0;
    c->push_edge = EDGE_NONE;
}

edge_t cursor_move(cursor_t *c, int dx, int dy, rect_t b)
{
    int32_t nx = c->x + dx * CURSOR_GAIN;
    int32_t ny = c->y + dy * CURSOR_GAIN;

    // Which edge the raw motion overshoots, and by how far. On a diagonal push that
    // exceeds two edges at once, take the axis with the larger overshoot.
    edge_t edge = EDGE_NONE;
    int32_t over = 0;
    bool into = false;
    if (nx < b.x0)      { edge = EDGE_LEFT;  over = b.x0 - nx; into = (dx < 0); }
    else if (nx > b.x1) { edge = EDGE_RIGHT; over = nx - b.x1; into = (dx > 0); }
    if (ny < b.y0)      { int32_t o = b.y0 - ny; if (o > over) { edge = EDGE_TOP;    over = o; into = (dy < 0); } }
    else if (ny > b.y1) { int32_t o = ny - b.y1; if (o > over) { edge = EDGE_BOTTOM; over = o; into = (dy > 0); } }

    c->x = clampi(nx, b.x0, b.x1);
    c->y = clampi(ny, b.y0, b.y1);

    // Only a sustained push *into* one edge accumulates; pulling away, sliding
    // along, or idling resets it, so the shove has to be directed and held.
    if (edge != EDGE_NONE && into) {
        if (edge == c->push_edge) {
            c->push += over;
        } else {
            c->push_edge = edge;
            c->push = over;
        }
        if (c->push >= EDGE_PUSH_THRESHOLD) {
            c->push = 0;
            c->push_edge = EDGE_NONE;
            return edge;
        }
    } else {
        c->push = 0;
        c->push_edge = EDGE_NONE;
    }
    return EDGE_NONE;
}
