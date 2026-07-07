#include "cursor.h"

#include "our_descriptor.h"   // ABS_AXIS_MAX

// Relative mouse counts are scaled up into the absolute coordinate space. Unlike
// a relative pointer, an absolute one gets no pointer acceleration from the host,
// so we apply our own gain: this many absolute units per mouse count. Tune to
// taste — higher is faster. (A later milestone may make this host-configurable.)
#define CURSOR_GAIN  20

static int32_t s_x;
static int32_t s_y;

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

void cursor_reset(void)
{
    s_x = ABS_AXIS_MAX / 2;
    s_y = ABS_AXIS_MAX / 2;
}

void cursor_apply_delta(int dx, int dy)
{
    s_x = clamp_axis(s_x + dx * CURSOR_GAIN);
    s_y = clamp_axis(s_y + dy * CURSOR_GAIN);
}

uint16_t cursor_x(void)
{
    return (uint16_t)s_x;
}

uint16_t cursor_y(void)
{
    return (uint16_t)s_y;
}
