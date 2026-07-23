#ifndef KVM_H
#define KVM_H

#include <stdint.h>

// The routing brain: keeps one active cursor moving through a global desk model of
// all the hosts' displays (see layout.h), routes input to whichever host owns the
// display the cursor is on, and crosses the cursor between displays when it is
// pushed off an edge — nudging it with relative motion across a same-host seam, or
// switching which host we drive across a host boundary. Stack-independent: it talks
// to hosts only through ble_hid.h. Callbacks run on the USB input task.

// Reset routing state. Call once at startup.
void kvm_init(void);

// Handle one USB mouse report: move the active cursor, cross to a neighbouring
// display if pushed off an edge, and send the resulting absolute position (plus the
// wheel and horizontal pan, which ride along untouched) to the host that owns the
// current display.
void kvm_on_mouse(uint8_t buttons, int dx, int dy, int wheel, int pan);

// Handle one USB keyboard report: route it to the host owning the current display.
void kvm_on_keyboard(uint8_t modifiers, const uint8_t keys[6]);

// Request a switch to layout `layout_id` (see layout.h). Safe to call from any task:
// the switch is applied on the input task just before the next report, re-homing the
// cursor so no stale per-host display state carries over. A no-op if already active.
void kvm_request_layout(int layout_id);

#endif
