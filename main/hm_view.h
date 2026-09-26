// main/hm_view.h —— Which weeks the main page shows, and what each cell is.
//
// A page is HM_ROWS weeks (rows, oldest at the top) by seven days (columns,
// Sunday to Saturday). UP pages to older weeks and DOWN to newer ones, a full
// page at a time; paging stops at the first and the current week. While the
// view follows today, new data keeps the current week in the bottom row.
//
// Month labels follow GitHub's rule: a month is named at the row of its first
// Sunday (for a row that starts before the history, at its first shown day).
// A leading partial-month label is dropped when the next label follows within
// two rows; the first label and every January also carry the year.
//
// Pure C; host-tested.
#pragma once

#include "hm_store.h"

#include <stdbool.h>
#include <stdint.h>

#define HM_ROWS        13      // weeks per page: one quarter
#define HM_COLS        7
#define HM_MAX_LABELS  5

typedef struct {
    int32_t bottom_week;    // week index of the bottom row
    bool follow_today;
} hm_view_t;

typedef struct {
    uint8_t row;
    uint8_t month;          // 1..12
    int16_t year;
    bool show_year;
} hm_label_t;

typedef struct {
    bool has_data;
    bool older;             // UP would move
    bool newer;             // DOWN would move
    uint8_t level[HM_ROWS][HM_COLS];   // 0..4, HM_LEVEL_UNKNOWN or HM_LEVEL_NONE
    uint8_t label_count;
    hm_label_t labels[HM_MAX_LABELS];
} hm_page_t;

void hm_view_init(hm_view_t *v);

// Re-anchor after the store changed (new day, new history limit).
void hm_view_sync(hm_view_t *v, const hm_store_t *s);

// direction < 0: older (UP); > 0: newer (DOWN). Returns true when the page moved.
bool hm_view_scroll(hm_view_t *v, const hm_store_t *s, int direction);

// Jump back to the page that ends with the current week.
void hm_view_to_today(hm_view_t *v, const hm_store_t *s);

void hm_view_build(const hm_view_t *v, const hm_store_t *s, hm_page_t *out);

// Days worth downloading now: the page plus one page of older weeks.
void hm_view_wanted_days(const hm_view_t *v, int32_t *from_day, int32_t *to_day);
