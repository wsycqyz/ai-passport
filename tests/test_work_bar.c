// Host tests for the work-time bar (main/work_bar.c). The work day is
// 09:00-21:00 at UTC+8; 1790384400 is 2026-09-26 09:00:00 local
// (01:00:00 UTC), computed independently.
#include "work_bar.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        s_failures++; \
    } \
} while (0)

#define UTC8   (8 * 60)
#define START  (9 * 60)
#define END    (21 * 60)
#define NINE   1790384400LL                      // 09:00:00 local
#define AT(h, m, s) (NINE + ((h) - 9) * 3600LL + (m) * 60LL + (s))

static work_bar_t at(int64_t utc)
{
    work_bar_t bar;
    work_bar_compute(utc, UTC8, START, END, &bar);
    return bar;
}

static bool is(int64_t utc, work_bar_tone_t tone, unsigned left_min, const char *text)
{
    const work_bar_t bar = at(utc);
    char buf[16];
    work_bar_text(&bar, buf, sizeof(buf));
    const bool ok = bar.tone == tone && bar.left_min == left_min && strcmp(buf, text) == 0 &&
                    (tone == WORK_BAR_OFF ? bar.total_min == 0 : bar.total_min == END - START);
    if (!ok) {
        fprintf(stderr, "utc %lld: tone %d left %u min \"%s\"\n", (long long)utc, bar.tone, bar.left_min, buf);
    }
    return ok;
}

static void test_clock_not_set(void)
{
    CHECK(is(0, WORK_BAR_OFF, 0, ""));                         // just after a cold start
    CHECK(is(12345, WORK_BAR_OFF, 0, ""));
    CHECK(is(WORK_BAR_VALID_FROM - 1, WORK_BAR_OFF, 0, ""));   // 2024-12-31 23:59:59 UTC
    CHECK(at(WORK_BAR_VALID_FROM).tone != WORK_BAR_OFF);       // 2025-01-01 counts as set
}

static void test_work_day_at_utc_plus_8(void)
{
    CHECK(is(AT(8, 59, 59), WORK_BAR_GREEN, 720, "12h"));    // full before the day
    CHECK(is(AT(9, 0, 0), WORK_BAR_GREEN, 720, "12h"));
    CHECK(is(AT(9, 0, 1), WORK_BAR_GREEN, 720, "12h"));      // blocks: rounded up to 5 minutes
    CHECK(is(AT(9, 5, 0), WORK_BAR_GREEN, 715, "12h"));      // 11 h 55 min
    CHECK(is(AT(9, 30, 0), WORK_BAR_GREEN, 690, "12h"));     // 11 h 30 min: half an hour rounds up
    CHECK(is(AT(9, 30, 1), WORK_BAR_GREEN, 690, "11h"));     // 11 h 29 min 59 s
    CHECK(is(AT(16, 25, 0), WORK_BAR_YELLOW, 275, "5h"));    // 4 h 35 min left: 5 h
    CHECK(is(AT(16, 35, 0), WORK_BAR_YELLOW, 265, "4h"));    // 4 h 25 min left: 4 h
    CHECK(is(AT(14, 55, 0), WORK_BAR_GREEN, 365, "6h"));     // more than half left
    CHECK(is(AT(15, 0, 0), WORK_BAR_YELLOW, 360, "6h"));     // half left
    CHECK(is(AT(18, 35, 0), WORK_BAR_YELLOW, 145, "2h"));    // more than a fifth
    CHECK(is(AT(18, 40, 0), WORK_BAR_RED, 140, "2h"));       // a fifth (2 h 24 min) or less
    CHECK(is(AT(20, 30, 0), WORK_BAR_RED, 30, "1h"));
    CHECK(is(AT(20, 30, 1), WORK_BAR_RED, 30, "0h"));        // the last half hour reads 0 h
    CHECK(is(AT(20, 59, 59), WORK_BAR_RED, 5, "0h"));        // one block sliver until the end
    CHECK(is(AT(21, 0, 0), WORK_BAR_RED, 0, "0h"));          // empty
    CHECK(is(AT(23, 59, 59), WORK_BAR_RED, 0, "0h"));
    CHECK(is(AT(24, 0, 0), WORK_BAR_GREEN, 720, "12h"));     // local midnight: full again
}

static void test_changes_follow_the_clock(void)
{
    // Over the work day the blocks change on every 5-minute mark and the text
    // on every half hour, both only downwards.
    work_bar_t prev = at(AT(9, 0, 0));
    int block_steps = 0;
    int text_steps = 0;
    for (int64_t t = AT(9, 0, 1); t <= AT(21, 0, 0); t++) {
        const work_bar_t now = at(t);
        if (now.left_min != prev.left_min) {
            block_steps++;
            CHECK(now.left_min < prev.left_min);
            CHECK((t - NINE) % 300 == 0);
        }
        if (now.left_hours != prev.left_hours) {
            text_steps++;
            CHECK(now.left_hours + 1 == prev.left_hours);
            CHECK((t - NINE) % 3600 == 1801);   // hh:30:01
        }
        prev = now;
    }
    CHECK(block_steps == 144);   // 12 hours of 5-minute steps
    CHECK(text_steps == 12);     // 12h -> 11h at 09:30:01 ... 1h -> 0h at 20:30:01
}

static void test_other_windows_and_offsets(void)
{
    work_bar_t bar;
    char text[16];
    // A 12-hour day 10:00-22:00 at UTC-5 on 2026-09-26 21:00 UTC (16:00 local).
    work_bar_compute(NINE + 20 * 3600, -5 * 60, 10 * 60, 22 * 60, &bar);
    work_bar_text(&bar, text, sizeof(text));
    CHECK(bar.tone == WORK_BAR_YELLOW && bar.left_min == 360 && bar.total_min == 720 &&
          strcmp(text, "6h") == 0);
    work_bar_compute(NINE - 3600, -5 * 60, 10 * 60, 22 * 60, &bar);   // 19:00 local, a quarter left
    work_bar_text(&bar, text, sizeof(text));
    CHECK(bar.tone == WORK_BAR_YELLOW && bar.left_min == 180 && strcmp(text, "3h") == 0);
    work_bar_compute(NINE + 2 * 3600, -5 * 60, 10 * 60, 22 * 60, &bar);   // 22:00 local
    CHECK(bar.tone == WORK_BAR_RED && bar.left_min == 0 && bar.left_hours == 0);
    work_bar_compute(NINE - 22 * 3600, UTC8, 0, 24 * 60, &bar);        // whole day, 11:00 local
    work_bar_text(&bar, text, sizeof(text));
    CHECK(bar.tone == WORK_BAR_GREEN && bar.left_min == 780 && strcmp(text, "13h") == 0);
    work_bar_compute(NINE, UTC8, 9 * 60, 18 * 60, &bar);               // a 9-hour day at its start
    work_bar_text(&bar, text, sizeof(text));
    CHECK(bar.tone == WORK_BAR_GREEN && bar.left_min == 540 && strcmp(text, "9h") == 0);
    // Invalid windows show no bar.
    work_bar_compute(NINE, UTC8, 18 * 60, 9 * 60, &bar);
    CHECK(bar.tone == WORK_BAR_OFF && bar.left_hours == 0);
    work_bar_compute(NINE, UTC8, 9 * 60, 9 * 60, &bar);
    CHECK(bar.tone == WORK_BAR_OFF);
    work_bar_compute(NINE, UTC8, 9 * 60, 25 * 60, &bar);
    CHECK(bar.tone == WORK_BAR_OFF);
    // Text fits small buffers.
    bar = at(NINE);
    CHECK(work_bar_text(&bar, text, 3) == 2 && strcmp(text, "12") == 0);
    CHECK(work_bar_text(&bar, text, 0) == 0);
    // Equality covers every field.
    work_bar_t other = bar;
    CHECK(work_bar_same(&bar, &other));
    other.left_hours--;
    CHECK(!work_bar_same(&bar, &other));
}

static void test_blocks(void)
{
    // One block per hour, filled from the bottom: at 16:25 (4 h 35 min left)
    // the four bottom blocks are full and the fifth holds 35 minutes.
    const work_bar_t bar = at(AT(16, 25, 0));
    CHECK(work_bar_block_minutes(&bar, 0) == 60 && work_bar_block_minutes(&bar, 3) == 60);
    CHECK(work_bar_block_minutes(&bar, 4) == 35);
    CHECK(work_bar_block_minutes(&bar, 5) == 0 && work_bar_block_minutes(&bar, 11) == 0);
    CHECK(work_bar_block_minutes(&bar, -1) == 0);
    const work_bar_t full = at(NINE);
    CHECK(full.total_min / 60 == 12 && work_bar_block_minutes(&full, 11) == 60);
    const work_bar_t off = at(0);
    CHECK(work_bar_block_minutes(&off, 0) == 0);
}

int main(void)
{
    test_clock_not_set();
    test_work_day_at_utc_plus_8();
    test_changes_follow_the_clock();
    test_other_windows_and_offsets();
    test_blocks();
    if (s_failures) {
        fprintf(stderr, "test_work_bar: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_work_bar: PASS\n");
    return 0;
}
