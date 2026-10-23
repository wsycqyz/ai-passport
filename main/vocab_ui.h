// main/vocab_ui.h - LVGL renderer of the Word Buddy view model (spec 11).
//
// DEVICE ONLY. vocab_ui_create() builds every widget once (about 60 objects incl. the
// 18-part pixel pet); afterwards objects are never created or deleted: vocab_ui_render()
// only shows/hides, sets texts, colors, fonts, positions and sizes, and only when a value
// changed (so an unchanged view causes no redraw). Fonts come from vocab_fonts.h and are set
// explicitly on every label; the word size follows the spec 11.3 rule measured with the real
// fonts (vocab_layout). Animations (spec 11.6) are lv_anim objects on the widgets: one-shot
// ones start when a view sequence counter changes (shake, pulse, nudge, confirm, pet eat),
// continuous ones follow view flags (blank blink, repeated nudge, pet idle). They change
// only position offsets, sizes, colors and opacities (no transforms, no layers).
//
// Caller: the app task, always while holding bsp_lvgl_lock() (LVGL is not thread-safe).
// Animation callbacks run in the LVGL task and touch only LVGL objects.
// Owner: lead developer.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocab_view.h"

// Builds the whole widget tree on a new screen and loads it. Returns false when LVGL could
// not allocate an object (the app logs it and keeps running without display updates).
bool vocab_ui_create(void);

// Applies `view`. Never creates or deletes objects. No-op before a successful create.
void vocab_ui_render(const vocab_view_t *view);

// Screen off / deep sleep: stops every animation and keeps them stopped while paused;
// unpausing lets the next render restart the continuous ones.
void vocab_ui_set_paused(bool paused);

// Number of completed display refreshes (LV_EVENT_REFR_READY, counted in the LVGL task).
// The app waits for it to advance after the first render so the backlight comes on over a
// drawn HOME. Safe to read without the lock (single 32-bit word).
uint32_t vocab_ui_frame_count(void);
