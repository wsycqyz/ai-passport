// main/app_ui.h —— Wi-Fi setup screens of the heatmap app (the "Wi-Fi by
// sound" module).
//
// One persistent LVGL screen, separate from the heatmap screen: a header (module
// name + battery), a content area rebuilt for each page, a single on-screen
// button activated by the OK key, and a hint that UP or DOWN returns to the
// heatmap. Showing any page makes this screen active. Every function takes the
// LVGL lock itself, so the controller task may call them directly; none may be
// called from button callbacks.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t level;          // microphone level 0..100
    bool clipping;
    bool receiving;
    uint16_t progress;      // body progress in permille while receiving
    const char *message;    // status line under the title
    int seconds_left;       // listening timeout countdown; < 0 hides it
} app_ui_listening_t;

// Create the screen (it is loaded by the first page shown). Call once after
// bsp_lvgl_init().
bool app_ui_init(void);

void app_ui_show_connecting(const char *ssid);
void app_ui_set_connecting(const char *stage, uint16_t progress);

void app_ui_show_setup(const char *note);

void app_ui_show_listening(void);
void app_ui_set_listening(const app_ui_listening_t *state);

void app_ui_show_failed(const char *title, const char *hint, const char *detail);

// Battery percentage in the top-right corner; negative hides it.
void app_ui_set_battery(int percent);
