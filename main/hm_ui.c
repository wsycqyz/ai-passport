// main/hm_ui.c —— see hm_ui.h.
#include "hm_ui.h"

#include "hm_calendar.h"

#include "bsp_display.h"
#include "bsp_pins.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define SHOW_LOCK_MS  1000

// GitHub's dark theme; level 0 is lifted slightly so empty days stay visible
// on the TFT.
#define C_BG          0x0D1117
#define C_TEXT        0xE6EDF3
#define C_MUTED       0x8B949E
#define C_FAINT       0x6E7681
#define C_PENDING     0x30363D   // outline of a day that is still downloading
#define C_ONLINE      0x3FB950
#define C_OFFLINE     0xF85149

static const uint32_t LEVEL_COLOR[GH_LEVEL_MAX + 1] = {
    0x21262D, 0x0E4429, 0x006D32, 0x26A641, 0x39D353,
};

// Layout of the 240x320 portrait screen. The display masks the corners with
// a 30 px radius, so the grid (x 51..187) and the bottom bar stay inside it.
#define CELL          17
#define PITCH         20
#define CELL_RADIUS   3
#define GRID_W        (HM_COLS * PITCH - (PITCH - CELL))
#define GRID_H        (HM_ROWS * PITCH - (PITCH - CELL))
#define GRID_X        ((BSP_LCD_W - GRID_W) / 2)
#define GRID_Y        18
#define LABEL_X       4
#define LABEL_GAP     7            // gutter labels end this far left of the grid
#define LABEL_LINE    15           // Montserrat 12 line height
#define CANVAS_H      (GRID_Y + GRID_H + 2 * LABEL_LINE)   // room for a year under the last row
#define BAR_CY        302          // centre line of the status dot and the legend
#define DOT_D         10
#define DOT_CX        (GRID_X - LABEL_GAP - DOT_D)
#define KEY_CELL      11
#define KEY_GAP       3

static lv_obj_t *s_screen;
static lv_obj_t *s_canvas;
static lv_obj_t *s_dot;
static lv_obj_t *s_empty_title;
static lv_obj_t *s_empty_reason;
static bool s_online;

// Read by the draw callback in the LVGL task; written under the LVGL lock.
static hm_page_t s_page;
static char s_year_text[HM_MAX_LABELS][8];

static bool overlaps(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

// Runs in the LVGL task for every refreshed band of the canvas; the layer's
// buffer area is that band, so cells and labels outside it are skipped.
static void draw_calendar(lv_event_t *e)
{
    if (!s_page.has_data) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t box;
    lv_obj_get_coords(s_canvas, &box);

    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.radius = CELL_RADIUS;
    fill.bg_opa = LV_OPA_COVER;
    lv_draw_rect_dsc_t pending;
    lv_draw_rect_dsc_init(&pending);
    pending.radius = CELL_RADIUS;
    pending.bg_opa = LV_OPA_TRANSP;
    pending.border_width = 1;
    pending.border_opa = LV_OPA_COVER;
    pending.border_color = lv_color_hex(C_PENDING);

    for (int r = 0; r < HM_ROWS; r++) {
        for (int c = 0; c < HM_COLS; c++) {
            const uint8_t level = s_page.level[r][c];
            if (level == HM_LEVEL_NONE) continue;
            lv_area_t cell = {
                .x1 = box.x1 + GRID_X + c * PITCH,
                .y1 = box.y1 + GRID_Y + r * PITCH,
            };
            cell.x2 = cell.x1 + CELL - 1;
            cell.y2 = cell.y1 + CELL - 1;
            if (!overlaps(&cell, &layer->buf_area)) continue;
            if (level <= GH_LEVEL_MAX) {
                fill.bg_color = lv_color_hex(LEVEL_COLOR[level]);
                lv_draw_rect(layer, &fill, &cell);
            } else {
                lv_draw_rect(layer, &pending, &cell);
            }
        }
    }

    lv_draw_label_dsc_t text;
    lv_draw_label_dsc_init(&text);
    text.font = &lv_font_montserrat_12;
    text.align = LV_TEXT_ALIGN_RIGHT;
    for (int i = 0; i < s_page.label_count; i++) {
        const hm_label_t *label = &s_page.labels[i];
        // Centred on the 17 px row: Montserrat 12 caps sit 4..13 px into the line.
        lv_area_t line = {
            .x1 = box.x1 + LABEL_X,
            .x2 = box.x1 + GRID_X - LABEL_GAP - 1,
            .y1 = box.y1 + GRID_Y + label->row * PITCH + 1,
        };
        line.y2 = line.y1 + LABEL_LINE - 1;
        if (overlaps(&line, &layer->buf_area)) {
            text.color = lv_color_hex(C_MUTED);
            text.text = hm_month_abbr(label->month);
            lv_draw_label(layer, &text, &line);
        }
        if (!label->show_year) continue;
        lv_area_move(&line, 0, LABEL_LINE);
        if (overlaps(&line, &layer->buf_area)) {
            text.color = lv_color_hex(C_FAINT);
            text.text = s_year_text[i];
            lv_draw_label(layer, &text, &line);
        }
    }
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *square(lv_obj_t *parent, int size, int radius, uint32_t color)
{
    lv_obj_t *obj = plain(parent);
    lv_obj_set_size(obj, size, size);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static lv_obj_t *text_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *value)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, value);
    return label;
}

// "Less [] [] [] [] [] More", right-aligned with the grid.
static void create_legend(void)
{
    lv_obj_t *row = plain(s_screen);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, KEY_GAP, 0);
    lv_obj_t *less = text_label(row, &lv_font_montserrat_12, C_MUTED, "Less");
    lv_obj_set_style_margin_right(less, 3, 0);
    for (int level = 0; level <= GH_LEVEL_MAX; level++) {
        square(row, KEY_CELL, 2, LEVEL_COLOR[level]);
    }
    lv_obj_t *more = text_label(row, &lv_font_montserrat_12, C_MUTED, "More");
    lv_obj_set_style_margin_left(more, 3, 0);
    lv_obj_align(row, LV_ALIGN_TOP_RIGHT, -(BSP_LCD_W - (GRID_X + GRID_W)), BAR_CY - LABEL_LINE / 2);
}

bool hm_ui_init(void)
{
    if (!bsp_lvgl_lock(SHOW_LOCK_MS)) return false;
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    s_canvas = plain(s_screen);
    lv_obj_set_size(s_canvas, BSP_LCD_W, CANVAS_H);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_add_event_cb(s_canvas, draw_calendar, LV_EVENT_DRAW_MAIN, NULL);

    s_empty_title = text_label(s_screen, &lv_font_montserrat_20, C_TEXT, "No data");
    lv_obj_align(s_empty_title, LV_ALIGN_TOP_MID, 0, 118);
    s_empty_reason = text_label(s_screen, &lv_font_montserrat_14, C_MUTED, "Starting...");
    lv_label_set_long_mode(s_empty_reason, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_empty_reason, 196);
    lv_obj_set_style_text_align(s_empty_reason, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_empty_reason, LV_ALIGN_TOP_MID, 0, 152);

    s_dot = square(s_screen, DOT_D, LV_RADIUS_CIRCLE, C_OFFLINE);
    lv_obj_set_pos(s_dot, DOT_CX - DOT_D / 2, BAR_CY - DOT_D / 2);
    create_legend();

    lv_screen_load(s_screen);
    bsp_lvgl_unlock();
    return true;
}

void hm_ui_show(void)
{
    if (!bsp_lvgl_lock(SHOW_LOCK_MS)) return;
    if (lv_screen_active() != s_screen) lv_screen_load(s_screen);
    bsp_lvgl_unlock();
}

void hm_ui_set_page(const hm_page_t *page, const char *empty_reason)
{
    if (!bsp_lvgl_lock(SHOW_LOCK_MS)) return;
    // Repaint the calendar only when a cell or label changed.
    if (memcmp(&s_page, page, sizeof(s_page)) != 0) {
        s_page = *page;
        for (int i = 0; i < page->label_count; i++) {
            snprintf(s_year_text[i], sizeof(s_year_text[i]), "%d", page->labels[i].year);
        }
        lv_obj_invalidate(s_canvas);
    }
    if (page->has_data) {
        lv_obj_add_flag(s_empty_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_empty_reason, LV_OBJ_FLAG_HIDDEN);
    } else {
        const char *reason = empty_reason ? empty_reason : "";
        if (strcmp(lv_label_get_text(s_empty_reason), reason) != 0) lv_label_set_text(s_empty_reason, reason);
        lv_obj_remove_flag(s_empty_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_empty_reason, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_lvgl_unlock();
}

void hm_ui_set_online(bool online)
{
    if (!bsp_lvgl_lock(SHOW_LOCK_MS)) return;
    if (online != s_online) {
        s_online = online;
        lv_obj_set_style_bg_color(s_dot, lv_color_hex(online ? C_ONLINE : C_OFFLINE), 0);
    }
    bsp_lvgl_unlock();
}
