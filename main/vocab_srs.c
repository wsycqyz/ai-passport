// main/vocab_srs.c - mastery transitions and session composition (see vocab_srs.h).
#include "vocab_srs.h"

#include "vocab_config.h"

static const uint8_t INTERVALS[VOCAB_MASTERY_MAX + 1u] = VOCAB_INTERVALS_INIT;

uint32_t vocab_srs_interval(uint8_t mastery)
{
    return mastery <= VOCAB_MASTERY_MAX ? INTERVALS[mastery] : 0u;
}

bool vocab_srs_is_due(const vocab_record_t *record, vocab_tick_t tick)
{
    return vocab_record_seen(record) && record->due <= tick;
}

static uint8_t min_u8(uint8_t a, uint8_t b)
{
    return a < b ? a : b;
}

static void set_level(vocab_record_t *rec, uint8_t mastery, vocab_tick_t due,
                      vocab_srs_effect_t *effect)
{
    rec->mastery = mastery > VOCAB_MASTERY_MAX ? (uint8_t)VOCAB_MASTERY_MAX : mastery;
    rec->due = due;
    if (rec->mastery == VOCAB_MASTERY_MAX && !(rec->flags & VOCAB_REC_EVER_MASTERED)) {
        rec->flags |= VOCAB_REC_EVER_MASTERED;
        effect->new_mastered = true;
    }
}

static void promote(vocab_record_t *rec, vocab_tick_t tick, vocab_srs_effect_t *effect)
{
    const uint8_t next = min_u8((uint8_t)(rec->mastery + 1u), (uint8_t)VOCAB_MASTERY_MAX);
    set_level(rec, next, vocab_tick_add(tick, vocab_srs_interval(next)), effect);
}

static void lapse(vocab_record_t *rec)
{
    if (rec->lapses < 255u) rec->lapses++;
}

vocab_srs_effect_t vocab_srs_apply(vocab_record_t *rec, vocab_purpose_t purpose,
                                   vocab_format_t format, vocab_result_t result,
                                   vocab_tick_t tick, bool was_seen)
{
    vocab_srs_effect_t effect = { false, false, false };
    if (!rec) return effect;
    const vocab_tick_t next = vocab_tick_add(tick, 1u);
    rec->flags |= VOCAB_REC_SEEN;

    if (result == VOCAB_RESULT_TOO_EASY) return effect;  // O12: unchanged
    effect.objective = format == VOCAB_FORMAT_S1 || format == VOCAB_FORMAT_S3;

    switch (purpose) {
    case VOCAB_PURPOSE_LEARN:
        if (format == VOCAB_FORMAT_S2) {
            effect.objective = false;
            if (result == VOCAB_RESULT_KNOW) {                       // O3
                promote(rec, tick, &effect);
            } else if (result == VOCAB_RESULT_FORGOT) {              // O4
                set_level(rec, min_u8(rec->mastery, 1u), next, &effect);
                lapse(rec);
            }
        } else if (format == VOCAB_FORMAT_S1) {
            if (result == VOCAB_RESULT_CORRECT_FIRST) {              // O1
                set_level(rec, 1u, next, &effect);
                effect.first_try = true;
            } else if (result == VOCAB_RESULT_WRONG) {               // O2
                set_level(rec, 0u, next, &effect);
                if (was_seen) lapse(rec);
            }
        } else if (format == VOCAB_FORMAT_S3) {
            if (result == VOCAB_RESULT_CORRECT_FIRST) {              // O7
                promote(rec, tick, &effect);
                effect.first_try = true;
            } else if (result == VOCAB_RESULT_CORRECT_SECOND) {      // O8
                rec->due = next;
            } else if (result == VOCAB_RESULT_WRONG) {               // O9
                set_level(rec, 2u, next, &effect);
                lapse(rec);
            }
        }
        break;
    case VOCAB_PURPOSE_TEST_LATER:
        if (result == VOCAB_RESULT_CORRECT_FIRST) {                  // O5
            effect.first_try = true;
        } else if (result == VOCAB_RESULT_WRONG) {                   // O6
            set_level(rec, 0u, next, &effect);
            lapse(rec);
        }
        break;
    case VOCAB_PURPOSE_SPOT_CHECK:
        if (result == VOCAB_RESULT_CORRECT_FIRST) effect.first_try = true;  // O10; O11 elsewhere
        break;
    case VOCAB_PURPOSE_TOO_EASY_CHECK:
        if (result == VOCAB_RESULT_CORRECT_FIRST) {                  // O13
            set_level(rec, (uint8_t)VOCAB_MASTERY_MAX,
                      vocab_tick_add(tick, VOCAB_TOO_EASY_INTERVAL), &effect);
            effect.first_try = true;
        } else if (result == VOCAB_RESULT_WRONG) {                   // O14
            set_level(rec, min_u8(rec->mastery, 1u), next, &effect);
            lapse(rec);
        }
        break;
    default:
        break;
    }
    return effect;
}

// Ordering key for due reviews (D) and early reviews (E): lower due first (D: most overdue
// first), then lower mastery, then lower uid.
static bool review_before(const vocab_bank_t *bank, const vocab_record_t *records,
                          vocab_index_t a, vocab_index_t b)
{
    const vocab_record_t *ra = &records[a];
    const vocab_record_t *rb = &records[b];
    if (ra->due != rb->due) return ra->due < rb->due;
    if (ra->mastery != rb->mastery) return ra->mastery < rb->mastery;
    return bank->entries[a].uid < bank->entries[b].uid;
}

// Keeps the `k` best candidates (by review_before) in `best`, sorted, while scanning.
static void top_k_insert(const vocab_bank_t *bank, const vocab_record_t *records,
                         vocab_index_t *best, size_t *n, size_t k, vocab_index_t candidate)
{
    size_t pos = *n;
    while (pos > 0 && review_before(bank, records, candidate, best[pos - 1])) pos--;
    if (pos >= k) return;
    size_t end = *n < k ? *n : k - 1;
    for (size_t i = end; i > pos; i--) best[i] = best[i - 1];
    best[pos] = candidate;
    if (*n < k) (*n)++;
}

size_t vocab_srs_compose(const vocab_bank_t *bank, const vocab_record_t *records,
                         vocab_tick_t tick, vocab_rng_t *rng, vocab_index_t *out, size_t cap)
{
    if (!bank || !bank->entries || !records || !out || bank->count == 0 || cap == 0) return 0;
    size_t want = VOCAB_SESSION_WORDS;
    if (want > bank->count) want = bank->count;
    if (want > cap) want = cap;

    vocab_index_t due[VOCAB_SESSION_WORDS];
    vocab_index_t early[VOCAB_SESSION_WORDS];
    size_t n_due_best = 0;
    size_t n_early_best = 0;
    size_t due_total = 0;
    size_t new_total = 0;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_record_t *r = &records[i];
        if (!vocab_record_seen(r)) {
            new_total++;
        } else if (r->due <= tick) {
            due_total++;
            top_k_insert(bank, records, due, &n_due_best, want, i);
        } else {
            top_k_insert(bank, records, early, &n_early_best, want, i);
        }
    }

    size_t n_new = VOCAB_SESSION_WORDS > due_total ? VOCAB_SESSION_WORDS - due_total : 0;
    if (n_new < VOCAB_MIN_NEW_PER_SESSION) n_new = VOCAB_MIN_NEW_PER_SESSION;
    if (n_new > VOCAB_MAX_NEW_PER_SESSION) n_new = VOCAB_MAX_NEW_PER_SESSION;
    if (n_new > new_total) n_new = new_total;
    if (n_new > want) n_new = want;
    size_t n_rev = want - n_new;
    if (n_rev > n_due_best) n_rev = n_due_best;

    size_t count = 0;
    for (size_t i = 0; i < n_rev; i++) out[count++] = due[i];
    // New words in bank order: first within the cap, later (step 6) beyond it.
    size_t new_taken = 0;
    for (vocab_index_t i = 0; i < bank->count && new_taken < n_new; i++) {
        if (!vocab_record_seen(&records[i])) {
            out[count++] = i;
            new_taken++;
        }
    }
    for (size_t i = 0; i < n_early_best && count < want; i++) out[count++] = early[i];
    if (count < want) {
        size_t skip = new_taken;
        for (vocab_index_t i = 0; i < bank->count && count < want; i++) {
            if (vocab_record_seen(&records[i])) continue;
            if (skip > 0) {
                skip--;
                continue;
            }
            out[count++] = i;
        }
    }
    if (rng) vocab_rng_shuffle(rng, out, count);
    return count;
}
