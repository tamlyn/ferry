#include "layout.h"

#include "our_descriptor.h"   // ABS_AXIS_MAX

// Host slots. ble_hid.c pins each machine to a fixed slot by its BLE identity (the
// Mac to SLOT_MAC), so these indices are stable regardless of connection order — the
// Mac is always HOST_MAC. Keep HOST_MAC in sync with SLOT_MAC in ble_hid.c.
#define HOST_MAC 0
#define HOST_PC  1

// ---- The desk: three displays, two layouts --------------------------------
//
// The three display rectangles are the same physical arrangement in both layouts;
// only which host drives each one changes. Measured on the dev Mac via
// CGDisplayBounds (points; y increases downward):
//   external : origin (-2560, -716) 2560x1440 — upper-left, its right edge meeting
//              the built-in's left edge over y 0..724 only.
//   built-in : origin (0, 0)       1512x982
// The PC is a separate machine, so its position is our model, not an OS fact: its
// top-right corner sits at the external's bottom-right corner (0, 724), extending
// left and down. Its resolution is a PLACEHOLDER (1920x1080) until the real panel is
// known.
enum { DISP_EXTERNAL, DISP_BUILTIN, DISP_PC, N_DISP };

static const rect_t s_rect[N_DISP] = {
    [DISP_EXTERNAL] = { -2560, -716,    0,  724 },
    [DISP_BUILTIN]  = {     0,    0, 1512,  982 },
    [DISP_PC]       = { -1920,  724,    0, 1804 },
};

// A layout is just which host owns each display. Config A: the external is plugged
// into the Mac. Config B: it's re-plugged into the PC — same physical spot, but now
// addressed via the PC. Because kvm.c decides same-host-nudge vs. host-switch from
// ownership at runtime and the rectangles never move, the adjacency table below is
// identical for both layouts; flipping the external's owner is the whole difference.
//   A: built-in left edge (y 0..724) <-> external is a same-host nudge; the external
//      bottom / PC top is a Mac<->PC switch.
//   B: those flip — external <-> built-in becomes a Mac<->PC switch, external <-> PC
//      becomes a same-host PC nudge.
static const int s_host[LAYOUT_COUNT][N_DISP] = {
    [LAYOUT_A] = { [DISP_EXTERNAL] = HOST_MAC, [DISP_BUILTIN] = HOST_MAC, [DISP_PC] = HOST_PC },
    [LAYOUT_B] = { [DISP_EXTERNAL] = HOST_PC,  [DISP_BUILTIN] = HOST_MAC, [DISP_PC] = HOST_PC },
};
static const char *const s_name[LAYOUT_COUNT] = {
    [LAYOUT_A] = "A (external on Mac)",
    [LAYOUT_B] = "B (external on PC)",
};
static int s_active = LAYOUT_A;

int    layout_display_count(void) { return N_DISP; }
int    layout_host_of(int disp)   { return s_host[s_active][disp]; }
rect_t layout_rect(int disp)      { return s_rect[disp]; }

int         layout_count(void)        { return LAYOUT_COUNT; }
int         layout_active(void)       { return s_active; }
const char *layout_name(int id)       { return (id >= 0 && id < LAYOUT_COUNT) ? s_name[id] : "?"; }
void        layout_set_active(int id) { s_active = (id >= 0 && id < LAYOUT_COUNT) ? id : LAYOUT_A; }

int layout_home_display(int host)
{
    for (int i = 0; i < N_DISP; i++) {
        if (s_host[s_active][i] == host) {
            return i;
        }
    }
    return 0;
}

// Explicit edge adjacency: which neighbouring display the cursor crosses to when
// pushed off an edge, and over what span of that edge (y for LEFT/RIGHT, x for
// TOP/BOTTOM). This is deliberately *not* pure rectangle geometry — the handoffs
// are a choice: the Mac's left edge is split in half between the external and the
// PC even though the external physically borders three-quarters of it, and the
// whole of the PC's right edge crosses to the Mac even though the built-in is
// shorter (the cursor clamps into it on arrival). This table is what a future
// configurator would generate.
typedef struct {
    edge_t  edge;
    int32_t lo, hi;   // span along the edge
    int     neighbor;
} adj_t;

static const adj_t s_adj_external[] = {
    { EDGE_RIGHT,  -716,  724, DISP_BUILTIN },   // -> Mac built-in (same host)
    { EDGE_BOTTOM, -1920,   0, DISP_PC },        // -> PC
};
static const adj_t s_adj_builtin[] = {
    { EDGE_LEFT,     0,  491, DISP_EXTERNAL },   // top half -> external (same host)
    { EDGE_LEFT,   491,  982, DISP_PC },         // bottom half -> PC
};
static const adj_t s_adj_pc[] = {
    { EDGE_RIGHT,  724, 1804, DISP_BUILTIN },    // whole right edge -> Mac built-in
    { EDGE_TOP,  -1920,    0, DISP_EXTERNAL },   // -> external
};
static const struct {
    const adj_t *a;
    int          n;
} s_adj[] = {
    [DISP_EXTERNAL] = { s_adj_external, 2 },
    [DISP_BUILTIN]  = { s_adj_builtin,  2 },
    [DISP_PC]       = { s_adj_pc,       2 },
};

int layout_neighbor(int disp, edge_t edge, int32_t gx, int32_t gy)
{
    int32_t pos = (edge == EDGE_LEFT || edge == EDGE_RIGHT) ? gy : gx;
    for (int i = 0; i < s_adj[disp].n; i++) {
        const adj_t *a = &s_adj[disp].a[i];
        if (a->edge == edge && a->lo <= pos && pos < a->hi) {
            return a->neighbor;
        }
    }
    return -1;
}

void layout_to_abs(int disp, int32_t gx, int32_t gy, uint16_t *ax, uint16_t *ay)
{
    rect_t r = s_rect[disp];
    int32_t x = (int32_t)((int64_t)(gx - r.x0) * ABS_AXIS_MAX / (r.x1 - r.x0));
    int32_t y = (int32_t)((int64_t)(gy - r.y0) * ABS_AXIS_MAX / (r.y1 - r.y0));
    *ax = (uint16_t)(x < 0 ? 0 : (x > ABS_AXIS_MAX ? ABS_AXIS_MAX : x));
    *ay = (uint16_t)(y < 0 ? 0 : (y > ABS_AXIS_MAX ? ABS_AXIS_MAX : y));
}

void layout_nudge_dir(int from, int to, int *ax, int *ay)
{
    rect_t a = s_rect[from], b = s_rect[to];
    int32_t ddx = (b.x0 + b.x1) / 2 - (a.x0 + a.x1) / 2;
    int32_t ddy = (b.y0 + b.y1) / 2 - (a.y0 + a.y1) / 2;
    int32_t mx = ddx < 0 ? -ddx : ddx;
    int32_t my = ddy < 0 ? -ddy : ddy;
    if (mx >= my) {
        *ax = ddx > 0 ? 1 : -1;
        *ay = 0;
    } else {
        *ax = 0;
        *ay = ddy > 0 ? 1 : -1;
    }
}
