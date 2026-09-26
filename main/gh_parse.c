// main/gh_parse.c —— see gh_parse.h.
#include "gh_parse.h"

#include "hm_calendar.h"

#include <string.h>

enum { MODE_SCAN = 0, MODE_SKIP, MODE_VALUE };
enum { FIELD_DATE = 0, FIELD_LEVEL, FIELD_COUNT };

#define SKIP_MAX    4       // separators allowed between a key and its value
#define DATE_LEN    10      // YYYY-MM-DD
#define NUMBER_MAX  9       // digits; longer numbers are rejected

static const struct {
    const char *text;
    uint8_t field;
} PATTERNS[GH_PATTERNS] = {
    { "\"date\":", FIELD_DATE },
    { "data-date=", FIELD_DATE },
    { "\"level\":", FIELD_LEVEL },
    { "data-level=", FIELD_LEVEL },
    { "\"count\":", FIELD_COUNT },
};

void gh_parser_init(gh_parser_t *p, gh_day_fn on_day, void *user)
{
    memset(p, 0, sizeof(*p));
    p->on_day = on_day;
    p->user = user;
    p->pending_day = -1;
    p->pending_count = -1;
    for (int k = 0; k < GH_PATTERNS; k++) {
        // KMP prefix function, so a partial match never hides an overlapping one
        // (e.g. "data-data-date=").
        const char *pat = PATTERNS[k].text;
        uint8_t q = 0;
        for (size_t i = 1; pat[i]; i++) {
            while (q > 0 && pat[i] != pat[q]) q = p->fail[k][q - 1];
            if (pat[i] == pat[q]) q++;
            p->fail[k][i] = q;
        }
    }
}

static void begin_value(gh_parser_t *p, uint8_t field)
{
    p->mode = MODE_SKIP;
    p->field = field;
    p->skipped = 0;
    p->value_len = 0;
    memset(p->progress, 0, sizeof(p->progress));
}

static void scan(gh_parser_t *p, char c)
{
    for (int k = 0; k < GH_PATTERNS; k++) {
        const char *pat = PATTERNS[k].text;
        uint8_t q = p->progress[k];
        while (q > 0 && pat[q] != c) q = p->fail[k][q - 1];
        if (pat[q] == c) q++;
        if (pat[q] == '\0') {
            begin_value(p, PATTERNS[k].field);
            return;
        }
        p->progress[k] = q;
    }
}

static void reject(gh_parser_t *p)
{
    p->rejected++;
    // A broken date or level loses its day; a broken count only its count.
    if (p->field != FIELD_COUNT) {
        p->pending_day = -1;
        p->pending_count = -1;
    }
    p->mode = MODE_SCAN;
}

static void finish_date(gh_parser_t *p)
{
    p->value[p->value_len] = '\0';
    int32_t day;
    if (hm_parse_iso_date(p->value, &day)) {
        p->pending_day = day;
        p->pending_count = -1;
        p->mode = MODE_SCAN;
    } else {
        reject(p);
    }
}

static void finish_number(gh_parser_t *p)
{
    int32_t n = 0;
    for (uint8_t i = 0; i < p->value_len; i++) n = n * 10 + (p->value[i] - '0');
    p->mode = MODE_SCAN;
    if (p->field == FIELD_COUNT) {
        if (p->pending_day >= 0) p->pending_count = n;
        return;
    }
    if (p->pending_day < 0) return;   // a level that belongs to no day, e.g. the legend
    if (n > GH_LEVEL_MAX) {
        reject(p);
        return;
    }
    if (p->on_day) p->on_day(p->user, p->pending_day, (uint8_t)n, p->pending_count);
    p->days++;
    p->pending_day = -1;
    p->pending_count = -1;
}

// Returns false when c ended the value and must be scanned as ordinary text.
static bool value_char(gh_parser_t *p, char c)
{
    const bool digit = c >= '0' && c <= '9';
    if (p->field == FIELD_DATE) {
        if (!digit && c != '-') {
            reject(p);
            return false;
        }
        p->value[p->value_len++] = c;
        if (p->value_len == DATE_LEN) finish_date(p);
        return true;
    }
    if (digit) {
        if (p->value_len == NUMBER_MAX) {
            reject(p);
            return false;
        }
        p->value[p->value_len++] = c;
        return true;
    }
    if (p->value_len) {
        finish_number(p);
    } else {
        reject(p);   // e.g. "level":null
    }
    return false;
}

void gh_parser_feed(gh_parser_t *p, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (p->mode == MODE_SKIP) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\'') {
                if (++p->skipped <= SKIP_MAX) continue;
                reject(p);   // then scan c: a quote may start the next key
            } else {
                p->mode = MODE_VALUE;
            }
        }
        if (p->mode == MODE_VALUE && value_char(p, c)) continue;
        scan(p, c);
    }
}

void gh_block_init(gh_block_t *b, int32_t min_day, int32_t max_day)
{
    memset(b, 0, sizeof(*b));
    b->min_day = min_day;
    b->max_day = max_day;
    b->counts_known = true;
}

bool gh_block_put(gh_block_t *b, int32_t day, uint8_t level, int32_t count)
{
    if (day < b->min_day || day > b->max_day || level > GH_LEVEL_MAX) {
        b->rejected++;
        return false;
    }
    if (b->span == 0) {
        b->first_day = day;
        b->span = 1;
        b->levels[0] = GH_LEVEL_UNKNOWN;
    } else if (day < b->first_day) {
        const int32_t shift = b->first_day - day;
        if (b->span + shift > GH_BLOCK_DAYS) {
            b->rejected++;
            return false;
        }
        memmove(b->levels + shift, b->levels, b->span);
        memset(b->levels, GH_LEVEL_UNKNOWN, (size_t)shift);
        b->first_day = day;
        b->span = (uint16_t)(b->span + shift);
    } else if (day - b->first_day >= b->span) {
        const int32_t need = day - b->first_day + 1;
        if (need > GH_BLOCK_DAYS) {
            b->rejected++;
            return false;
        }
        memset(b->levels + b->span, GH_LEVEL_UNKNOWN, (size_t)(need - b->span));
        b->span = (uint16_t)need;
    }
    uint8_t *slot = &b->levels[day - b->first_day];
    if (*slot == GH_LEVEL_UNKNOWN) {
        b->filled++;
        if (count < 0) {
            b->counts_known = false;
        } else {
            const uint32_t add = (uint32_t)count;
            b->total = b->total > UINT32_MAX - add ? UINT32_MAX : b->total + add;
        }
    }
    *slot = level;
    return true;
}

void gh_block_on_day(void *block, int32_t day, uint8_t level, int32_t count)
{
    gh_block_put((gh_block_t *)block, day, level, count);
}

int32_t gh_block_last_day(const gh_block_t *b)
{
    return b->span ? b->first_day + b->span - 1 : -1;
}

uint8_t gh_block_level(const gh_block_t *b, int32_t day)
{
    if (!b->span || day < b->first_day || day - b->first_day >= b->span) return GH_LEVEL_UNKNOWN;
    return b->levels[day - b->first_day];
}

bool gh_block_is_empty_year(const gh_block_t *b)
{
    if (!b->filled) return false;
    for (uint16_t i = 0; i < b->span; i++) {
        if (b->levels[i] != 0 && b->levels[i] != GH_LEVEL_UNKNOWN) return false;
    }
    return true;
}
