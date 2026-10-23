// main/vocab_app.h - the Word Buddy app task (device side of the engine).
//
// DEVICE ONLY. The app task owns the engine, the view model, the persistent store and the
// key queue. It initializes battery, storage, audio and the UI, then loops: wait for a key
// event or the next engine deadline, feed the engine, execute its commands (audio requests,
// key-queue flush, backlight, save gate, battery reading, deep sleep) and render the view
// while holding the LVGL lock once per iteration.
//
// Owner: lead developer.
#pragma once

#include <stdint.h>

#include "bsp_button.h"
#include "esp_err.h"

// Creates the key queue (VOCAB_INPUT_QUEUE_DEPTH) and the app task. Called once by app_main
// after the display and LVGL are initialized. Returns ESP_ERR_NO_MEM on failure.
esp_err_t vocab_app_start(void);

// BSP button callback hand-off (shared esp_timer task): enqueues {key, event, time} with a
// zero timeout and returns; an event is dropped when the queue is full or the app has not
// started. Never blocks, never touches LVGL, audio or storage.
void vocab_app_post_key(bsp_btn_t btn, bsp_btn_ev_t ev, int64_t time_us);
