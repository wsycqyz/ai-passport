// main/hm_store.h —— Contribution levels downloaded so far, by day.
//
// Holds GitHub's rolling "last year" (refreshed periodically; its newest day
// is "today") plus up to HM_MAX_YEARS older calendar years that are fetched on
// demand while the user scrolls back. Where both cover a day, the rolling year
// wins, so the newest weeks match the GitHub profile.
//
// History begins at the newest of: HM_FIRST_YEAR, the oldest storable year,
// and the year after the first fetched calendar year without a single
// contribution (so scrolling back stops before an empty stretch).
//
// Pure C, owned by the controller task; host-tested.
#pragma once

#include "gh_parse.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HM_LEVEL_NONE      0xFF              // not a calendar day: future, or before history
#define HM_LEVEL_UNKNOWN   GH_LEVEL_UNKNOWN  // a calendar day that has not been downloaded
#define HM_MAX_YEARS       10                // older calendar years kept besides the last year
#define HM_FIRST_YEAR      2008              // GitHub's first year
#define HM_MIN_LAST_DAYS   300               // plausibility floor for a rolling year
#define HM_MIN_YEAR_DAYS   300               // plausibility floor for a calendar year

typedef struct {
    bool have_last;
    int32_t today;                      // newest day of the rolling year
    int16_t floor_year;                 // 0 until an empty year has been found
    gh_block_t last;
    gh_block_t years[HM_MAX_YEARS];
    int16_t year_of[HM_MAX_YEARS];      // 0: free slot
} hm_store_t;

void hm_store_init(hm_store_t *s);

// Plausibility checks, also used by the downloader to decide on its fallback.
bool hm_store_last_plausible(const gh_block_t *b);
bool hm_store_year_plausible(int year, const gh_block_t *b);

// Install downloaded data; false (store unchanged) when implausible.
bool hm_store_set_last(hm_store_t *s, const gh_block_t *b);
bool hm_store_set_year(hm_store_t *s, int year, const gh_block_t *b);

// Earliest day the calendar may show; valid once have_last is set.
int32_t hm_store_first_day(const hm_store_t *s);

// 0..4, HM_LEVEL_UNKNOWN, or HM_LEVEL_NONE.
uint8_t hm_store_level(const hm_store_t *s, int32_t day);

// Newest calendar year that still has to be downloaded to cover the days
// [from_day, to_day] (clipped to the history); 0 when nothing is missing.
int hm_store_missing_year(const hm_store_t *s, int32_t from_day, int32_t to_day);

// Oldest calendar year the store accepts; valid once have_last is set.
int hm_store_oldest_year(const hm_store_t *s);

// The same without the history limit: the oldest year of the 10-year window.
// Years from here on are worth keeping in flash; the empty year just below the
// limit is what restores that limit after a restart.
int hm_store_window_oldest_year(const hm_store_t *s);

// The stored block of a calendar year, or NULL.
const gh_block_t *hm_store_year_block(const hm_store_t *s, int year);

// Blocks as persisted in flash: a 12-byte header (format 1, flags, first day,
// span, total; little-endian) followed by one level byte per day. filled is
// recomputed on decode; anything malformed or outside [min_day, max_day] is
// rejected.
#define HM_BLOB_HEADER  12
#define HM_BLOB_MAX     (HM_BLOB_HEADER + GH_BLOCK_DAYS)

// Returns the encoded length, or 0 when the block is empty or cap too small.
size_t hm_block_encode(const gh_block_t *b, uint8_t *out, size_t cap);
bool hm_block_decode(const uint8_t *in, size_t len, int32_t min_day, int32_t max_day, gh_block_t *out);

// Same days, levels and totals (range limits and counters are ignored).
bool hm_block_same(const gh_block_t *a, const gh_block_t *b);
