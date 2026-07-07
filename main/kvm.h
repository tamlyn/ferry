#ifndef KVM_H
#define KVM_H

#include <stdint.h>

// The routing brain: decides which of the connected hosts input goes to, keeps a
// separate absolute cursor per host, and hops the cursor between machines when it
// is pushed off a screen edge toward an adjacent, ready host. Stack-independent —
// it talks to hosts only through ble_hid.h. Callbacks run on the USB input task.

// Reset per-host cursors and routing state. Call once at startup.
void kvm_init(void);

// Handle one USB mouse report: move the active host's cursor, hop if pushed off
// an edge toward an adjacent ready host, and send the result — including the
// wheel delta, which rides along untouched by the cursor model — to the active
// host.
void kvm_on_mouse(uint8_t buttons, int dx, int dy, int wheel);

// Handle one USB keyboard report: route it to the active host.
void kvm_on_keyboard(uint8_t modifiers, const uint8_t keys[6]);

#endif
