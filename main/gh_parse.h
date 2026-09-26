// main/gh_parse.h —— Streaming extractor for GitHub contribution days.
//
// Understands both responses the firmware downloads, fed in arbitrary chunks:
//   - github-contributions-api JSON:  {"date":"2025-09-21","count":3,"level":2}
//   - GitHub's calendar HTML:         <td ... data-date="2025-09-21" ... data-level="2">
// A day is reported when a level follows a date; a "count" between them is
// passed along (JSON only; HTML days report -1). Levels without a preceding
// date (the HTML legend) are ignored, and a new date replaces an unpaired one.
// Whitespace and quotes between a key and its value are skipped.
//
// gh_block_t collects reported days into a fixed window of at most
// GH_BLOCK_DAYS consecutive days, in any arrival order (GitHub's HTML lists
// all Sundays first, then all Mondays, ...).
//
// Pure C without allocation; host-tested with random chunk boundaries.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GH_PATTERNS        5
#define GH_PATTERN_MAX     12
#define GH_LEVEL_MAX       4
#define GH_LEVEL_UNKNOWN   0xFE    // a day inside the window that was not received
#define GH_BLOCK_DAYS      372     // 53 weeks + 1 day: GitHub's rolling year, or a calendar year

typedef void (*gh_day_fn)(void *user, int32_t day, uint8_t level, int32_t count);

typedef struct {
    gh_day_fn on_day;
    void *user;
    uint8_t fail[GH_PATTERNS][GH_PATTERN_MAX];   // KMP failure function per pattern
    uint8_t progress[GH_PATTERNS];               // matched prefix length per pattern
    uint8_t mode;
    uint8_t field;
    uint8_t skipped;
    uint8_t value_len;
    char value[12];
    int32_t pending_day;     // -1: none
    int32_t pending_count;   // -1: unknown
    uint32_t days;           // days reported
    uint32_t rejected;       // malformed dates, levels or counts
} gh_parser_t;

void gh_parser_init(gh_parser_t *p, gh_day_fn on_day, void *user);
void gh_parser_feed(gh_parser_t *p, const char *data, size_t len);

typedef struct {
    int32_t min_day;          // accepted range, inclusive
    int32_t max_day;
    int32_t first_day;        // day of levels[0]; meaningful when span > 0
    uint16_t span;            // consecutive days covered by levels[]
    uint16_t filled;          // distinct days received
    uint32_t total;           // sum of the received counts
    bool counts_known;        // every received day carried a count
    uint32_t rejected;        // days outside the range or the window
    uint8_t levels[GH_BLOCK_DAYS];
} gh_block_t;

void gh_block_init(gh_block_t *b, int32_t min_day, int32_t max_day);
bool gh_block_put(gh_block_t *b, int32_t day, uint8_t level, int32_t count);
// gh_day_fn adapter: pass the block as the parser's user pointer.
void gh_block_on_day(void *block, int32_t day, uint8_t level, int32_t count);
// Last day of the window, or -1 when empty.
int32_t gh_block_last_day(const gh_block_t *b);
// Level of a day; GH_LEVEL_UNKNOWN outside the window or for a gap.
uint8_t gh_block_level(const gh_block_t *b, int32_t day);
// True when every received day has level 0 (no contributions at all).
bool gh_block_is_empty_year(const gh_block_t *b);
