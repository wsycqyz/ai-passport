// main/work_bar.c —— see work_bar.h.
#include "work_bar.h"

#include <stdio.h>

#define DAY_S  86400

void work_bar_compute(int64_t utc_s, int32_t utc_offset_min, uint16_t start_min, uint16_t end_min,
                      work_bar_t *out)
{
    out->tone = WORK_BAR_OFF;
    out->left_min = 0;
    out->total_min = 0;
    out->left_hours = 0;
    if (utc_s < WORK_BAR_VALID_FROM || start_min >= end_min || end_min > 24 * 60) return;

    int64_t second = (utc_s + (int64_t)utc_offset_min * 60) % DAY_S;
    if (second < 0) second += DAY_S;
    const int64_t start = (int64_t)start_min * 60;
    const int64_t end = (int64_t)end_min * 60;
    int64_t left_s = end - start;                  // before the day starts: all of it
    if (second >= end) {
        left_s = 0;
    } else if (second > start) {
        left_s = end - second;
    }
    // Nearest hour from the exact time left: 30 minutes and more round up.
    out->left_hours = (uint8_t)((left_s + 1800) / 3600);
    const int64_t step_s = WORK_BAR_STEP_MIN * 60;
    int64_t left = (left_s + step_s - 1) / step_s * WORK_BAR_STEP_MIN;
    out->total_min = (uint16_t)(end_min - start_min);
    if (left > out->total_min) left = out->total_min;
    out->left_min = (uint16_t)left;
    if (out->left_min * 2u > out->total_min) {
        out->tone = WORK_BAR_GREEN;
    } else if (out->left_min * 5u > out->total_min) {
        out->tone = WORK_BAR_YELLOW;
    } else {
        out->tone = WORK_BAR_RED;
    }
}

bool work_bar_same(const work_bar_t *a, const work_bar_t *b)
{
    return a->tone == b->tone && a->left_min == b->left_min && a->total_min == b->total_min &&
           a->left_hours == b->left_hours;
}

size_t work_bar_text(const work_bar_t *bar, char *out, size_t cap)
{
    if (!cap) return 0;
    const int n = bar->tone == WORK_BAR_OFF ? snprintf(out, cap, "%s", "")
                                            : snprintf(out, cap, "%uh", (unsigned)bar->left_hours);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

uint8_t work_bar_block_minutes(const work_bar_t *bar, int block_from_bottom)
{
    if (bar->tone == WORK_BAR_OFF || block_from_bottom < 0) return 0;
    const int minutes = (int)bar->left_min - block_from_bottom * 60;
    if (minutes <= 0) return 0;
    return (uint8_t)(minutes > 60 ? 60 : minutes);
}
