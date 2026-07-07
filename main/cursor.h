#ifndef CURSOR_H
#define CURSOR_H

#include <stdint.h>

// The virtual absolute cursor. Relative mouse motion is accumulated into a fixed
// logical coordinate space (0..ABS_AXIS_MAX on each axis) and clamped to bounds;
// the absolute position is what we emit to the host. This is what lets the host
// track exactly where the cursor is — the basis for hopping between machines.

// Centre the cursor. Call once at startup.
void cursor_reset(void);

// Accumulate one relative motion report (raw mouse counts) into the position,
// applying the sensitivity gain and clamping to the coordinate bounds.
void cursor_apply_delta(int dx, int dy);

uint16_t cursor_x(void);
uint16_t cursor_y(void);

#endif
