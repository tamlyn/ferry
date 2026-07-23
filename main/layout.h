#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdint.h>

// The desk model: a set of display rectangles in one global "points" coordinate
// space, each belonging to a BLE host slot. It stitches together each host's own
// coordinate space — the Mac's is real (built-in + external, measured), the PC's
// is a fiction we place where the cursor should hop to it — so the KVM can decide,
// at any display edge, whether the cursor crosses to another display and how.
//
// Two kinds of seam fall out of the geometry: a border between two displays of the
// *same* host (crossed with a relative-motion nudge, since absolute positioning is
// clamped to the display the host's cursor is on) and a border between displays of
// *different* hosts (crossed by switching which host we drive). See kvm.c.

typedef enum {
    EDGE_NONE = 0,
    EDGE_LEFT,
    EDGE_RIGHT,
    EDGE_TOP,
    EDGE_BOTTOM,
} edge_t;

// A display rectangle in global desk points. Occupies [x0, x1) x [y0, y1); the far
// edges x1/y1 are the shared seam coordinates with neighbouring displays.
typedef struct {
    int32_t x0, y0, x1, y1;
} rect_t;

// The desk layouts, selected at runtime (see control.c). They share the same display
// geometry and differ only in which host drives the external monitor. Keep LAYOUT_A/B
// in step with the ownership vectors and flash colours in layout.c / control.c.
typedef enum {
    LAYOUT_A,      // external monitor plugged into the Mac
    LAYOUT_B,      // external monitor plugged into the PC
    LAYOUT_COUNT,
} layout_id_t;

int    layout_display_count(void);
int    layout_host_of(int disp);        // BLE host slot a display belongs to
rect_t layout_rect(int disp);
int    layout_home_display(int host);   // default display when a host is first driven

// Runtime layout selection. layout_set_active is called only from the KVM (input
// task); the accessors above read the active layout. Use kvm_request_layout to switch
// safely from another task.
int         layout_count(void);         // number of layouts
int         layout_active(void);        // active layout id
void        layout_set_active(int id);  // select a layout (out-of-range -> LAYOUT_A)
const char *layout_name(int id);        // short human name, for logs

// The neighbouring display across `edge` of `disp` at global point (gx, gy), or -1
// if that side is a wall there — displays only border where their rectangles
// actually touch, so the same edge can lead to different displays (or nowhere)
// depending on position along it.
int    layout_neighbor(int disp, edge_t edge, int32_t gx, int32_t gy);

// Map a global point to a host absolute-report coordinate (0..ABS_AXIS_MAX) within
// `disp`'s rectangle.
void   layout_to_abs(int disp, int32_t gx, int32_t gy, uint16_t *ax, uint16_t *ay);

// The nudge axis/direction (each -1, 0, or +1) that moves a host's cursor from
// display `from` to adjacent display `to` on the same host.
void   layout_nudge_dir(int from, int to, int *ax, int *ay);

#endif
