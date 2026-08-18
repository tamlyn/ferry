// Physical layout selector: BOOT button + RGB status LED.
//
// The desk can be arranged in more than one way (external monitor on the Mac vs. on
// the PC — see layout.h). This module switches between them from the hardware: a press
// of the dev board's BOOT button (GPIO0) advances to the next layout, and the on-board
// WS2812 RGB LED flashes the selected layout's colour so you can see which one took.
// The selection is persisted in NVS and restored on the next boot.
//
// The LED is a non-essential indicator: if it fails to initialise (e.g. the wrong GPIO
// for this board revision) the KVM still runs and the button still switches layouts.

#include "control.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"
#include "nvs.h"

#include "kvm.h"
#include "layout.h"

static const char *TAG = "control";

// The dev board's BOOT button is on GPIO0 (active low, pulled up). It doubles as the
// download strapping pin, but a momentary press during normal operation is just an
// ordinary input.
#define BOOT_GPIO       0

// The 44-pin ESP32-S3-WROOM-1 dev board's addressable RGB LED. GPIO48 on most of these
// boards; some revisions use GPIO38 — if the LED never lights, try 38.
#define LED_GPIO        48
#define LED_BRIGHTNESS  48    // WS2812 at full brightness is harsh; a flash is enough
#define FLASH_MS        700   // how long the layout colour shows on a change

#define POLL_MS         20
#define DEBOUNCE_MS     30

#define NVS_NAMESPACE   "control"
#define NVS_KEY_LAYOUT  "layout"

static led_strip_handle_t s_strip;   // NULL if the LED failed to initialise
static int s_selected;               // this task's authoritative view of the choice

static void layout_colour(int id, uint8_t *r, uint8_t *g, uint8_t *b)
{
    // Config A = blue, Config B = green; anything unexpected = red.
    const uint8_t v = LED_BRIGHTNESS;
    switch (id) {
        case LAYOUT_A: *r = 0; *g = 0; *b = v; break;
        case LAYOUT_B: *r = 0; *g = v; *b = 0; break;
        default:       *r = v; *g = 0; *b = 0; break;
    }
}

static void flash_layout(int id)
{
    if (s_strip == NULL) {
        return;
    }
    uint8_t r, g, b;
    layout_colour(id, &r, &g, &b);
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
    vTaskDelay(pdMS_TO_TICKS(FLASH_MS));
    led_strip_clear(s_strip);
}

static void save_layout(int id)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_LAYOUT, (uint8_t)id);
    nvs_commit(h);
    nvs_close(h);
}

static int load_layout(void)
{
    nvs_handle_t h;
    uint8_t id = LAYOUT_A;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_LAYOUT, &id);   // leaves id unchanged if absent
        nvs_close(h);
    }
    return id < layout_count() ? id : LAYOUT_A;
}

static void advance_layout(void)
{
    s_selected = (s_selected + 1) % layout_count();
    kvm_request_layout(s_selected);
    save_layout(s_selected);
    ESP_LOGI(TAG, "layout -> %s", layout_name(s_selected));
    flash_layout(s_selected);
}

// Poll the BOOT button; one clean press advances the layout. Debounce by re-reading
// after a short delay, then wait for release so a held button doesn't cycle repeatedly.
static void control_task(void *arg)
{
    flash_layout(s_selected);   // announce the layout restored at boot
    for (;;) {
        if (gpio_get_level(BOOT_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            if (gpio_get_level(BOOT_GPIO) == 0) {
                advance_layout();
                while (gpio_get_level(BOOT_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

static void led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        s_strip = NULL;
        ESP_LOGW(TAG, "RGB LED init failed (%s) — layout switching still works",
                 esp_err_to_name(err));
        return;
    }
    led_strip_clear(s_strip);
}

esp_err_t control_init(void)
{
    led_init();

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    s_selected = load_layout();
    kvm_request_layout(s_selected);
    ESP_LOGI(TAG, "layout restored: %s", layout_name(s_selected));

    if (xTaskCreate(control_task, "control", 3072, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
