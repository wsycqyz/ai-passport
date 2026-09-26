// main/app_ui.c —— see app_ui.h. Dark "signal" theme drawn with LVGL
// primitives (arcs, lines, bars); only the built-in Montserrat 12/14/20 fonts
// are used, so all text is printable ASCII (dynamic SSIDs are sanitised by the
// caller with app_text_ssid()).
#include "app_ui.h"

#include "bsp_display.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHOW_LOCK_MS    1000
#define UPDATE_LOCK_MS  100

#define C_BG_TOP    0x0F1E30
#define C_BG_BOTTOM 0x081019
#define C_SURFACE   0x16283D
#define C_TRACK     0x27405C
#define C_TEXT      0xF1F5F9
#define C_MUTED     0x8FA3BF
#define C_ACCENT    0x2DD4BF
#define C_ON_ACCENT 0x06302D
#define C_OK        0x34D399
#define C_WARN      0xFBBF24
#define C_ERR       0xF87171
#define C_WHITE     0xFFFFFF

// Layout of the 240x320 portrait screen (corners are masked with a 30 px radius).
#define CONTENT_Y   40
#define CONTENT_H   206
#define BUTTON_Y    250
#define HINT_Y      296         // "UP / DOWN: back to heatmap" under the button
#define ICON_CY     48          // icon centre inside the content area
#define TITLE_Y     98
#define TEXT_W      212

#define LEVEL_BARS    11
#define LEVEL_HISTORY (LEVEL_BARS / 2 + 1)

typedef enum {
    PAGE_NONE = 0,
    PAGE_CONNECTING,
    PAGE_SETUP,
    PAGE_LISTENING,
    PAGE_FAILED,
} page_t;

static lv_obj_t *s_screen;
static lv_obj_t *s_content;
static lv_obj_t *s_button_label;
static lv_obj_t *s_battery;
static page_t s_page;

// Widgets of the current page; cleared whenever the content is rebuilt.
static lv_obj_t *s_arcs[3];
static uint8_t s_arc_step;
static lv_obj_t *s_title;
static lv_obj_t *s_detail;
static lv_obj_t *s_bar;
static lv_obj_t *s_countdown;
static lv_obj_t *s_levels[LEVEL_BARS];
static uint8_t s_history[LEVEL_HISTORY];

static const lv_point_precise_t CROSS_A[] = { { 22, 22 }, { 42, 42 } };
static const lv_point_precise_t CROSS_B[] = { { 42, 22 }, { 22, 42 } };

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *value)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, value ? value : "");
    return label;
}

// Horizontally centred label; single-line modes get an explicit one-line height.
static lv_obj_t *centred(int y, int width, const lv_font_t *font, uint32_t color,
                         const char *value, lv_label_long_mode_t mode)
{
    lv_obj_t *label = text(s_content, font, color, value);
    lv_label_set_long_mode(label, mode);
    lv_obj_set_width(label, width);
    if (mode != LV_LABEL_LONG_MODE_WRAP) lv_obj_set_height(label, lv_font_get_line_height(font));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static void set_text(lv_obj_t *label, const char *value)
{
    if (!label) return;
    if (!value) value = "";
    // Avoid redrawing labels whose text did not change (10 Hz updates).
    if (strcmp(lv_label_get_text(label), value) != 0) lv_label_set_text(label, value);
}

static lv_obj_t *disc(lv_obj_t *parent, int cx, int cy, int diameter, uint32_t color)
{
    lv_obj_t *obj = plain(parent);
    lv_obj_set_size(obj, diameter, diameter);
    lv_obj_set_pos(obj, cx - diameter / 2, cy - diameter / 2);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static void stroke(lv_obj_t *parent, const lv_point_precise_t *points, uint32_t count, int width)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, points, count);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(C_WHITE), 0);
}

static lv_obj_t *progress_bar(int y)
{
    lv_obj_t *bar = lv_bar_create(s_content);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bar, 176, 6);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, y);
    lv_bar_set_range(bar, 0, 1000);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    return bar;
}

static void begin_page(page_t page, const char *button)
{
    // The heatmap has its own screen; every setup page brings this one back.
    if (lv_screen_active() != s_screen) lv_screen_load(s_screen);
    lv_obj_clean(s_content);
    memset(s_arcs, 0, sizeof(s_arcs));
    memset(s_levels, 0, sizeof(s_levels));
    memset(s_history, 0, sizeof(s_history));
    s_arc_step = 0;
    s_title = s_detail = s_bar = s_countdown = NULL;
    s_page = page;
    lv_label_set_text(s_button_label, button);
}

// Three arcs above a dot: the Wi-Fi symbol. The animation timer lights the
// arcs one after another while connecting.
static void wifi_icon(int cx, int cy)
{
    static const int radius[3] = { 16, 29, 42 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *arc = lv_arc_create(s_content);
        lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(arc, 2 * radius[i], 2 * radius[i]);
        lv_obj_set_pos(arc, cx - radius[i], cy - radius[i]);
        lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
        lv_arc_set_bg_angles(arc, 225, 315);
        lv_obj_set_style_arc_width(arc, 7, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(C_TRACK), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
        s_arcs[i] = arc;
    }
    disc(s_content, cx, cy - 3, 12, C_ACCENT);
}

static void animate(lv_timer_t *timer)
{
    (void)timer;
    // Runs in the LVGL task, which already holds the LVGL lock.
    if (s_page != PAGE_CONNECTING || !s_arcs[0]) return;
    s_arc_step = (uint8_t)((s_arc_step + 1) % 4);
    for (int i = 0; i < 3; i++) {
        const uint32_t color = i < s_arc_step ? C_ACCENT : C_TRACK;
        lv_obj_set_style_arc_color(s_arcs[i], lv_color_hex(color), LV_PART_MAIN);
    }
}

bool app_ui_init(void)
{
    if (!bsp_lvgl_lock(SHOW_LOCK_MS)) return false;
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(C_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(s_screen, lv_color_hex(C_BG_BOTTOM), 0);
    lv_obj_set_style_bg_grad_dir(s_screen, LV_GRAD_DIR_VER, 0);

    disc(s_screen, 23, 21, 7, C_ACCENT);
    lv_obj_t *name = text(s_screen, &lv_font_montserrat_14, C_MUTED, "SOUND WI-FI");
    lv_obj_set_style_text_letter_space(name, 2, 0);
    lv_obj_set_pos(name, 32, 13);
    s_battery = text(s_screen, &lv_font_montserrat_14, C_MUTED, "");
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -26, 13);
    lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);

    s_content = plain(s_screen);
    lv_obj_set_pos(s_content, 0, CONTENT_Y);
    lv_obj_set_size(s_content, LV_HOR_RES, CONTENT_H);

    lv_obj_t *button = plain(s_screen);
    lv_obj_set_size(button, 192, 42);
    lv_obj_set_pos(button, 24, BUTTON_Y);
    lv_obj_set_style_radius(button, 21, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_t *key = plain(button);
    lv_obj_set_size(key, 34, 24);
    lv_obj_align(key, LV_ALIGN_LEFT_MID, 9, 0);
    lv_obj_set_style_radius(key, 8, 0);
    lv_obj_set_style_bg_color(key, lv_color_hex(C_ON_ACCENT), 0);
    lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
    lv_obj_center(text(key, &lv_font_montserrat_14, C_ACCENT, "OK"));
    s_button_label = text(button, &lv_font_montserrat_14, C_ON_ACCENT, "");
    lv_obj_align(s_button_label, LV_ALIGN_CENTER, 19, 0);

    lv_obj_t *hint = text(s_screen, &lv_font_montserrat_12, C_MUTED, "UP / DOWN: back to heatmap");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, HINT_Y);

    s_page = PAGE_NONE;
    lv_timer_create(animate, 280, NULL);
    bsp_lvgl_unlock();
    return true;
}

void app_ui_show_connecting(const char *ssid)
{
    if (!bsp_lvgl_lock(SHOW_LOCK_MS)) return;
    begin_page(PAGE_CONNECTING, "Re-configure Wi-Fi");
    wifi_icon(120, ICON_CY + 26);
    s_title = centred(TITLE_Y, TEXT_W, &lv_font_montserrat_20, C_TEXT, "Connecting", LV_LABEL_LONG_MODE_CLIP);
    centred(TITLE_Y + 30, 200, &lv_font_montserrat_14, C_ACCENT, ssid, LV_LABEL_LONG_MODE_DOTS);
    s_detail = centred(TITLE_Y + 52, TEXT_W, &lv_font_montserrat_14, C_MUTED, "Starting Wi-Fi...",
                       LV_LABEL_LONG_MODE_DOTS);
    s_bar = progress_bar(TITLE_Y + 84);
    bsp_lvgl_unlock();
}

void app_ui_set_connecting(const char *stage, uint16_t progress)
{
    if (!bsp_lvgl_lock(UPDATE_LOCK_MS)) return;
    if (s_page == PAGE_CONNECTING) {
        set_text(s_detail, stage);
        lv_bar_set_value(s_bar, progress > 1000 ? 1000 : progress, LV_ANIM_ON);
    }
    bsp_lvgl_unlock();
}

void app_ui_show_setup(const char *note)
{
    if (!bsp_lvgl_lock(SHOW_LOCK_MS)) return;
    begin_page(PAGE_SETUP, "Start listening");
    // Compact icon so the whole instruction fits between it and the button.
    const int icon_cy = 24;
    disc(s_content, 120, icon_cy, 42, C_SURFACE);
    static const int heights[5] = { 7, 15, 22, 15, 7 };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *bar = plain(s_content);
        lv_obj_set_size(bar, 4, heights[i]);
        lv_obj_set_pos(bar, 120 - 14 + i * 6, icon_cy - heights[i] / 2);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }
    s_title = centred(56, TEXT_W, &lv_font_montserrat_20, C_TEXT, "Set up Wi-Fi", LV_LABEL_LONG_MODE_CLIP);
    int y = 88;
    if (note) {
        centred(y, TEXT_W, &lv_font_montserrat_14, C_WARN, note, LV_LABEL_LONG_MODE_DOTS);
        y += 24;
    }
    // Listening must already run when the PC starts playing, hence this order.
    centred(y, TEXT_W, &lv_font_montserrat_14, C_MUTED,
            "Press OK to start listening, then play the Wi-Fi setup sound on your PC. "
            "Hold me near the speaker.",
            LV_LABEL_LONG_MODE_WRAP);
    bsp_lvgl_unlock();
}

void app_ui_show_listening(void)
{
    if (!bsp_lvgl_lock(SHOW_LOCK_MS)) return;
    begin_page(PAGE_LISTENING, "Cancel");
    const int pitch = 13;
    const int left = 120 - (LEVEL_BARS * pitch - 5) / 2;
    for (int i = 0; i < LEVEL_BARS; i++) {
        lv_obj_t *bar = plain(s_content);
        lv_obj_set_size(bar, 8, 6);
        lv_obj_set_pos(bar, left + i * pitch, ICON_CY - 3);
        lv_obj_set_style_radius(bar, 4, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        s_levels[i] = bar;
    }
    s_title = centred(TITLE_Y, TEXT_W, &lv_font_montserrat_20, C_TEXT, "Listening...", LV_LABEL_LONG_MODE_CLIP);
    s_detail = centred(TITLE_Y + 30, TEXT_W, &lv_font_montserrat_14, C_MUTED,
                       "Play the setup sound on your PC", LV_LABEL_LONG_MODE_WRAP);
    s_bar = progress_bar(TITLE_Y + 76);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    s_countdown = centred(TITLE_Y + 90, TEXT_W, &lv_font_montserrat_14, C_MUTED, "", LV_LABEL_LONG_MODE_CLIP);
    bsp_lvgl_unlock();
}

void app_ui_set_listening(const app_ui_listening_t *state)
{
    if (!bsp_lvgl_lock(UPDATE_LOCK_MS)) return;
    if (s_page != PAGE_LISTENING) {
        bsp_lvgl_unlock();
        return;
    }
    // The newest level sits in the centre and older ones ripple outwards.
    memmove(s_history + 1, s_history, sizeof(s_history) - 1);
    s_history[0] = state->level > 100 ? 100 : state->level;
    const uint32_t color = state->clipping ? C_WARN : (state->receiving ? C_OK : C_ACCENT);
    for (int i = 0; i < LEVEL_BARS; i++) {
        const int d = abs(i - LEVEL_BARS / 2);
        const int h = 6 + s_history[d] * (72 - 6 * d) / 100;
        lv_obj_set_height(s_levels[i], h);
        lv_obj_set_y(s_levels[i], ICON_CY - h / 2);
        lv_obj_set_style_bg_color(s_levels[i], lv_color_hex(color), 0);
    }
    set_text(s_title, state->receiving ? "Receiving..." : "Listening...");
    set_text(s_detail, state->message);
    if (state->receiving) {
        lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_bar, state->progress > 1000 ? 1000 : state->progress, LV_ANIM_ON);
    } else {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    }
    char countdown[24] = "";
    if (state->seconds_left >= 0) snprintf(countdown, sizeof(countdown), "Stops in %d s", state->seconds_left);
    set_text(s_countdown, countdown);
    bsp_lvgl_unlock();
}

void app_ui_show_failed(const char *title, const char *hint, const char *detail)
{
    if (!bsp_lvgl_lock(SHOW_LOCK_MS)) return;
    begin_page(PAGE_FAILED, "Re-configure Wi-Fi");
    lv_obj_t *badge = disc(s_content, 120, 40, 64, C_ERR);
    stroke(badge, CROSS_A, 2, 6);
    stroke(badge, CROSS_B, 2, 6);
    s_title = centred(84, TEXT_W, &lv_font_montserrat_20, C_TEXT, title, LV_LABEL_LONG_MODE_DOTS);
    s_detail = centred(114, TEXT_W, &lv_font_montserrat_14, C_MUTED, hint, LV_LABEL_LONG_MODE_WRAP);
    centred(188, TEXT_W, &lv_font_montserrat_14, C_MUTED, detail, LV_LABEL_LONG_MODE_DOTS);
    bsp_lvgl_unlock();
}

void app_ui_set_battery(int percent)
{
    if (!bsp_lvgl_lock(UPDATE_LOCK_MS)) return;
    if (percent < 0) {
        lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
    } else {
        const char *symbol = percent >= 90 ? LV_SYMBOL_BATTERY_FULL
                           : percent >= 65 ? LV_SYMBOL_BATTERY_3
                           : percent >= 40 ? LV_SYMBOL_BATTERY_2
                           : percent >= 15 ? LV_SYMBOL_BATTERY_1
                           : LV_SYMBOL_BATTERY_EMPTY;
        char value[24];
        snprintf(value, sizeof(value), "%s %d%%", symbol, percent > 100 ? 100 : percent);
        set_text(s_battery, value);
        lv_obj_set_style_text_color(s_battery, lv_color_hex(percent <= 15 ? C_ERR : C_MUTED), 0);
        lv_obj_remove_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_lvgl_unlock();
}
