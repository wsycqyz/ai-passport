// main/hm_view.c —— see hm_view.h.
#include "hm_view.h"

#include "hm_calendar.h"

#include <string.h>

void hm_view_init(hm_view_t *v)
{
    v->bottom_week = 0;
    v->follow_today = true;
}

static int32_t newest_bottom(const hm_store_t *s)
{
    return hm_week_of(s->today);
}

// Bottom row of the page whose top row holds the first day of history.
static int32_t oldest_bottom(const hm_store_t *s)
{
    const int32_t bottom = hm_week_of(hm_store_first_day(s)) + HM_ROWS - 1;
    const int32_t newest = newest_bottom(s);
    return bottom < newest ? bottom : newest;
}

static void clamp(hm_view_t *v, const hm_store_t *s)
{
    const int32_t newest = newest_bottom(s);
    const int32_t oldest = oldest_bottom(s);
    if (v->follow_today || v->bottom_week > newest) v->bottom_week = newest;
    if (v->bottom_week < oldest) v->bottom_week = oldest;
    v->follow_today = v->bottom_week == newest;
}

void hm_view_sync(hm_view_t *v, const hm_store_t *s)
{
    if (s->have_last) clamp(v, s);
}

bool hm_view_scroll(hm_view_t *v, const hm_store_t *s, int direction)
{
    if (!s->have_last || direction == 0) return false;
    const int32_t before = v->bottom_week;
    v->follow_today = false;
    v->bottom_week += direction < 0 ? -HM_ROWS : HM_ROWS;
    clamp(v, s);
    return v->bottom_week != before;
}

void hm_view_to_today(hm_view_t *v, const hm_store_t *s)
{
    v->follow_today = true;
    hm_view_sync(v, s);
}

static void add_labels(hm_page_t *out, int32_t top_week)
{
    int prev_month = 0;
    for (int r = 0; r < HM_ROWS && out->label_count < HM_MAX_LABELS; r++) {
        int c = 0;
        while (c < HM_COLS && out->level[r][c] == HM_LEVEL_NONE) c++;
        if (c == HM_COLS) continue;   // nothing shown in this row
        int year, month;
        hm_civil_from_days(hm_week_start(top_week + r) + c, &year, &month, NULL);
        if (month == prev_month) continue;
        prev_month = month;
        out->labels[out->label_count++] = (hm_label_t){
            .row = (uint8_t)r, .month = (uint8_t)month, .year = (int16_t)year,
        };
    }
    // A partial month at the top only gets a label if it has room for it.
    if (out->label_count >= 2 && out->labels[1].row - out->labels[0].row < 2) {
        memmove(&out->labels[0], &out->labels[1], (out->label_count - 1) * sizeof(out->labels[0]));
        out->label_count--;
    }
    for (int i = 0; i < out->label_count; i++) {
        out->labels[i].show_year = i == 0 || out->labels[i].month == 1;
    }
}

void hm_view_build(const hm_view_t *v, const hm_store_t *s, hm_page_t *out)
{
    memset(out, 0, sizeof(*out));
    memset(out->level, HM_LEVEL_NONE, sizeof(out->level));
    out->has_data = s->have_last;
    if (!out->has_data) return;
    const int32_t top_week = v->bottom_week - (HM_ROWS - 1);
    for (int r = 0; r < HM_ROWS; r++) {
        const int32_t sunday = hm_week_start(top_week + r);
        for (int c = 0; c < HM_COLS; c++) out->level[r][c] = hm_store_level(s, sunday + c);
    }
    out->older = v->bottom_week > oldest_bottom(s);
    out->newer = v->bottom_week < newest_bottom(s);
    add_labels(out, top_week);
}

void hm_view_wanted_days(const hm_view_t *v, int32_t *from_day, int32_t *to_day)
{
    *from_day = hm_week_start(v->bottom_week - (2 * HM_ROWS - 1));
    *to_day = hm_week_start(v->bottom_week) + HM_COLS - 1;
}
