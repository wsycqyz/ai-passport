// main/vocab_srs.h - mastery transitions and session composition (spec 4.1 - 4.4).
//
// Pure functions on the per-word records; caller: the session (app task) and host tests;
// never blocks; no allocation. The spot-check rollback (O11) is vocab_streak_fail() in
// vocab_anticheat.h; vocab_srs_apply() leaves records untouched for that row.
//
// Owner: lead developer (Milestone 1 skeleton; completed and fully tested in Milestone 2).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vocab_rng.h"
#include "vocab_types.h"

// Side effects of one outcome that the session needs for XP and statistics.
typedef struct {
    bool objective;        // an objective answer (Stage 1/3 or check card)
    bool first_try;        // counts for first-try XP: O1, O5, O7, O10, O13
    bool new_mastered;     // the word reached mastery 5 for the first time (ever_mastered set)
} vocab_srs_effect_t;

// INTERVAL[m] in ticks (spec 4.1); 0 for m == 0 or m > 5.
uint32_t vocab_srs_interval(uint8_t mastery);

// True when the word is due at tick T: seen && due <= T (spec 4.2).
bool vocab_srs_is_due(const vocab_record_t *record, vocab_tick_t tick);

// Applies one outcome row of spec 4.3 (O1-O10, O12-O14) to `record` at session tick `tick`
// and marks the word seen. `was_seen` is the SEEN flag before this card was shown: O2 counts
// a lapse only for a word seen before (the session marks words seen when a card starts).
// SPOT_CHECK + WRONG (O11) changes nothing here (the caller runs vocab_streak_fail).
// Unknown combinations change nothing and return a zero effect.
vocab_srs_effect_t vocab_srs_apply(vocab_record_t *record, vocab_purpose_t purpose,
                                   vocab_format_t format, vocab_result_t result,
                                   vocab_tick_t tick, bool was_seen);

// Spec 4.4: chooses the session words (due reviews by overdue-ness, new words in bank
// order within the new-word limits, then early reviews, then more new words) and shuffles
// them with `rng` (Fisher-Yates). Writes up to `cap` bank indices to `out` and returns the
// count (min(VOCAB_SESSION_WORDS, bank size, cap)); 0 for an empty bank.
size_t vocab_srs_compose(const vocab_bank_t *bank, const vocab_record_t *records,
                         vocab_tick_t tick, vocab_rng_t *rng, vocab_index_t *out, size_t cap);
