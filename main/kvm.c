#include "kvm.h"

#include "esp_log.h"

#include "ble_hid.h"
#include "cursor.h"

static const char *TAG = "kvm";

// One cursor per host — each machine remembers where its pointer was left. Host
// order is connection order: slot 0 is the "left" screen, slot 1 the "right".
// Arranging screen positions is deferred to a later milestone (no wrap-around).
static cursor_t s_cursor[BLE_HID_MAX_HOSTS];
static int s_active;

void kvm_init(void)
{
    for (int i = 0; i < BLE_HID_MAX_HOSTS; i++) {
        cursor_reset(&s_cursor[i]);
    }
    s_active = 0;
}

// Pick a host to receive input, preferring the current active one. Returns -1 if
// no host is ready (nothing connected+subscribed yet).
static int pick_active(void)
{
    if (ble_hid_ready(s_active)) {
        return s_active;
    }
    for (int i = 0; i < BLE_HID_MAX_HOSTS; i++) {
        if (ble_hid_ready(i)) {
            return i;
        }
    }
    return -1;
}

void kvm_on_mouse(uint8_t buttons, int dx, int dy, int wheel)
{
    int active = pick_active();
    if (active < 0) {
        return;   // no ready host to drive
    }
    if (active != s_active) {
        ESP_LOGI(TAG, "active host -> %d (previous went away)", active);
        s_active = active;
    }

    cursor_edge_t edge = cursor_apply_delta(&s_cursor[s_active], dx, dy);

    if (edge != CURSOR_EDGE_NONE) {
        // The adjacent host: right edge -> next slot, left edge -> previous slot.
        int dest = (edge == CURSOR_EDGE_RIGHT) ? s_active + 1 : s_active - 1;
        if (dest >= 0 && dest < BLE_HID_MAX_HOSTS && ble_hid_ready(dest)) {
            // Enter the destination from the opposite edge at the same height, so
            // the pointer appears to cross the seam between the two screens.
            cursor_edge_t entry = (edge == CURSOR_EDGE_RIGHT) ? CURSOR_EDGE_LEFT
                                                              : CURSOR_EDGE_RIGHT;
            cursor_enter_from(&s_cursor[dest], entry, cursor_y(&s_cursor[s_active]));
            ESP_LOGI(TAG, "hop %d -> %d (off %s edge)", s_active, dest,
                     edge == CURSOR_EDGE_RIGHT ? "right" : "left");
            s_active = dest;
        }
    }

    ble_hid_send_mouse(s_active, buttons,
                       cursor_x(&s_cursor[s_active]), cursor_y(&s_cursor[s_active]),
                       (int8_t)wheel);
}

void kvm_on_keyboard(uint8_t modifiers, const uint8_t keys[6])
{
    int active = pick_active();
    if (active < 0) {
        return;
    }
    s_active = active;
    ble_hid_send_keyboard(s_active, modifiers, keys);
}
