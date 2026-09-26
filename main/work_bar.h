// main/work_bar.h —— How much of today's work time is left, as a bar.
//
// The bar is full until the work day starts, drains while it runs and is
// empty after it ends; it fills again at local midnight. Its blocks follow the
// remaining time in 5-minute steps (rounded up, reaching zero exactly when the
// day ends), so it changes only on the clock's 5-minute marks. The text under
// it is whole hours rounded to the nearest hour: 4 h 35 min shows 5 h, 4 h 25
// min shows 4 h, and exactly half an hour rounds up; it changes on the half
// hours. Tone: more than half of the day left is green, half or less yellow, a
// fifth or less red. Until the clock has been set (it reads a year before
// 2025) there is no bar.
//
// Pure C (host-tested): times are UTC seconds plus a fixed UTC offset.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WORK_BAR_STEP_MIN    5
#define WORK_BAR_VALID_FROM  1735689600    // 2025-01-01 00:00:00 UTC

typedef enum {
    WORK_BAR_OFF = 0,      // clock not set, or an invalid work window
    WORK_BAR_GREEN,
    WORK_BAR_YELLOW,
    WORK_BAR_RED,
} work_bar_tone_t;

typedef struct {
    work_bar_tone_t tone;
    uint16_t left_min;     // remaining work minutes, a multiple of the step
    uint16_t total_min;    // length of the work day
    uint8_t left_hours;    // remaining time rounded to the nearest hour
} work_bar_t;

// start_min < end_min <= 24 * 60, minutes after local midnight.
void work_bar_compute(int64_t utc_s, int32_t utc_offset_min, uint16_t start_min, uint16_t end_min,
                      work_bar_t *out);

bool work_bar_same(const work_bar_t *a, const work_bar_t *b);

// "5h", "0h"; "" when the bar is off. Returns the text length.
size_t work_bar_text(const work_bar_t *bar, char *out, size_t cap);

// Minutes of the n-th hour block counted from the bottom (0..60), for drawing.
uint8_t work_bar_block_minutes(const work_bar_t *bar, int block_from_bottom);
