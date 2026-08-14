#include "kvm.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "ble_hid.h"
#include "cursor.h"
#include "layout.h"

static const char *TAG = "kvm";

// Crossing a display seam on a host we position absolutely. Absolute positioning is
// clamped to the display that host's cursor is on and cannot cross it, so we:
// (1) absolute-position it at the shared seam on its *current* display, (2) walk it
// across with a short relative nudge, (3) absolute-position it at the entry point on
// the new display (step 3 is the caller's normal send). Pre-positioning first means
// the nudge only has to clear the seam — so it stays small (no overshoot "dance") and
// it works even when the cursor was parked outside the seam's overlap band (e.g.
// below where the external borders the built-in). A host driven relatively needs none
// of this: its own seams cost it nothing, and it is placed by resync_relative.
#define NUDGE_STEP        10   // relative counts per report — deliberately gentle
#define NUDGE_STEP_MS      8
#define SEAM_COUNTS       30   // total nudge; we start at the seam, so this is small.
                               // Bump if a crossing fails to take (e.g. macOS sticky
                               // display edges resisting it).
#define PREPOS_SETTLE_MS  15   // let the host apply the pre-position before nudging

// Points of overshoot that must accumulate against one edge before a shove counts as
// a deliberate crossing rather than a fast flick that merely reached the edge. Kept
// low: at ACCEL_MAX_GAIN a fast flick overshoots ~30 points per report, so this is a
// couple of reports of continued push — enough to reject a single stray report,
// short enough not to feel like the edge is resisting.
#define EDGE_PUSH_THRESHOLD 60

// Far enough past any desktop to pin the cursor against its far side whatever the
// host's resolution — the point of a 16-bit relative axis.
#define SLAM 32767

// The MX Master's gesture paddle arrives as button 6 (bit 5). Rather than forward it
// as a raw button the host has no default action for, Ferry turns a paddle *press*
// into the active host's desktop-overview gesture: macOS Mission Control (Ctrl+Up)
// or Windows Task View (Win+Tab). Ctrl+Up is the stock macOS shortcut — the dedicated
// Mission Control key is a Consumer-page usage our boot keyboard can't send, and it
// relies on that shortcut being enabled (System Settings > Keyboard > Shortcuts).
// The Mac is always host slot 0 (HOST_MAC in layout.c / SLOT_MAC in ble_hid.c).
#define BTN_PADDLE  0x20
#define HOST_MAC    0
#define MOD_LCTRL   0x01
#define MOD_LGUI    0x08
#define KEY_UP      0x52
#define KEY_TAB     0x2B

// The one active cursor lives in global desk points; s_active_disp/host say which
// display (and host) it is on. s_os_disp remembers, per host, which of that host's
// displays its OS cursor is parked on — needed because absolute positioning only
// addresses a host's *current* display, so reaching any other display of that host
// needs a nudge. s_buttons is the last button state, re-asserted by a crossing's
// pre-position report so a held drag survives it. -1 = display not yet known.
static cursor_t s_cursor;
static int s_active_host = -1;
static int s_active_disp = -1;
static int s_os_disp[BLE_HID_MAX_HOSTS];
static uint8_t s_buttons;
static bool s_paddle_down;   // previous gesture-paddle state, for press-edge detection

// A layout switch requested from another task (e.g. control.c's button). Applied on
// the input task at the next report — see apply_pending_layout. -1 = nothing pending.
static volatile int s_pending_layout = -1;

void kvm_init(void)
{
    for (int i = 0; i < BLE_HID_MAX_HOSTS; i++) {
        s_os_disp[i] = -1;
    }
    s_active_host = -1;
    s_active_disp = -1;
    s_buttons = 0;
}

void kvm_request_layout(int layout_id)
{
    // Just record the request; the input task applies it (apply_pending_layout) so the
    // switch and cursor re-home never race a report in flight. Presses are human-paced,
    // so a request landing in the brief window before the input task consumes the last
    // one is corrected by the next press.
    s_pending_layout = layout_id;
}

// Apply a pending layout switch on the input task. Re-homing via kvm_init drops the
// per-host display memory, which is meaningful only within one layout; the cursor
// re-homes (to the first ready host) on this same report.
static void apply_pending_layout(void)
{
    int id = s_pending_layout;
    if (id < 0) {
        return;
    }
    s_pending_layout = -1;
    if (id != layout_active()) {
        layout_set_active(id);
        kvm_init();
        ESP_LOGI(TAG, "layout -> %s", layout_name(id));
    }
}

static bool host_drivable(int host)
{
    return ble_hid_ready(host) &&
           (!layout_host_relative(host) || ble_hid_rel_ready(host));
}

// A host that is connected and subscribed, preferring the current active one.
static int pick_ready(void)
{
    if (s_active_host >= 0 && host_drivable(s_active_host)) {
        return s_active_host;
    }
    for (int i = 0; i < BLE_HID_MAX_HOSTS; i++) {
        if (host_drivable(i)) {
            return i;
        }
    }
    return -1;
}

// Walk a host's cursor with a burst of relative motion. Sent with no buttons, so
// the host's button state (last sent on the absolute report) rides through
// untouched — on macOS that is the only place clicks may live (our_descriptor.c).
// Blocks the caller (the USB input task) for the burst; acceptable for a
// deliberate, occasional crossing.
static void nudge_burst(int host, int ax, int ay, int counts)
{
    int reports = counts / NUDGE_STEP;
    for (int i = 0; i < reports; i++) {
        ble_hid_send_mouse_rel(host, 0, (int16_t)(ax * NUDGE_STEP), (int16_t)(ay * NUDGE_STEP), 0, 0);
        vTaskDelay(pdMS_TO_TICKS(NUDGE_STEP_MS));
    }
}

// Put a relative host's cursor at a known place. We cannot ask where it is — nothing
// comes back from a host — so instead we pin it into a corner of its desktop with two
// over-range reports, which the host clamps, and walk it from there to (gx, gy) with
// one exact delta. Exact because the host is configured to apply relative motion 1:1
// (no pointer acceleration), which is what lets this model stay true rather than
// merely dead-reckoned. Doing it on every arrival also quietly repairs any drift from
// something else having moved that cursor: the machine's own trackpad, an application
// warping it, a dropped report.
static void resync_relative(int host, int32_t gx, int32_t gy)
{
    int32_t cx, cy;
    if (layout_resync_corner(host, &cx, &cy) < 0) {
        return;
    }
    // Buttons are left clear: a drag cannot span two machines, and the caller's own
    // report re-asserts whatever is held a moment later.
    ble_hid_send_mouse_rel(host, 0,  SLAM,     0, 0, 0);
    ble_hid_send_mouse_rel(host, 0,     0, -SLAM, 0, 0);
    ble_hid_send_mouse_rel(host, 0, (int16_t)(gx - cx), (int16_t)(gy - cy), 0, 0);
}

// Begin driving `host`: place the active cursor at the centre of the display its OS
// cursor is believed to be on (its home display if we've never driven it).
static void activate_host(int host)
{
    int disp = (s_os_disp[host] >= 0) ? s_os_disp[host] : layout_home_display(host);
    s_os_disp[host] = disp;
    rect_t r = layout_rect(disp);
    int32_t cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2;
    cursor_init(&s_cursor, cx, cy);
    s_active_host = host;
    s_active_disp = disp;
    if (layout_host_relative(host)) {
        resync_relative(host, cx, cy);
    }
    ESP_LOGI(TAG, "active host -> %d, display %d", host, disp);
}

// Move host `nh`'s cursor from its current display `cd` onto adjacent display `nd`:
// absolute-position it at the seam on cd (clamping the entry point back into cd),
// then nudge across. The caller asserts the final absolute position on nd.
static void nudge_across(int nh, int cd, int nd, int32_t ex, int32_t ey)
{
    rect_t cr = layout_rect(cd);
    int32_t px = ex < cr.x0 ? cr.x0 : (ex > cr.x1 ? cr.x1 : ex);
    int32_t py = ey < cr.y0 ? cr.y0 : (ey > cr.y1 ? cr.y1 : ey);
    uint16_t ax, ay;
    layout_to_abs(cd, px, py, &ax, &ay);
    ble_hid_send_mouse(nh, s_buttons, ax, ay, 0, 0);   // pre-position at the seam on cd
    vTaskDelay(pdMS_TO_TICKS(PREPOS_SETTLE_MS));

    int ndx, ndy;
    layout_nudge_dir(cd, nd, &ndx, &ndy);
    nudge_burst(nh, ndx, ndy, SEAM_COUNTS);          // clear the seam onto nd
}

// Cross the active cursor onto neighbouring display `nd`, entering at global point
// (gx, gy) clamped into it. Does whatever the destination requires: nothing at all
// for a seam its host walks by itself, a nudge across the seam for a host we position
// absolutely, or a re-synchronisation when arriving at a relative host from the other
// machine. Returns true if it moved that host's cursor itself, in which case the
// caller must not also send it this report's motion.
static bool cross_to(int nd, int32_t gx, int32_t gy)
{
    int nh = layout_host_of(nd);
    rect_t nr = layout_rect(nd);
    int32_t ex = gx < nr.x0 ? nr.x0 : (gx > nr.x1 ? nr.x1 : gx);
    int32_t ey = gy < nr.y0 ? nr.y0 : (gy > nr.y1 ? nr.y1 : gy);

    // The display nh's cursor is on now (which we must cross from).
    int cd = (nh == s_active_host) ? s_active_disp
                                   : (s_os_disp[nh] >= 0 ? s_os_disp[nh] : nd);
    bool placed = false;
    if (layout_host_relative(nh)) {
        // Its own seams need no help; only arriving from the other machine does,
        // because we have no idea where this one left its cursor.
        if (nh != s_active_host) {
            ESP_LOGI(TAG, "switch to host %d display %d [resync]", nh, nd);
            resync_relative(nh, ex, ey);
            placed = true;
        }
    } else if (cd != nd) {
        ESP_LOGI(TAG, "cross to host %d display %d (from %d)%s",
                 nh, nd, cd, nh == s_active_host ? "" : " [switch]");
        nudge_across(nh, cd, nd, ex, ey);
        placed = true;
    } else if (nh != s_active_host) {
        ESP_LOGI(TAG, "switch to host %d display %d", nh, nd);
    }
    s_os_disp[nh] = nd;
    s_active_host = nh;
    s_active_disp = nd;
    cursor_init(&s_cursor, ex, ey);
    return placed;
}

// Fire the active host's desktop-overview shortcut as a brief keyboard chord. Blocks
// the input task ~20 ms for the press then release, like a seam nudge — fine for a
// human-paced button. A held physical modifier is momentarily cleared by the release
// and restored by the keyboard's next report; not worth tracking for a rare press.
static void send_overview(int host)
{
    uint8_t mod = (host == HOST_MAC) ? MOD_LCTRL : MOD_LGUI;
    uint8_t key = (host == HOST_MAC) ? KEY_UP    : KEY_TAB;
    const uint8_t press[6]   = { key, 0, 0, 0, 0, 0 };
    const uint8_t release[6] = { 0 };
    ble_hid_send_keyboard(host, mod, press);
    vTaskDelay(pdMS_TO_TICKS(20));
    ble_hid_send_keyboard(host, 0, release);
}

void kvm_on_mouse(uint8_t buttons, int dx, int dy, int wheel, int pan)
{
    apply_pending_layout();
    int host = pick_ready();
    if (host < 0) {
        return;   // no ready host to drive
    }
    if (host != s_active_host) {
        activate_host(host);
    }

    // Where the model stood before this report. A relative host is sent the distance
    // the model actually travelled — after clamping, and across any seam it walked —
    // so that its cursor goes exactly where the model went and nowhere else.
    int32_t was_x = s_cursor.x, was_y = s_cursor.y;
    bool placed = false;

    cursor_step_t step;
    cursor_move(&s_cursor, dx, dy, layout_rect(s_active_disp), &step);
    if (step.edge != EDGE_NONE) {
        int nd = layout_neighbor(s_active_disp, step.edge, s_cursor.x, s_cursor.y);
        if (nd >= 0) {
            // A relative host walks its own display seam as the motion arrives, so
            // the model has to follow it straight across — holding at the edge to
            // wait for a shove would leave the two out of step. Everything else is a
            // deliberate crossing and has to be pushed for.
            bool follows = layout_host_of(nd) == s_active_host &&
                           layout_host_relative(s_active_host);
            if (follows) {
                cross_to(nd, step.raw_x, step.raw_y);
            } else if (step.push >= EDGE_PUSH_THRESHOLD) {
                placed = cross_to(nd, s_cursor.x, s_cursor.y);
            }
        }
    }

    // The gesture paddle (button 6) drives desktop overview, not a raw button — fire
    // it on the press edge and mask it out so the host never sees it as a click.
    bool paddle_pressed = (buttons & BTN_PADDLE) && !s_paddle_down;
    s_paddle_down = buttons & BTN_PADDLE;
    buttons &= ~BTN_PADDLE;

    if (layout_host_relative(s_active_host)) {
        // `placed` means the host's cursor was put where it belongs directly, so this
        // report's motion has already been accounted for; was_x/was_y also refer to
        // the machine we just left, and differencing across that would be nonsense.
        int32_t mx = placed ? 0 : s_cursor.x - was_x;
        int32_t my = placed ? 0 : s_cursor.y - was_y;
        ble_hid_send_mouse_rel(s_active_host, buttons, (int16_t)mx, (int16_t)my,
                               (int8_t)wheel, (int8_t)pan);
    } else {
        uint16_t ax, ay;
        layout_to_abs(s_active_disp, s_cursor.x, s_cursor.y, &ax, &ay);
        ble_hid_send_mouse(s_active_host, buttons, ax, ay, (int8_t)wheel, (int8_t)pan);
    }
    s_buttons = buttons;

    // After the position report, so the ~20 ms chord doesn't delay the cursor.
    if (paddle_pressed) {
        send_overview(s_active_host);
    }
}

void kvm_on_keyboard(uint8_t modifiers, const uint8_t keys[6])
{
    apply_pending_layout();
    int host = pick_ready();
    if (host < 0) {
        return;
    }
    if (host != s_active_host) {
        activate_host(host);
    }
    ble_hid_send_keyboard(s_active_host, modifiers, keys);
}
