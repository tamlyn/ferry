#include "cursor.h"

#include "our_descriptor.h"   // ABS_AXIS_MAX

// Relative mouse counts are scaled up into the absolute coordinate space. Unlike
// a relative pointer, an absolute one gets no pointer acceleration from the host,
// so we apply our own gain: this many absolute units per mouse count. Tune to
// taste — higher is faster. (A later milestone may make this host-configurable.)
#define CURSOR_GAIN  20

// How much overshoot (in absolute units) must accumulate against one edge before
// we treat it as a deliberate "push off the screen" and hop. At CURSOR_GAIN=20
// this is a firm, sustained shove into the edge over several reports — a normal
// fast flick that merely reaches the edge won't cross it. Tune on hardware.
#define EDGE_HOP_THRESHOLD  6000

static int32_t clamp_axis(int32_t v)
{
    if (v < 0) {
        return 0;
    }
    if (v > ABS_AXIS_MAX) {
        return ABS_AXIS_MAX;
    }
    return v;
}

void cursor_reset(cursor_t *c)
{
    c->x = ABS_AXIS_MAX / 2;
    c->y = ABS_AXIS_MAX / 2;
    c->edge_push = 0;
}

cursor_edge_t cursor_apply_delta(cursor_t *c, int dx, int dy)
{
    int32_t raw_x = c->x + dx * CURSOR_GAIN;

    // Clamp X and measure how far past the edge the raw motion would have gone.
    cursor_edge_t at_edge = CURSOR_EDGE_NONE;
    int32_t overshoot = 0;
    if (raw_x < 0) {
        overshoot = -raw_x;
        at_edge = CURSOR_EDGE_LEFT;
    } else if (raw_x > ABS_AXIS_MAX) {
        overshoot = raw_x - ABS_AXIS_MAX;
        at_edge = CURSOR_EDGE_RIGHT;
    }

    c->x = clamp_axis(raw_x);
    c->y = clamp_axis(c->y + dy * CURSOR_GAIN);

    // Accumulate overshoot only while the motion keeps pushing *into* the same
    // edge; any pull away from it (or purely vertical/idle motion) resets the
    // count, so the push has to be sustained and directed to trigger a hop.
    if (at_edge == CURSOR_EDGE_LEFT && dx < 0) {
        c->edge_push += overshoot;
        if (c->edge_push >= EDGE_HOP_THRESHOLD) {
            c->edge_push = 0;
            return CURSOR_EDGE_LEFT;
        }
    } else if (at_edge == CURSOR_EDGE_RIGHT && dx > 0) {
        c->edge_push += overshoot;
        if (c->edge_push >= EDGE_HOP_THRESHOLD) {
            c->edge_push = 0;
            return CURSOR_EDGE_RIGHT;
        }
    } else {
        c->edge_push = 0;
    }

    return CURSOR_EDGE_NONE;
}

void cursor_enter_from(cursor_t *c, cursor_edge_t entry_edge, uint16_t y)
{
    c->x = (entry_edge == CURSOR_EDGE_RIGHT) ? ABS_AXIS_MAX : 0;
    c->y = clamp_axis(y);
    c->edge_push = 0;
}

uint16_t cursor_x(const cursor_t *c)
{
    return (uint16_t)c->x;
}

uint16_t cursor_y(const cursor_t *c)
{
    return (uint16_t)c->y;
}
