#ifndef CONTROL_H
#define CONTROL_H

#include "esp_err.h"

// The physical layout selector: the dev board's BOOT button (GPIO0) cycles the desk
// layout (see layout.h) and the on-board RGB LED flashes the selected layout's colour.
// The choice is saved to NVS and restored on boot. Call after nvs_flash_init() and
// kvm_init().
esp_err_t control_init(void);

#endif
