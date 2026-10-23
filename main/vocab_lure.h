// main/vocab_lure.h - Stage 1 lure (distractor meaning) selection (spec 5.2).
//
// Pure functions with an injected RNG. The session calls vocab_lure_pick() when it decides
// a card's format: no valid lure -> the card is served as Stage 2 (spec 4.6, AC-14).
// The lure is shown only as its Chinese meaning; English lures are never displayed.
//
// Caller: session (app task) and host tests; never blocks; no allocation.
// Owner: Stage 1 developer. Contract written by the lead; only additive changes.
#pragma once

#include <stdbool.h>

#include "vocab_rng.h"
#include "vocab_types.h"

// Spec 5.2 step 1 (step 3 when allow_same_category): true when `candidate` may serve as a
// lure for `target`: a different entry (different uid), a different category (unless
// allow_same_category), a different meaning string, and neither meaning is a substring of
// the other (byte-wise on UTF-8, e.g. "book" vs "open your book" meanings). NULL -> false.
// A NULL or empty meaning on either side -> false. The kinds (WORD/PHRASE) do not matter
// here; vocab_lure_pick() applies the kind preference. Symmetric in target/candidate.
bool vocab_lure_acceptable(const vocab_entry_t *target, const vocab_entry_t *candidate,
                           bool allow_same_category);

// Spec 5.2 steps 1-4. Picks a lure for bank entry `target`:
//   1. candidates per vocab_lure_acceptable(..., false);
//   2. prefer candidates of the same kind (WORD/PHRASE, vocab_entry_kind) when any exist;
//   3. if there are none at all, relax the category rule (same category allowed), again
//      preferring the same kind;
//   4. pick uniformly with `rng` among the preferred set; `avoid` (VOCAB_INDEX_NONE for
//      none) is excluded when another candidate remains (inserted cards avoid the lure
//      already used for that word in this session).
// The preferred set is therefore the first non-empty tier of: other category + same kind,
// other category + any kind, same category allowed + same kind, same category allowed +
// any kind. A different category always wins over the kind preference. `avoid` only
// narrows the preferred set (it never moves the pick to a weaker tier); an `avoid` that is
// not in the set (NONE, the target, out of range, another tier) has no effect.
// Returns true and writes *out_lure (an index != target). Returns false and leaves
// *out_lure untouched when no acceptable lure exists, the bank is NULL/empty or `target`
// is out of range (also for a NULL `rng` or `out_lure`). With valid arguments the pick
// succeeds exactly when some bank entry is vocab_lure_acceptable(target, entry, true), so
// callers may predict it without consuming randomness. Consumes RNG values only when it
// returns true: one vocab_rng_below() draw, none when the preferred set has one entry.
// Cost: two passes over the bank, no memory beyond a few locals (device-safe).
bool vocab_lure_pick(const vocab_bank_t *bank, vocab_index_t target, vocab_index_t avoid,
                     vocab_rng_t *rng, vocab_index_t *out_lure);
