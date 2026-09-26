// main/hm_store.c —— see hm_store.h.
#include "hm_store.h"

#include "hm_calendar.h"

#include <string.h>

void hm_store_init(hm_store_t *s)
{
    memset(s, 0, sizeof(*s));
}

bool hm_store_last_plausible(const gh_block_t *b)
{
    return b->filled >= HM_MIN_LAST_DAYS && b->span <= GH_BLOCK_DAYS &&
           hm_year_of(b->first_day) >= HM_FIRST_YEAR;
}

bool hm_store_year_plausible(int year, const gh_block_t *b)
{
    return year >= HM_FIRST_YEAR && b->filled >= HM_MIN_YEAR_DAYS &&
           b->first_day >= hm_days_from_civil(year, 1, 1) &&
           gh_block_last_day(b) <= hm_days_from_civil(year, 12, 31);
}

bool hm_store_set_last(hm_store_t *s, const gh_block_t *b)
{
    if (!hm_store_last_plausible(b)) return false;
    s->last = *b;
    s->today = gh_block_last_day(b);
    s->have_last = true;
    return true;
}

static int slot_of(const hm_store_t *s, int year)
{
    for (int i = 0; i < HM_MAX_YEARS; i++) {
        if (s->year_of[i] == year) return i;
    }
    return -1;
}

// Calendar years are only needed before the rolling year. Counting the limit
// from its first day keeps them within HM_MAX_YEARS slots even when the
// rolling year starts on Jan 2 (Dec 31 of a leap year falling on a Sunday).
static int window_oldest(const hm_store_t *s)
{
    const int oldest = hm_year_of(s->last.first_day - 1) - (HM_MAX_YEARS - 1);
    return oldest < HM_FIRST_YEAR ? HM_FIRST_YEAR : oldest;
}

static int oldest_year(const hm_store_t *s)
{
    const int window = window_oldest(s);
    return s->floor_year > window ? s->floor_year : window;
}

bool hm_store_set_year(hm_store_t *s, int year, const gh_block_t *b)
{
    if (!s->have_last || !hm_store_year_plausible(year, b) || year < oldest_year(s)) return false;
    int slot = slot_of(s, year);
    if (slot < 0) slot = slot_of(s, 0);
    if (slot < 0) {
        // Full: replace the stored year farthest from today.
        slot = 0;
        for (int i = 1; i < HM_MAX_YEARS; i++) {
            if (s->year_of[i] < s->year_of[slot]) slot = i;
        }
    }
    s->years[slot] = *b;
    s->year_of[slot] = (int16_t)year;
    if (gh_block_is_empty_year(b) && year + 1 > s->floor_year) s->floor_year = (int16_t)(year + 1);
    return true;
}

int32_t hm_store_first_day(const hm_store_t *s)
{
    return hm_days_from_civil(oldest_year(s), 1, 1);
}

uint8_t hm_store_level(const hm_store_t *s, int32_t day)
{
    if (!s->have_last || day > s->today || day < hm_store_first_day(s)) return HM_LEVEL_NONE;
    if (day >= s->last.first_day) return gh_block_level(&s->last, day);
    const int slot = slot_of(s, hm_year_of(day));
    return slot < 0 ? HM_LEVEL_UNKNOWN : gh_block_level(&s->years[slot], day);
}

int hm_store_missing_year(const hm_store_t *s, int32_t from_day, int32_t to_day)
{
    if (!s->have_last) return 0;
    const int32_t first = hm_store_first_day(s);
    if (from_day < first) from_day = first;
    if (to_day > s->today) to_day = s->today;
    // Only days before the rolling year can be missing.
    if (to_day >= s->last.first_day) to_day = s->last.first_day - 1;
    if (from_day > to_day) return 0;
    for (int year = hm_year_of(to_day); year >= hm_year_of(from_day); year--) {
        if (slot_of(s, year) < 0) return year;
    }
    return 0;
}

int hm_store_oldest_year(const hm_store_t *s)
{
    return oldest_year(s);
}

int hm_store_window_oldest_year(const hm_store_t *s)
{
    return window_oldest(s);
}

const gh_block_t *hm_store_year_block(const hm_store_t *s, int year)
{
    const int slot = year > 0 ? slot_of(s, year) : -1;
    return slot < 0 ? NULL : &s->years[slot];
}

enum { BLOB_FORMAT = 1, BLOB_COUNTS_KNOWN = 0x01 };

static void put_le(uint8_t *out, uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; i++) out[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t get_le(const uint8_t *in, int bytes)
{
    uint32_t value = 0;
    for (int i = 0; i < bytes; i++) value |= (uint32_t)in[i] << (8 * i);
    return value;
}

size_t hm_block_encode(const gh_block_t *b, uint8_t *out, size_t cap)
{
    const size_t len = HM_BLOB_HEADER + b->span;
    if (!b->span || b->span > GH_BLOCK_DAYS || cap < len) return 0;
    out[0] = BLOB_FORMAT;
    out[1] = b->counts_known ? BLOB_COUNTS_KNOWN : 0;
    put_le(out + 2, (uint32_t)b->first_day, 4);
    put_le(out + 6, b->span, 2);
    put_le(out + 8, b->total, 4);
    memcpy(out + HM_BLOB_HEADER, b->levels, b->span);
    return len;
}

bool hm_block_decode(const uint8_t *in, size_t len, int32_t min_day, int32_t max_day, gh_block_t *out)
{
    if (len < HM_BLOB_HEADER || in[0] != BLOB_FORMAT || (in[1] & ~BLOB_COUNTS_KNOWN)) return false;
    const int32_t first = (int32_t)get_le(in + 2, 4);
    const uint32_t span = get_le(in + 6, 2);
    if (span == 0 || span > GH_BLOCK_DAYS || len != HM_BLOB_HEADER + span) return false;
    if (first < min_day || first > max_day || (int64_t)first + span - 1 > max_day) return false;
    gh_block_init(out, min_day, max_day);
    for (uint32_t i = 0; i < span; i++) {
        const uint8_t level = in[HM_BLOB_HEADER + i];
        if (level > GH_LEVEL_MAX && level != GH_LEVEL_UNKNOWN) return false;
        out->levels[i] = level;
        if (level != GH_LEVEL_UNKNOWN) out->filled++;
    }
    out->first_day = first;
    out->span = (uint16_t)span;
    out->total = get_le(in + 8, 4);
    out->counts_known = in[1] & BLOB_COUNTS_KNOWN;
    return true;
}

bool hm_block_same(const gh_block_t *a, const gh_block_t *b)
{
    return a->first_day == b->first_day && a->span == b->span && a->total == b->total &&
           a->counts_known == b->counts_known && memcmp(a->levels, b->levels, a->span) == 0;
}
