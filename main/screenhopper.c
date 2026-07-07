// Screen Hopper (ESP32-S3) — M2: USB input → absolute cursor → BLE HID.
//
// The single-host bridge: a USB mouse and keyboard (via a hub) are hosted, the
// mouse's relative motion is accumulated into an absolute virtual cursor, and
// both are re-transmitted to one paired computer as a BLE HID peripheral — full
// pointer + keyboard control with no host-side software.

#include <stdint.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "ble_hid.h"
#include "cursor.h"
#include "usb_input.h"

static const char *TAG = "screenhopper";

// A USB mouse report: move the virtual cursor by the relative delta and send its
// new absolute position (plus button state) to the host.
static void on_mouse(uint8_t buttons, int dx, int dy)
{
    cursor_apply_delta(dx, dy);
    ble_hid_send_mouse(buttons, cursor_x(), cursor_y());
}

// A USB keyboard report: pass the modifiers + keycodes straight through.
static void on_keyboard(uint8_t modifiers, const uint8_t keys[6])
{
    ble_hid_send_keyboard(modifiers, keys);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Screen Hopper (ESP32-S3) — M2: USB → absolute cursor → BLE HID");

    // NVS holds the BLE bonding keys, so paired hosts reconnect without re-pairing.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    cursor_reset();
    ESP_ERROR_CHECK(ble_hid_init());
    ESP_ERROR_CHECK(usb_input_start(on_mouse, on_keyboard));

    ESP_LOGI(TAG, "ready — pair with a host, then drive the USB mouse and keyboard");
}
