// main/hm_calendar.h —— Proleptic Gregorian date arithmetic for the heatmap.
//
// Pure C (host-tested). A "day" is the number of days since 1970-01-01, the
// representation the contribution store and the page model use. Weeks start
// on Sunday, as in GitHub's contribution calendar; week w starts on day
// hm_week_start(w), and 1970-01-01 (a Thursday) is in week 0.
#pragma once

#include <stdbool.h>
#include <stdint.h>

int32_t hm_days_from_civil(int year, int month, int day);
void hm_civil_from_days(int32_t days, int *year, int *month, int *day);
int hm_year_of(int32_t days);
int hm_weekday(int32_t days);          // 0 = Sunday .. 6 = Saturday
int32_t hm_week_of(int32_t days);
int32_t hm_week_start(int32_t week);   // the Sunday of that week
int hm_days_in_month(int year, int month);

// Strict "YYYY-MM-DD": exactly ten characters naming a real date in 1970..2199.
bool hm_parse_iso_date(const char *text, int32_t *days);

// "Jan".."Dec"; "" when month is outside 1..12.
const char *hm_month_abbr(int month);
