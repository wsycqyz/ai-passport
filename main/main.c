// main/main.c - Word Buddy: offline English vocabulary trainer for FoloToy AI Passport.
//
// Boot order: wake-cause check and key guard, shared I2C, display + LVGL (backlight stays off
// until the app has drawn HOME), the app task (vocab_app.c: battery, storage, audio, UI,
// engine), then the buttons. Startup always opens the vocabulary app (HOME); the baseline
// hardware-test menu is not part of this firmware.
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"      // pin numbers for the display error log
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "key_guard.h"
#include "vocab_app.h"

static const char *TAG = "main";

// Waking from deep sleep by key reboots the chip while that key is still held: the button
// component starts a few hundred milliseconds later, registers a press that is already
// down and grades it a long press after 500 ms. Drop key actions until that press comes
// up; the deadline is only a safety net for a stuck key (see key_guard.h).
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;
static volatile bool s_input_ready;

// Button callbacks run on the shared esp_timer task: hand the event to the app and return.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // Consume the guard before the readiness check: a release can arrive after the
    // post-init voltage sample but before input dispatch is ready, and dropping it here
    // would leave the guard armed to swallow the next real press.
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "wake key released; keys active");
        }
        return;
    }
    if (!s_input_ready) return;
    vocab_app_post_key(btn, ev, esp_timer_get_time());
}

void app_main(void)
{
    ESP_LOGI(TAG, "Word Buddy starting");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & ~(1u << ESP_SLEEP_WAKEUP_UNDEFINED)) {
        ESP_LOGI(TAG, "wake causes: 0x%02x", (unsigned)wake_causes);
    }
    // After a key wake that key is still held during the whole boot: block its action.
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "key wake: key actions blocked until that key is released");
    }

    bsp_i2c_init();

    // Without the display there is no UI: log the wiring and stop.
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed; check SPI wiring "
                      "(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(0);  // the app switches it on after drawing HOME

    esp_err_t err = vocab_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app task creation failed: %s", esp_err_to_name(err));
        return;
    }

    err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(err));
    } else if (s_key_guard.armed) {
        // If the wake key came up before the button driver started, no release event will
        // arrive: sync once with the real voltage and disarm when it is already released.
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) ESP_LOGI(TAG, "wake key released before the driver started");
    }
    s_input_ready = true;
}
