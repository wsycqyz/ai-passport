// main/hm_ui.h —— The main page: the GitHub contribution calendar.
//
// A screen of its own, in GitHub's dark palette: 13 weeks (rows, newest at the
// bottom) by 7 days (Sunday..Saturday) of rounded squares, month labels in the
// left gutter (the first label and January also show the year), the online
// dot at the bottom left (green: connected, red: not) and the Less-More legend
// at the bottom right. Nothing else is drawn, in particular nothing at the top.
// Without data the calendar is replaced by "No data" and a short reason.
//
// The calendar is painted by one object's draw callback from a private copy
// of the page, so no LVGL object exists per cell (the LVGL pool is 24 KB).
// Every function takes the LVGL lock itself: call them from the controller
// task, never from button callbacks.
#pragma once

#include "hm_view.h"

#include <stdbool.h>

// Create the screen and make it active. Call once after bsp_lvgl_init().
bool hm_ui_init(void);

void hm_ui_show(void);

// empty_reason is shown under "No data" when page->has_data is false.
void hm_ui_set_page(const hm_page_t *page, const char *empty_reason);

void hm_ui_set_online(bool online);
