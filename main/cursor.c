#include "cursor.h"

#include <stdbool.h>

// Pointer acceleration. An absolute pointer gets no host-side acceleration, so we
// apply our own: the gain (desk points per mouse count) rises with speed, so slow,
// deliberate motion is precise and fast flicks still cover ground. Speed is proxied
// by the per-report count magnitude — USB polls at a fixed rate, so counts-per-report
// tracks hand velocity. Gain ramps linearly from ACCEL_MIN_GAIN at rest to
// ACCEL_MAX_GAIN once speed reaches ACCEL_FULL_SPEED, then holds.
//
// GAIN_UNIT is fixed-point 1.0 (== 1 point/count, the old flat CURSOR_GAIN, which
// roughly matched the pre-multi-monitor feel). Tune by feel: ACCEL_MIN_GAIN sets how
// slow fine motion is, ACCEL_MAX_GAIN the top speed (raise past GAIN_UNIT for a faster
// ceiling), ACCEL_FULL_SPEED how hard you must move to reach it. The last depends on
// the mouse's DPI and poll rate, so it is the one most likely to want adjusting.
#define GAIN_UNIT         256
#define ACCEL_MIN_GAIN     64   // 0.25x — gain for slow, precise motion
#define ACCEL_MAX_GAIN    320   // 1.25x — gain for fast flicks (a boost past 1:1)
#define ACCEL_FULL_SPEED   24   // counts/report at which gain reaches the max

// Points of overshoot that must accumulate against one edge before a push counts as
// a deliberate crossing rather than a fast flick that merely reaches the edge. Kept
// low deliberately: crossings should feel immediate, and at ACCEL_MAX_GAIN a fast
// flick overshoots ~30 points per report, so this is a couple of reports' worth of
// continued shoving — enough to reject a single stray report, not enough to notice.
#define EDGE_PUSH_THRESHOLD 60

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Fixed-point gain (GAIN_UNIT == 1.0) for a per-report speed in mouse counts.
static int32_t accel_gain(int32_t speed)
{
    if (speed >= ACCEL_FULL_SPEED) {
        return ACCEL_MAX_GAIN;
    }
    return ACCEL_MIN_GAIN + (ACCEL_MAX_GAIN - ACCEL_MIN_GAIN) * speed / ACCEL_FULL_SPEED;
}

void cursor_init(cursor_t *c, int32_t x, int32_t y)
{
    c->x = x;
    c->y = y;
    c->acc_x = 0;
    c->acc_y = 0;
    c->push = 0;
    c->push_edge = EDGE_NONE;
}

edge_t cursor_move(cursor_t *c, int dx, int dy, rect_t b)
{
    // Scale the raw counts by a speed-dependent gain, carrying the sub-point
    // remainder so slow motion (gain < 1) still moves instead of truncating to zero.
    int32_t speed = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    int32_t gain = accel_gain(speed);
    c->acc_x += dx * gain;
    c->acc_y += dy * gain;
    int32_t mx = c->acc_x / GAIN_UNIT;
    int32_t my = c->acc_y / GAIN_UNIT;
    c->acc_x -= mx * GAIN_UNIT;
    c->acc_y -= my * GAIN_UNIT;

    int32_t nx = c->x + mx;
    int32_t ny = c->y + my;

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
