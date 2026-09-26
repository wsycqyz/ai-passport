// main/hm_ui.h —— The main page: the GitHub contribution calendar and the
// work-time bar.
//
// A screen of its own, in GitHub's dark palette. Left: 13 weeks (rows, newest
// at the bottom) by 7 days (Sunday..Saturday) of rounded squares, with month
// labels in the gutter (the first label and January also show the year).
// Right: the work-time bar, one block per work hour, green/yellow/red, with the
// hours left and the online dot (green: connected, red: not) underneath;
// "Clock not set" replaces the bar until the clock is set. Bottom: the
// Less-More legend. Nothing is drawn at the top. Without data the calendar is
// replaced by "No data" and a short reason.
//
// The calendar and the bar are painted by draw callbacks from private copies
// of their state, so no LVGL object exists per square (the LVGL pool is 24 KB),
// and each is repainted only when its state changed. Every function takes the
// LVGL lock itself: call them from the controller task, never from button
// callbacks.
#pragma once

#include "hm_view.h"
#include "work_bar.h"

#include <stdbool.h>

// Create the screen and make it active. Call once after bsp_lvgl_init().
bool hm_ui_init(void);

void hm_ui_show(void);

// empty_reason is shown under "No data" when page->has_data is false.
void hm_ui_set_page(const hm_page_t *page, const char *empty_reason);

void hm_ui_set_online(bool online);

void hm_ui_set_work_bar(const work_bar_t *bar);
