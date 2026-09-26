// Host tests for the contribution calendar pipeline: date arithmetic, the
// streaming parser and its day window, the store and the page model
// (main/hm_calendar.c, main/gh_parse.c, main/hm_store.c, main/hm_view.c).
// All fixtures are synthetic.
//
// Manual cross-check: `test_heatmap --dump FILE` prints "YYYY-MM-DD level count"
// for every day found in a saved API (JSON) or GitHub (HTML) response.
#include "gh_parse.h"
#include "hm_calendar.h"
#include "hm_store.h"
#include "hm_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        s_failures++; \
    } \
} while (0)

#define FAIL(what) do { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, what); \
    s_failures++; \
} while (0)

static int32_t D(int y, int m, int d)
{
    return hm_days_from_civil(y, m, d);
}

static uint32_t s_rng = 20260925u;

static uint32_t rnd(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return s_rng >> 8;
}

// Deterministic synthetic contributions.
static uint8_t level_for(int32_t day)
{
    return (uint8_t)((day * 7 + day / 3) % 5);
}

static int32_t count_for(int32_t day)
{
    const uint8_t level = level_for(day);
    return level ? level * 3 + day % 3 : 0;
}

static void iso(int32_t day, char out[11])
{
    int y, m, d;
    hm_civil_from_days(day, &y, &m, &d);
    snprintf(out, 11, "%04d-%02d-%02d", y, m, d);
}

// ---------------------------------------------------------------------------
// Growable text buffer for fixtures.
// ---------------------------------------------------------------------------

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} text_t;

static void append(text_t *t, const char *s)
{
    const size_t n = strlen(s);
    if (t->len + n + 1 > t->cap) {
        t->cap = (t->len + n + 1) * 2;
        t->data = realloc(t->data, t->cap);
        if (!t->data) abort();
    }
    memcpy(t->data + t->len, s, n + 1);
    t->len += n;
}

// github-contributions-api v4 shape, compact like the real service.
static void api_json(text_t *t, int32_t first, int32_t last, uint8_t (*level)(int32_t))
{
    append(t, "{\"total\":{\"lastYear\":123},\"contributions\":[");
    for (int32_t day = first; day <= last; day++) {
        char date[11], item[96];
        iso(day, date);
        snprintf(item, sizeof(item), "%s{\"date\":\"%s\",\"count\":%d,\"level\":%u}",
                 day == first ? "" : ",", date, (int)count_for(day), level(day));
        append(t, item);
    }
    append(t, "]}");
}

// GitHub's calendar table: one row per weekday, one column per week; every
// cell is followed by its tool-tip, and the legend repeats data-level.
static void github_html(text_t *t, int32_t first_sunday, int weeks, int32_t last)
{
    append(t, "<div class=\"js-calendar-graph\" data-graph-url=\"/users/x/contributions\" "
              "data-from=\"2025-09-21 00:00:00 UTC\" data-to=\"2026-09-25 23:59:59 UTC\">\n"
              "<table><thead><tr><td class=\"ContributionCalendar-label\" colspan=\"2\">"
              "<span aria-hidden=\"true\">Sep</span></td></tr></thead><tbody>\n");
    for (int row = 0; row < 7; row++) {
        append(t, "<tr style=\"height: 10px\">\n<td class=\"ContributionCalendar-label\">"
                  "<span class=\"sr-only\">Sunday</span></td>\n");
        for (int col = 0; col < weeks; col++) {
            const int32_t day = first_sunday + col * 7 + row;
            char cell[512], date[11];
            if (day > last) {
                append(t, "<td></td>\n");
                continue;
            }
            iso(day, date);
            snprintf(cell, sizeof(cell),
                     "<td tabindex=\"0\" data-ix=\"%d\" aria-selected=\"false\" "
                     "aria-describedby=\"contribution-graph-legend-level-%u\" style=\"width: 10px\" "
                     "data-date=\"%s\" id=\"contribution-day-component-%d-%d\" data-level=\"%u\" "
                     "role=\"gridcell\" data-view-component=\"true\" class=\"ContributionCalendar-day\"></td>\n"
                     "  <tool-tip for=\"contribution-day-component-%d-%d\" popover=\"manual\">"
                     "%d contributions on some day.</tool-tip>\n",
                     col, level_for(day), date, row, col, level_for(day), row, col, (int)count_for(day));
            append(t, cell);
        }
        append(t, "</tr>\n");
    }
    append(t, "</tbody></table></div>\n<span>Less</span>\n");
    for (int level = 0; level <= 4; level++) {
        char legend[160];
        snprintf(legend, sizeof(legend),
                 "<div style=\"width: 10px\" id=\"contribution-graph-legend-level-%d\" data-level=\"%d\" "
                 "class=\"ContributionCalendar-day\"></div>\n", level, level);
        append(t, legend);
    }
    append(t, "<span>More</span>\n");
}

typedef struct {
    int days;
    int32_t day[400];
    uint8_t level[400];
    int32_t count[400];
} record_t;

static void record_day(void *user, int32_t day, uint8_t level, int32_t count)
{
    record_t *r = user;
    if (r->days < 400) {
        r->day[r->days] = day;
        r->level[r->days] = level;
        r->count[r->days] = count;
    }
    r->days++;
}

static void feed_chunked(gh_parser_t *p, const char *data, size_t len, int max_chunk)
{
    size_t pos = 0;
    while (pos < len) {
        size_t n = max_chunk <= 0 ? len - pos : 1 + rnd() % (uint32_t)max_chunk;
        if (n > len - pos) n = len - pos;
        gh_parser_feed(p, data + pos, n);
        pos += n;
    }
}

static int parse_text(const char *text, record_t *r)
{
    gh_parser_t p;
    memset(r, 0, sizeof(*r));
    gh_parser_init(&p, record_day, r);
    gh_parser_feed(&p, text, strlen(text));
    return (int)p.rejected;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_calendar(void)
{
    CHECK(D(1970, 1, 1) == 0);
    CHECK(D(2000, 3, 1) == 11017);
    CHECK(D(2026, 9, 25) == 20721);
    CHECK(D(2025, 9, 21) == 20352);
    CHECK(D(2016, 1, 1) == 16801);
    CHECK(D(2199, 12, 31) == 84005);
    for (int32_t day = D(2007, 1, 1); day <= D(2032, 12, 31); day++) {
        int y, m, d;
        hm_civil_from_days(day, &y, &m, &d);
        if (D(y, m, d) != day || d < 1 || d > hm_days_in_month(y, m)) {
            FAIL("civil round trip");
            break;
        }
        const int32_t sunday = hm_week_start(hm_week_of(day));
        if (hm_weekday(sunday) != 0 || day - sunday != hm_weekday(day)) {
            FAIL("week arithmetic");
            break;
        }
    }
    CHECK(hm_weekday(0) == 4);                    // Thursday
    CHECK(hm_weekday(-1) == 3);                   // Wednesday 1969-12-31
    CHECK(hm_weekday(D(2026, 9, 25)) == 5);       // Friday
    CHECK(hm_weekday(D(2025, 9, 21)) == 0);       // Sunday
    CHECK(hm_week_start(0) == -4 && hm_week_of(-4) == 0 && hm_week_of(-5) == -1);
    CHECK(hm_year_of(D(2024, 12, 31)) == 2024 && hm_year_of(D(2025, 1, 1)) == 2025);
    CHECK(hm_days_in_month(2024, 2) == 29 && hm_days_in_month(2100, 2) == 28);
    CHECK(hm_days_in_month(2000, 2) == 29 && hm_days_in_month(2025, 13) == 0);

    int32_t day = -1;
    CHECK(hm_parse_iso_date("2024-02-29", &day) && day == D(2024, 2, 29));
    CHECK(!hm_parse_iso_date("2025-02-29", &day));
    CHECK(!hm_parse_iso_date("2025-13-01", &day));
    CHECK(!hm_parse_iso_date("2025-00-10", &day));
    CHECK(!hm_parse_iso_date("2025-1-01", &day));
    CHECK(!hm_parse_iso_date("2025-01-011", &day));
    CHECK(!hm_parse_iso_date("1969-12-31", &day));
    CHECK(!hm_parse_iso_date("2025/01/01", &day));
    CHECK(!hm_parse_iso_date("", &day) && !hm_parse_iso_date(NULL, &day));
    CHECK(strcmp(hm_month_abbr(1), "Jan") == 0 && strcmp(hm_month_abbr(12), "Dec") == 0);
    CHECK(strcmp(hm_month_abbr(0), "") == 0 && strcmp(hm_month_abbr(13), "") == 0);
}

static void check_recorded(const record_t *r, int32_t first, int32_t last, bool counts)
{
    CHECK(r->days == last - first + 1);
    for (int i = 0; i < r->days && i < 400; i++) {
        const int32_t day = first + i;
        if (r->day[i] != day || r->level[i] != level_for(day) ||
            r->count[i] != (counts ? count_for(day) : -1)) {
            FAIL("recorded day mismatch");
            return;
        }
    }
}

static void test_parse_api_json(void)
{
    const int32_t first = D(2025, 9, 21);
    const int32_t last = D(2026, 9, 25);
    text_t json = { 0 };
    api_json(&json, first, last, level_for);
    // Whole, byte by byte, and random chunks: pattern and value splits.
    const int chunks[] = { 0, 1, 2, 7, 23, 64 };
    for (size_t k = 0; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
        record_t r;
        memset(&r, 0, sizeof(r));
        gh_parser_t p;
        gh_parser_init(&p, record_day, &r);
        feed_chunked(&p, json.data, json.len, chunks[k]);
        CHECK(p.rejected == 0);
        CHECK(p.days == (uint32_t)(last - first + 1));
        check_recorded(&r, first, last, true);
    }
    free(json.data);
}

static void test_parse_github_html(void)
{
    const int32_t first = D(2025, 9, 21);
    const int32_t last = D(2026, 9, 25);
    text_t html = { 0 };
    github_html(&html, first, 54, last);
    for (int round = 0; round < 4; round++) {
        gh_block_t block;
        gh_block_init(&block, D(2008, 1, 1), D(2199, 12, 31));
        gh_parser_t p;
        gh_parser_init(&p, gh_block_on_day, &block);
        feed_chunked(&p, html.data, html.len, round == 0 ? 0 : 1 + round * 13);
        CHECK(p.rejected == 0);
        CHECK(p.days == (uint32_t)(last - first + 1));   // the legend is not a day
        CHECK(block.first_day == first && gh_block_last_day(&block) == last);
        CHECK(block.filled == last - first + 1 && block.rejected == 0);
        CHECK(!block.counts_known);
        bool same = true;
        for (int32_t day = first; day <= last; day++) same &= gh_block_level(&block, day) == level_for(day);
        CHECK(same);
    }
    free(html.data);
}

static void test_parse_edge_cases(void)
{
    record_t r;
    // Whitespace, quoted numbers, and a key split by an overlapping prefix.
    CHECK(parse_text("{ \"date\": \"2025-01-02\", \"count\": \"4\", \"level\": 1 }", &r) == 0);
    CHECK(r.days == 1 && r.day[0] == D(2025, 1, 2) && r.level[0] == 1 && r.count[0] == 4);
    CHECK(parse_text("<td data-data-date=\"2025-01-03\" x data-level=\"3\">", &r) == 0);
    CHECK(r.days == 1 && r.day[0] == D(2025, 1, 3) && r.level[0] == 3 && r.count[0] == -1);
    CHECK(parse_text("<td data-date='2025-01-04' data-level='2'>", &r) == 0);
    CHECK(r.days == 1 && r.level[0] == 2);
    // A legend level without a date is not a day.
    CHECK(parse_text("<div data-level=\"3\"></div><div data-level=\"4\"></div>", &r) == 0);
    CHECK(r.days == 0);
    // A date without a level is replaced by the next one.
    CHECK(parse_text("{\"date\":\"2025-01-05\"},{\"date\":\"2025-01-06\",\"level\":2}", &r) == 0);
    CHECK(r.days == 1 && r.day[0] == D(2025, 1, 6));
    // Invalid values drop their day; parsing continues afterwards.
    CHECK(parse_text("{\"date\":\"2025-02-30\",\"level\":1}{\"date\":\"2025-03-01\",\"level\":1}", &r) == 1);
    CHECK(r.days == 1 && r.day[0] == D(2025, 3, 1));
    CHECK(parse_text("{\"date\":\"2025-03-02\",\"level\":7}{\"date\":\"2025-03-03\",\"level\":null}", &r) == 2);
    CHECK(r.days == 0);
    CHECK(parse_text("{\"date\":\"2025-03-04\",\"level\":1234567890}", &r) == 1);
    CHECK(r.days == 0);
    CHECK(parse_text("{\"date\":\"25-03-04\",\"level\":1}", &r) == 1);
    CHECK(r.days == 0);
    // A broken count keeps the day but not its count.
    CHECK(parse_text("{\"date\":\"2025-03-05\",\"count\":x,\"level\":1}", &r) == 1);
    CHECK(r.days == 1 && r.count[0] == -1);
    // Too many separators between a key and its value.
    CHECK(parse_text("{\"date\":     \"2025-03-06\",\"level\":1}", &r) == 1);
    CHECK(r.days == 0);
    // An error response has no days.
    CHECK(parse_text("{\"error\":\"GitHub user \\\"x\\\" not found.\"}", &r) == 0);
    CHECK(r.days == 0);
    CHECK(parse_text("", &r) == 0 && r.days == 0);
}

static void test_block_window(void)
{
    gh_block_t b;
    const int32_t jan1 = D(2025, 1, 1);
    gh_block_init(&b, jan1, D(2025, 12, 31));
    CHECK(gh_block_last_day(&b) == -1 && gh_block_level(&b, jan1) == GH_LEVEL_UNKNOWN);
    CHECK(!gh_block_is_empty_year(&b));
    // Weekday-row order, as GitHub's HTML delivers it.
    for (int weekday = 0; weekday < 7; weekday++) {
        for (int32_t day = hm_week_start(hm_week_of(jan1)) + weekday; day <= D(2025, 12, 31); day += 7) {
            if (day < jan1) {
                CHECK(!gh_block_put(&b, day, 1, 1));
                continue;
            }
            CHECK(gh_block_put(&b, day, 0, 0));
        }
    }
    CHECK(b.first_day == jan1 && b.span == 365 && b.filled == 365 && b.counts_known && b.total == 0);
    CHECK(gh_block_is_empty_year(&b));
    CHECK(b.rejected == 3);   // Dec 29-31, 2024 fall outside the year
    CHECK(gh_block_put(&b, D(2025, 6, 1), 2, 5) && b.filled == 365 && b.total == 0);   // a replacement
    CHECK(!gh_block_is_empty_year(&b));
    CHECK(!gh_block_put(&b, D(2025, 6, 1), 5, 1));   // level out of range

    // Window capacity and gaps.
    gh_block_init(&b, D(2008, 1, 1), D(2199, 12, 31));
    const int32_t base = D(2025, 9, 21);
    CHECK(gh_block_put(&b, base + 10, 1, 2));
    CHECK(gh_block_put(&b, base, 3, 4));
    CHECK(b.first_day == base && b.span == 11 && b.filled == 2 && b.total == 6);
    CHECK(gh_block_level(&b, base + 5) == GH_LEVEL_UNKNOWN);
    CHECK(gh_block_put(&b, base + GH_BLOCK_DAYS - 1, 1, -1) && !b.counts_known);
    CHECK(!gh_block_put(&b, base + GH_BLOCK_DAYS, 1, 1));
    CHECK(!gh_block_put(&b, base - 1, 1, 1));
    CHECK(gh_block_level(&b, base + GH_BLOCK_DAYS) == GH_LEVEL_UNKNOWN);
    CHECK(gh_block_level(&b, base - 1) == GH_LEVEL_UNKNOWN);
}

// A rolling year ending on `today`, starting on the Sunday 52 weeks earlier.
static void make_last(gh_block_t *b, int32_t today, uint8_t (*level)(int32_t))
{
    gh_block_init(b, D(2008, 1, 1), D(2199, 12, 31));
    for (int32_t day = hm_week_start(hm_week_of(today) - 52); day <= today; day++) {
        gh_block_put(b, day, level(day), count_for(day));
    }
}

static void make_year(gh_block_t *b, int year, uint8_t (*level)(int32_t))
{
    gh_block_init(b, D(year, 1, 1), D(year, 12, 31));
    for (int32_t day = D(year, 1, 1); day <= D(year, 12, 31); day++) gh_block_put(b, day, level(day), 0);
}

static uint8_t level_zero(int32_t day)
{
    (void)day;
    return 0;
}

static uint8_t level_three(int32_t day)
{
    (void)day;
    return 3;
}

static void test_store(void)
{
    static hm_store_t s;
    static gh_block_t b;
    const int32_t today = D(2026, 9, 25);
    hm_store_init(&s);
    CHECK(hm_store_level(&s, today) == HM_LEVEL_NONE);
    CHECK(hm_store_missing_year(&s, D(2020, 1, 1), today) == 0);
    make_year(&b, 2025, level_three);
    CHECK(!hm_store_set_year(&s, 2025, &b));   // needs "today" first

    gh_block_init(&b, D(2008, 1, 1), D(2199, 12, 31));
    for (int32_t day = today - 100; day <= today; day++) gh_block_put(&b, day, 1, 1);
    CHECK(!hm_store_last_plausible(&b) && !hm_store_set_last(&s, &b));

    make_last(&b, today, level_for);
    CHECK(b.first_day == D(2025, 9, 21) && b.filled == 370);
    CHECK(hm_store_set_last(&s, &b));
    CHECK(s.have_last && s.today == today);
    CHECK(hm_store_first_day(&s) == D(2016, 1, 1));
    CHECK(hm_store_level(&s, today) == level_for(today));
    CHECK(hm_store_level(&s, today + 1) == HM_LEVEL_NONE);
    CHECK(hm_store_level(&s, D(2025, 9, 21)) == level_for(D(2025, 9, 21)));
    CHECK(hm_store_level(&s, D(2025, 9, 20)) == HM_LEVEL_UNKNOWN);
    CHECK(hm_store_level(&s, D(2015, 12, 31)) == HM_LEVEL_NONE);
    CHECK(hm_store_missing_year(&s, D(2025, 9, 21), today) == 0);
    CHECK(hm_store_missing_year(&s, D(2025, 6, 1), today) == 2025);
    CHECK(hm_store_missing_year(&s, D(2023, 6, 1), D(2025, 9, 1)) == 2025);   // newest first

    // A calendar year fills the days before the rolling year; the rolling
    // year keeps precedence where both exist.
    make_year(&b, 2025, level_three);
    CHECK(hm_store_set_year(&s, 2025, &b));
    CHECK(hm_store_level(&s, D(2025, 9, 20)) == 3);
    CHECK(hm_store_level(&s, D(2025, 9, 22)) == level_for(D(2025, 9, 22)));
    CHECK(hm_store_missing_year(&s, D(2023, 6, 1), today) == 2024);

    // An implausible or foreign block is refused.
    gh_block_init(&b, D(2008, 1, 1), D(2199, 12, 31));
    for (int32_t day = D(2023, 12, 1); day <= D(2024, 11, 30); day++) gh_block_put(&b, day, 1, 1);
    CHECK(!hm_store_year_plausible(2024, &b) && !hm_store_set_year(&s, 2024, &b));
    make_year(&b, 2015, level_three);
    CHECK(!hm_store_set_year(&s, 2015, &b));   // older than HM_MAX_YEARS allows

    // The first year without contributions ends the history after it.
    make_year(&b, 2024, level_zero);
    CHECK(hm_store_set_year(&s, 2024, &b));
    CHECK(s.floor_year == 2025 && hm_store_first_day(&s) == D(2025, 1, 1));
    CHECK(hm_store_level(&s, D(2024, 12, 31)) == HM_LEVEL_NONE);
    CHECK(hm_store_level(&s, D(2025, 1, 1)) == 3);
    CHECK(hm_store_missing_year(&s, D(2020, 1, 1), today) == 0);
    make_year(&b, 2023, level_three);
    CHECK(!hm_store_set_year(&s, 2023, &b));

    // A refreshed rolling year moves "today" but keeps the older years.
    make_last(&b, today + 3, level_for);
    CHECK(hm_store_set_last(&s, &b) && s.today == today + 3);
    CHECK(hm_store_level(&s, D(2025, 3, 1)) == 3);
    CHECK(hm_store_oldest_year(&s) == 2025);
    CHECK(hm_store_year_block(&s, 2025) && hm_store_year_block(&s, 2025)->first_day == D(2025, 1, 1));
    CHECK(hm_store_year_block(&s, 2022) == NULL && hm_store_year_block(&s, 0) == NULL);
}

static void test_view_pages(void)
{
    static hm_store_t s;
    static gh_block_t b;
    hm_view_t v;
    hm_page_t page;
    const int32_t today = D(2026, 9, 25);   // Friday

    hm_store_init(&s);
    hm_view_init(&v);
    hm_view_sync(&v, &s);
    hm_view_build(&v, &s, &page);
    CHECK(!page.has_data && page.label_count == 0);
    CHECK(!hm_view_scroll(&v, &s, -1));

    make_last(&b, today, level_for);
    hm_store_set_last(&s, &b);
    hm_view_sync(&v, &s);
    CHECK(v.bottom_week == hm_week_of(today) && v.follow_today);
    hm_view_build(&v, &s, &page);
    CHECK(page.has_data && page.older && !page.newer);
    CHECK(page.level[HM_ROWS - 1][5] == level_for(today));
    CHECK(page.level[HM_ROWS - 1][6] == HM_LEVEL_NONE);   // tomorrow
    CHECK(page.level[0][0] == level_for(D(2026, 6, 28)));
    // Rows start Jun 28, Jul 5, ...: the lone June row loses its label.
    CHECK(page.label_count == 3);
    CHECK(page.labels[0].row == 1 && page.labels[0].month == 7 && page.labels[0].show_year &&
          page.labels[0].year == 2026);
    CHECK(page.labels[1].row == 5 && page.labels[1].month == 8 && !page.labels[1].show_year);
    CHECK(page.labels[2].row == 10 && page.labels[2].month == 9);
    int32_t from, to;
    hm_view_wanted_days(&v, &from, &to);
    CHECK(from == D(2026, 3, 29) && to == D(2026, 9, 26));

    // UP pages 13 weeks back, DOWN returns and then stops.
    CHECK(hm_view_scroll(&v, &s, -1) && !v.follow_today);
    CHECK(v.bottom_week == hm_week_of(today) - HM_ROWS);
    hm_view_build(&v, &s, &page);
    CHECK(page.older && page.newer && page.level[HM_ROWS - 1][6] == level_for(D(2026, 6, 27)));
    CHECK(hm_view_scroll(&v, &s, 1) && v.follow_today);
    CHECK(!hm_view_scroll(&v, &s, 1));

    // Four pages back reaches September 2025, the rolling year's first week:
    // the earlier days are not downloaded yet, and 2025 is requested.
    for (int i = 0; i < 4; i++) CHECK(hm_view_scroll(&v, &s, -1));
    hm_view_build(&v, &s, &page);
    CHECK(page.level[0][0] == HM_LEVEL_UNKNOWN);
    hm_view_wanted_days(&v, &from, &to);
    CHECK(hm_store_missing_year(&s, from, to) == 2025);
    // Rows from Nov 23, 2025 to Feb 15, 2026: the first label and January
    // carry the year.
    v.bottom_week = hm_week_of(D(2026, 2, 15));
    v.follow_today = false;
    hm_view_build(&v, &s, &page);
    CHECK(page.label_count == 4);
    CHECK(page.labels[0].row == 0 && page.labels[0].month == 11 && page.labels[0].show_year &&
          page.labels[0].year == 2025);
    CHECK(page.labels[1].row == 2 && page.labels[1].month == 12 && !page.labels[1].show_year);
    CHECK(page.labels[2].row == 6 && page.labels[2].month == 1 && page.labels[2].show_year &&
          page.labels[2].year == 2026);
    CHECK(page.labels[3].row == 10 && page.labels[3].month == 2 && !page.labels[3].show_year);

    // New data keeps a page that is not following today where it is.
    const int32_t kept = v.bottom_week;
    make_last(&b, today + 7, level_for);
    hm_store_set_last(&s, &b);
    hm_view_sync(&v, &s);
    CHECK(v.bottom_week == kept);
    hm_view_to_today(&v, &s);
    CHECK(v.bottom_week == hm_week_of(today + 7) && v.follow_today);

    // Scrolling stops at the history limit (no empty year found: 2016).
    int moves = 0;
    while (hm_view_scroll(&v, &s, -1)) moves++;
    CHECK(moves > 40 && v.bottom_week == hm_week_of(D(2016, 1, 1)) + HM_ROWS - 1);
    hm_view_build(&v, &s, &page);
    CHECK(!page.older && page.newer);
    CHECK(page.level[0][4] == HM_LEVEL_NONE && page.level[0][5] == HM_LEVEL_UNKNOWN);   // Fri Jan 1, 2016

    // An empty year pulls the limit forward; the page is clamped to it and its
    // first row, which starts in December, is labelled with January.
    make_year(&b, 2025, level_three);
    CHECK(hm_store_set_year(&s, 2025, &b));
    make_year(&b, 2024, level_zero);
    CHECK(hm_store_set_year(&s, 2024, &b));
    hm_view_sync(&v, &s);
    CHECK(v.bottom_week == hm_week_of(D(2025, 1, 1)) + HM_ROWS - 1);
    hm_view_build(&v, &s, &page);
    CHECK(page.level[0][2] == HM_LEVEL_NONE && page.level[0][3] == 3);   // Wed Jan 1, 2025
    CHECK(page.label_count >= 1 && page.labels[0].row == 0 && page.labels[0].month == 1 &&
          page.labels[0].year == 2025 && page.labels[0].show_year);
    CHECK(!hm_view_scroll(&v, &s, -1));
}

static void test_view_short_history(void)
{
    static hm_store_t s;
    static gh_block_t b;
    hm_view_t v;
    hm_page_t page;
    const int32_t today = D(2026, 2, 10);
    hm_store_init(&s);
    make_last(&b, today, level_for);
    hm_store_set_last(&s, &b);
    make_year(&b, 2025, level_zero);
    CHECK(hm_store_set_year(&s, 2025, &b));   // history starts on Jan 1, 2026
    hm_view_init(&v);
    hm_view_sync(&v, &s);
    CHECK(v.bottom_week == hm_week_of(today));
    CHECK(!hm_view_scroll(&v, &s, -1));
    hm_view_build(&v, &s, &page);
    CHECK(!page.older && !page.newer);
    // The rows before January are blank and unlabelled.
    CHECK(page.level[0][0] == HM_LEVEL_NONE);
    CHECK(page.label_count >= 1 && page.labels[0].month == 1 && page.labels[0].show_year);
    for (int r = 0; r < page.labels[0].row; r++) {
        for (int c = 0; c < HM_COLS; c++) CHECK(page.level[r][c] == HM_LEVEL_NONE);
    }
}

static void test_block_blob(void)
{
    static gh_block_t b, back;
    uint8_t blob[HM_BLOB_MAX];
    const int32_t today = D(2026, 9, 25);
    make_last(&b, today, level_for);
    b.levels[5] = GH_LEVEL_UNKNOWN;   // a gap survives the round trip
    b.filled--;
    const size_t len = hm_block_encode(&b, blob, sizeof(blob));
    CHECK(len == HM_BLOB_HEADER + b.span && len <= HM_BLOB_MAX);
    CHECK(hm_block_decode(blob, len, D(2008, 1, 1), D(2199, 12, 31), &back));
    CHECK(hm_block_same(&b, &back) && back.filled == b.filled && back.counts_known == b.counts_known);
    CHECK(gh_block_last_day(&back) == today && hm_store_last_plausible(&back));

    // Too small an output buffer, and an empty block, encode to nothing.
    CHECK(hm_block_encode(&b, blob, len - 1) == 0);
    gh_block_init(&back, 0, 1);
    CHECK(hm_block_encode(&back, blob, sizeof(blob)) == 0);
    CHECK(hm_block_encode(&b, blob, sizeof(blob)) == len);

    // Every malformed input is rejected.
    uint8_t bad[HM_BLOB_MAX];
    CHECK(!hm_block_decode(blob, len - 1, D(2008, 1, 1), D(2199, 12, 31), &back));   // truncated
    CHECK(!hm_block_decode(blob, HM_BLOB_HEADER - 1, D(2008, 1, 1), D(2199, 12, 31), &back));
    memcpy(bad, blob, len);
    bad[0] = 2;                                                                        // unknown format
    CHECK(!hm_block_decode(bad, len, D(2008, 1, 1), D(2199, 12, 31), &back));
    memcpy(bad, blob, len);
    bad[1] = 0x80;                                                                     // unknown flag
    CHECK(!hm_block_decode(bad, len, D(2008, 1, 1), D(2199, 12, 31), &back));
    memcpy(bad, blob, len);
    bad[HM_BLOB_HEADER + 7] = 9;                                                       // impossible level
    CHECK(!hm_block_decode(bad, len, D(2008, 1, 1), D(2199, 12, 31), &back));
    memcpy(bad, blob, len);
    bad[6] = 0;                                                                        // span 0 / mismatch
    bad[7] = 0;
    CHECK(!hm_block_decode(bad, len, D(2008, 1, 1), D(2199, 12, 31), &back));
    CHECK(!hm_block_decode(blob, len, D(2026, 1, 1), D(2199, 12, 31), &back));        // starts too early
    CHECK(!hm_block_decode(blob, len, D(2008, 1, 1), today - 1, &back));              // ends too late

    // A different level, day or total is a change worth saving.
    CHECK(hm_block_decode(blob, len, D(2008, 1, 1), D(2199, 12, 31), &back));
    back.levels[10] = (uint8_t)((back.levels[10] + 1) % 5);
    CHECK(!hm_block_same(&b, &back));
    make_last(&back, today + 1, level_for);
    CHECK(!hm_block_same(&b, &back));
}

// Scroll from today to the history limit, downloading every year the view
// asks for, as the controller does. Downloads must settle: each year at most
// once and never more than the store can hold.
static bool history_settles(int32_t today)
{
    static hm_store_t s;
    static gh_block_t b;
    hm_view_t v;
    hm_store_init(&s);
    make_last(&b, today, level_three);
    if (!hm_store_set_last(&s, &b)) return false;
    hm_view_init(&v);
    hm_view_sync(&v, &s);
    int fetched[HM_MAX_YEARS + 1];
    int count = 0;
    for (int page = 0; page < 200; page++) {
        for (int guard = 0;; guard++) {
            int32_t from, to;
            hm_view_wanted_days(&v, &from, &to);
            const int year = hm_store_missing_year(&s, from, to);
            if (!year) break;
            for (int i = 0; i < count; i++) {
                if (fetched[i] == year) return false;   // asked for the same year twice
            }
            if (guard > HM_MAX_YEARS || count == HM_MAX_YEARS) return false;
            fetched[count++] = year;
            make_year(&b, year, level_three);
            if (!hm_store_set_year(&s, year, &b)) return false;
        }
        if (!hm_view_scroll(&v, &s, -1)) return true;
    }
    return false;
}

static void test_history_downloads_settle(void)
{
    // Dec 31 of a leap year on a Sunday: the rolling year starts on Jan 2.
    CHECK(history_settles(D(2028, 12, 31)));
    CHECK(history_settles(D(2056, 12, 31)));
    int failures = 0;
    for (int32_t day = D(2024, 12, 20); day <= D(2029, 1, 10) && failures < 3; day += 1) {
        if (!history_settles(day)) {
            char date[11];
            iso(day, date);
            fprintf(stderr, "history does not settle for today = %s\n", date);
            failures++;
        }
        if (day == D(2025, 1, 10)) day = D(2028, 12, 20);   // around both year ends only
    }
    CHECK(failures == 0);
    for (int32_t day = D(2016, 1, 1); day <= D(2060, 12, 31); day += 97) CHECK(history_settles(day));
}

static int dump(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 2;
    }
    static gh_block_t block;
    gh_block_init(&block, D(2008, 1, 1), D(2199, 12, 31));
    gh_parser_t p;
    gh_parser_init(&p, gh_block_on_day, &block);
    char buf[700];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) gh_parser_feed(&p, buf, n);
    fclose(f);
    for (int32_t day = block.first_day; day <= gh_block_last_day(&block); day++) {
        char date[11];
        iso(day, date);
        printf("%s %u\n", date, gh_block_level(&block, day));
    }
    fprintf(stderr, "days=%u rejected=%u filled=%u span=%u total=%u counts_known=%d last_plausible=%d\n",
            p.days, p.rejected, block.filled, block.span, block.total, block.counts_known,
            hm_store_last_plausible(&block));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--dump") == 0) return dump(argv[2]);
    test_calendar();
    test_parse_api_json();
    test_parse_github_html();
    test_parse_edge_cases();
    test_block_window();
    test_store();
    test_view_pages();
    test_view_short_history();
    test_block_blob();
    test_history_downloads_settle();
    if (s_failures) {
        fprintf(stderr, "test_heatmap: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_heatmap: PASS\n");
    return 0;
}
