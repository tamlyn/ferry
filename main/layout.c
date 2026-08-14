#include "layout.h"

#include "our_descriptor.h"   // ABS_AXIS_MAX

// Host slots. ble_hid.c pins each machine to a fixed slot by its BLE identity (the
// Mac to SLOT_MAC), so these indices are stable regardless of connection order — the
// Mac is always HOST_MAC. Keep HOST_MAC in sync with SLOT_MAC in ble_hid.c.
#define HOST_MAC 0
#define HOST_PC  1
#define N_HOST   2

// ---- The desk: three displays, two layouts --------------------------------
//
// The three display rectangles are the same physical arrangement in both layouts;
// only which host drives each one changes. Sizes are each display's own coordinate
// space as its owning host sees it — Mac points from CGDisplayBounds, Windows desktop
// pixels (physical resolution divided by its display scaling) for the PC. The PC's
// have to be exact, because a relatively-driven host is sent desk-point deltas
// verbatim and one desk point must therefore *be* one of its pixels; the Mac's only
// have to be the right shape, since absolute positioning normalises them away.
//   external : origin (-3072, -1004) 3072x1728 — upper-left, its right edge meeting
//              the built-in's left edge over y 0..724 only. That size is the PC's
//              view of it (3840x2160 at 125%), not the Mac's 2560x1440: layout B
//              needs it exact, and layout A does not care, since absolute positioning
//              only reads the rectangle's shape and both are 16:9. The Mac's pointer
//              therefore crosses it ~20% slower than it used to, which brings its
//              speed there closer to the built-in's in physical terms.
//   built-in : origin (0, 0)        1512x982
// The PC is a separate machine, so its position is our model, not an OS fact: its
// top-right corner sits at the external's bottom-right corner (0, 724), extending
// left and down. 1536x864 is its 1920x1080 panel at 125% scaling.
enum { DISP_EXTERNAL, DISP_BUILTIN, DISP_PC, N_DISP };

static const rect_t s_rect[N_DISP] = {
    [DISP_EXTERNAL] = { -3072, -1004,   0,  724 },
    [DISP_BUILTIN]  = {     0,    0, 1512,  982 },
    [DISP_PC]       = { -1536,  724,    0, 1588 },
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

// How each host's cursor is driven. macOS honours an absolute position on whichever
// display its cursor is on, so the Mac is positioned directly and a seam is crossed
// with a nudge. Windows honours absolute X/Y on its *primary* monitor only, leaving
// every other display unreachable, so the PC is driven with relative motion and its
// cursor is placed by re-synchronising against a desktop corner instead. That is a
// property of the machine, not of the desk, so it holds in both layouts.
static const bool s_host_relative[N_HOST] = { [HOST_MAC] = false, [HOST_PC] = true };

bool layout_host_relative(int host)
{
    return host >= 0 && host < N_HOST && s_host_relative[host];
}

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
    { EDGE_RIGHT, -1004,  724, DISP_BUILTIN },   // -> Mac built-in (same host)
    { EDGE_BOTTOM, -1536,   0, DISP_PC },        // -> PC
};
static const adj_t s_adj_builtin[] = {
    { EDGE_LEFT,     0,  491, DISP_EXTERNAL },   // top half -> external (same host)
    { EDGE_LEFT,   491,  982, DISP_PC },         // bottom half -> PC
};
static const adj_t s_adj_pc[] = {
    { EDGE_RIGHT,  724, 1588, DISP_BUILTIN },    // whole right edge -> Mac built-in
    { EDGE_TOP,  -1536,    0, DISP_EXTERNAL },   // -> external
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

// The corner an over-range relative report pins a host's cursor into: as far right,
// then as far up, as its desktop goes. Both of the PC's displays share their right
// edge, so a slam right lands on a wall common to both and a slam up then reaches the
// topmost one — which is what makes the corner reachable from anywhere without
// knowing where the cursor started. A desk where a host's displays are *not* flush on
// that side would need a corner both displays can reach, or this walks to a point
// that is off the host's desktop and the host clamps somewhere we don't expect.
int layout_resync_corner(int host, int32_t *gx, int32_t *gy)
{
    int best = -1;
    for (int i = 0; i < N_DISP; i++) {
        if (s_host[s_active][i] != host) {
            continue;
        }
        if (best < 0 || s_rect[i].x1 > s_rect[best].x1 ||
            (s_rect[i].x1 == s_rect[best].x1 && s_rect[i].y0 < s_rect[best].y0)) {
            best = i;
        }
    }
    if (best < 0) {
        return -1;
    }
    *gx = s_rect[best].x1;
    *gy = s_rect[best].y0;
    return best;
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
