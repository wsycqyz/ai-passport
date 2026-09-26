// main/hm_calendar.c —— see hm_calendar.h. The civil/day conversions are
// Howard Hinnant's public-domain "days_from_civil" algorithms.
#include "hm_calendar.h"

#include <stddef.h>

static int32_t floor_div(int32_t a, int32_t b)
{
    int32_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

int32_t hm_days_from_civil(int year, int month, int day)
{
    const int32_t y = year - (month <= 2);
    const int32_t era = floor_div(y, 400);
    const int32_t yoe = y - era * 400;                                          // [0, 399]
    const int32_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;  // [0, 365]
    const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                  // [0, 146096]
    return era * 146097 + doe - 719468;
}

void hm_civil_from_days(int32_t days, int *year, int *month, int *day)
{
    const int32_t z = days + 719468;
    const int32_t era = floor_div(z, 146097);
    const int32_t doe = z - era * 146097;
    const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int32_t mp = (5 * doy + 2) / 153;
    const int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    const int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (year) *year = (int)(yoe + era * 400 + (m <= 2));
    if (month) *month = m;
    if (day) *day = d;
}

int hm_year_of(int32_t days)
{
    int year;
    hm_civil_from_days(days, &year, NULL, NULL);
    return year;
}

int hm_weekday(int32_t days)
{
    return (int)(days + 4 - 7 * floor_div(days + 4, 7));
}

int32_t hm_week_of(int32_t days)
{
    return floor_div(days + 4, 7);
}

int32_t hm_week_start(int32_t week)
{
    return week * 7 - 4;
}

int hm_days_in_month(int year, int month)
{
    static const unsigned char DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return DAYS[month - 1];
}

static bool digits(const char *text, int count, int *value)
{
    int v = 0;
    for (int i = 0; i < count; i++) {
        if (text[i] < '0' || text[i] > '9') return false;
        v = v * 10 + (text[i] - '0');
    }
    *value = v;
    return true;
}

bool hm_parse_iso_date(const char *text, int32_t *days)
{
    int year, month, day;
    if (!text || !digits(text, 4, &year) || text[4] != '-' || !digits(text + 5, 2, &month) ||
        text[7] != '-' || !digits(text + 8, 2, &day) || text[10] != '\0') {
        return false;
    }
    if (year < 1970 || year > 2199 || day < 1 || day > hm_days_in_month(year, month)) return false;
    *days = hm_days_from_civil(year, month, day);
    return true;
}

const char *hm_month_abbr(int month)
{
    static const char *const NAMES[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    return month >= 1 && month <= 12 ? NAMES[month - 1] : "";
}
