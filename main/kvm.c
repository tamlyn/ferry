#include "kvm.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "ble_hid.h"
#include "cursor.h"
#include "layout.h"

static const char *TAG = "kvm";

// Crossing a display seam. Absolute positioning is clamped to the display a host's
// cursor is on and cannot cross it, so to move a host's cursor onto an adjacent
// display we: (1) absolute-position it at the shared seam on its *current* display,
// (2) walk it across with a short relative nudge, (3) absolute-position it at the
// entry point on the new display (step 3 is the caller's normal send). Pre-
// positioning first means the nudge only has to clear the seam — so it stays small
// (no overshoot "dance") and it works even when the cursor was parked outside the
// seam's overlap band (e.g. below where the external borders the built-in).
#define NUDGE_STEP        10   // relative counts per report — deliberately gentle
#define NUDGE_STEP_MS      8
#define SEAM_COUNTS       30   // total nudge; we start at the seam, so this is small.
                               // Bump if a crossing fails to take (e.g. macOS sticky
                               // display edges resisting it).
#define PREPOS_SETTLE_MS  15   // let the host apply the pre-position before nudging

// The one active cursor lives in global desk points; s_active_disp/host say which
// display (and host) it is on. s_os_disp remembers, per host, which of that host's
// displays its OS cursor is parked on — needed because absolute positioning only
// addresses a host's *current* display, so reaching any other display of that host
// needs a nudge. s_buttons is the last button state, carried through crossings and
// mirrored onto the relative report. -1 = display not yet known.
static cursor_t s_cursor;
static int s_active_host = -1;
static int s_active_disp = -1;
static int s_os_disp[BLE_HID_MAX_HOSTS];
static uint8_t s_buttons;

void kvm_init(void)
{
    for (int i = 0; i < BLE_HID_MAX_HOSTS; i++) {
        s_os_disp[i] = -1;
    }
    s_active_host = -1;
    s_active_disp = -1;
    s_buttons = 0;
}

// A host that is connected and subscribed, preferring the current active one.
static int pick_ready(void)
{
    if (s_active_host >= 0 && ble_hid_ready(s_active_host)) {
        return s_active_host;
    }
    for (int i = 0; i < BLE_HID_MAX_HOSTS; i++) {
        if (ble_hid_ready(i)) {
            return i;
        }
    }
    return -1;
}

// Walk a host's cursor with a burst of relative motion, holding the current button
// state so a drag survives the crossing. Blocks the caller (the USB input task) for
// the burst; acceptable for a deliberate, occasional crossing.
static void nudge_burst(int host, int ax, int ay, int counts)
{
    int reports = counts / NUDGE_STEP;
    for (int i = 0; i < reports; i++) {
        ble_hid_send_mouse_rel(host, s_buttons,
                               (int8_t)(ax * NUDGE_STEP), (int8_t)(ay * NUDGE_STEP));
        vTaskDelay(pdMS_TO_TICKS(NUDGE_STEP_MS));
    }
}

// Begin driving `host`: place the active cursor at the centre of the display its OS
// cursor is believed to be on (its home display if we've never driven it).
static void activate_host(int host)
{
    int disp = (s_os_disp[host] >= 0) ? s_os_disp[host] : layout_home_display(host);
    s_os_disp[host] = disp;
    rect_t r = layout_rect(disp);
    cursor_init(&s_cursor, (r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2);
    s_active_host = host;
    s_active_disp = disp;
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
    ble_hid_send_mouse(nh, s_buttons, ax, ay, 0);   // pre-position at the seam on cd
    vTaskDelay(pdMS_TO_TICKS(PREPOS_SETTLE_MS));

    int ndx, ndy;
    layout_nudge_dir(cd, nd, &ndx, &ndy);
    nudge_burst(nh, ndx, ndy, SEAM_COUNTS);          // clear the seam onto nd
}

// Cross the active cursor onto neighbouring display `nd`, doing whatever the layout
// requires: a same-host seam crossing, a plain host switch (new host already on the
// target display), or a host switch plus a crossing (new host parked elsewhere).
static void cross_to(int nd)
{
    int nh = layout_host_of(nd);
    rect_t nr = layout_rect(nd);
    // Entry point: keep the same global coords — continuous across the seam —
    // clamped into the destination rectangle.
    int32_t ex = s_cursor.x < nr.x0 ? nr.x0 : (s_cursor.x > nr.x1 ? nr.x1 : s_cursor.x);
    int32_t ey = s_cursor.y < nr.y0 ? nr.y0 : (s_cursor.y > nr.y1 ? nr.y1 : s_cursor.y);

    // The display nh's cursor is on now (which we must cross from).
    int cd = (nh == s_active_host) ? s_active_disp
                                   : (s_os_disp[nh] >= 0 ? s_os_disp[nh] : nd);
    if (cd != nd) {
        ESP_LOGI(TAG, "cross to host %d display %d (from %d)%s",
                 nh, nd, cd, nh == s_active_host ? "" : " [switch]");
        nudge_across(nh, cd, nd, ex, ey);
    } else if (nh != s_active_host) {
        ESP_LOGI(TAG, "switch to host %d display %d", nh, nd);
    }
    s_os_disp[nh] = nd;
    s_active_host = nh;
    s_active_disp = nd;
    cursor_init(&s_cursor, ex, ey);
}

void kvm_on_mouse(uint8_t buttons, int dx, int dy, int wheel)
{
    int host = pick_ready();
    if (host < 0) {
        return;   // no ready host to drive
    }
    if (host != s_active_host) {
        activate_host(host);
    }

    edge_t e = cursor_move(&s_cursor, dx, dy, layout_rect(s_active_disp));
    if (e != EDGE_NONE) {
        int nd = layout_neighbor(s_active_disp, e, s_cursor.x, s_cursor.y);
        if (nd >= 0) {
            cross_to(nd);
        }
    }

    uint16_t ax, ay;
    layout_to_abs(s_active_disp, s_cursor.x, s_cursor.y, &ax, &ay);
    ble_hid_send_mouse(s_active_host, buttons, ax, ay, (int8_t)wheel);

    // macOS binds clicks to the relative pointer (report 3) when the device exposes
    // both an absolute and a relative pointer, so mirror button changes onto it —
    // otherwise clicks (which ride the absolute report) are dropped on the Mac.
    // Windows reads the absolute report's buttons and is unaffected either way.
    if (buttons != s_buttons) {
        ble_hid_send_mouse_rel(s_active_host, buttons, 0, 0);
        s_buttons = buttons;
    }
}

void kvm_on_keyboard(uint8_t modifiers, const uint8_t keys[6])
{
    int host = pick_ready();
    if (host < 0) {
        return;
    }
    if (host != s_active_host) {
        activate_host(host);
    }
    ble_hid_send_keyboard(s_active_host, modifiers, keys);
}
