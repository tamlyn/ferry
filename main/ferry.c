// Ferry (ESP32-S3) — M3: USB input → absolute cursor → BLE HID KVM.
//
// A USB mouse and keyboard (via a hub) are hosted and re-transmitted over BLE HID
// to up to two paired computers at once. The KVM brain routes input to whichever
// host has control and hops the cursor between machines when it is pushed off a
// screen edge — full pointer + keyboard control of two machines, no host-side
// software.

#include <stdint.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "ble_hid.h"
#include "control.h"
#include "diag.h"
#include "kvm.h"
#include "usb_input.h"

static const char *TAG = "ferry";

// A USB mouse report: hand the relative delta + button + scroll state to the KVM,
// which moves the active host's cursor (hopping between hosts at the screen
// edges) and sends the absolute position (plus the wheel and pan) on.
static void on_mouse(uint8_t buttons, int dx, int dy, int wheel, int pan)
{
    kvm_on_mouse(buttons, dx, dy, wheel, pan);
}

// A USB keyboard report: the KVM routes it to the active host.
static void on_keyboard(uint8_t modifiers, const uint8_t keys[6])
{
    kvm_on_keyboard(modifiers, keys);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Ferry (ESP32-S3) — M3: USB → absolute cursor → BLE HID KVM");
    diag_log_reset_reason();   // why did we (re)boot? a fault here is the freeze's trail

    // NVS holds the BLE bonding keys, so paired hosts reconnect without re-pairing.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    kvm_init();
    ESP_ERROR_CHECK(control_init());   // BOOT-button layout selector + RGB status LED
    ESP_ERROR_CHECK(ble_hid_init());
    ESP_ERROR_CHECK(usb_input_start(on_mouse, on_keyboard));
    diag_start();   // reboot-on-wedge watchdog + heap/mbuf watermark logging

    ESP_LOGI(TAG, "ready — pair up to two hosts, then drive the USB mouse and keyboard");
}
