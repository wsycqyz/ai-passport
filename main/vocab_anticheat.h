// main/vocab_anticheat.h - "I know" streak, spot-check target and rollback (spec 4.8).
//
// Pure bookkeeping used by the session (app task) and host tests; never blocks, no
// allocation. The session owns one vocab_streak_t per session and drives it:
//   - Stage 2 KNOW (O3): BEFORE applying the promotion, call vocab_streak_claim() with the
//     record as it is now (pre-claim). A true return means the threshold was reached: the
//     session picks the target with vocab_streak_pick_target(), inserts a SPOT_CHECK card
//     as the next card (format: Stage 3 if the target has cloze data and its pre-claim
//     mastery >= 2, else Stage 1) and shows the 800 ms intro banner. If the card cannot be
//     inserted the session calls vocab_streak_clear() (claims stand).
//   - SPOT_CHECK first press correct (O10): vocab_streak_pass().
//   - SPOT_CHECK first press wrong (O11): vocab_streak_fail(). It restores EVER_MASTERED
//     from the snapshots, so a SUMMARY bonus derived from that flag is voided as well.
//   - Stage 2 FORGOT (O4), session start, pause -> HOME: vocab_streak_clear().
//   - Stage 1 / Stage 3 cards in between do not touch the streak.
//
// Owner: Stage 2 developer. Contract written by the lead; only additive changes.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocab_config.h"
#include "vocab_rng.h"
#include "vocab_types.h"

// One "I know" claim with the record state before the O3 promotion.
typedef struct {
    vocab_index_t index;     // bank index == records[] index of the claimed word
    uint8_t pre_mastery;     // mastery before the claim
    uint8_t pre_flags;       // VOCAB_REC_* flags before the claim (EVER_MASTERED may change)
    vocab_tick_t pre_due;    // due before the claim
} vocab_claim_t;

// Claims since the last spot check, "forgot" or session start, oldest first.
typedef struct {
    uint8_t count;                                   // 0..VOCAB_SPOT_CHECK_THRESHOLD
    vocab_claim_t claims[VOCAB_SPOT_CHECK_THRESHOLD];
} vocab_streak_t;

// Empties the streak (claims already applied stand).
void vocab_streak_clear(vocab_streak_t *streak);

// Appends a claim for bank index `index` with the pre-claim record `pre`. Returns true when
// the streak now holds VOCAB_SPOT_CHECK_THRESHOLD claims (insert a spot check now). If the
// streak is already full (caller did not clear it), the oldest claim is dropped first.
// A NULL `streak`/`pre` or index VOCAB_INDEX_NONE records nothing and returns false.
bool vocab_streak_claim(vocab_streak_t *streak, vocab_index_t index, const vocab_record_t *pre);

// Chooses the spot-check target uniformly with `rng` among claims[0 .. count-2], i.e.
// excluding the most recent claim (its meaning was on screen a moment ago). Writes the
// claim slot to *out_slot and returns true; returns false when count < 2.
// Details: earlier claims of the same word as the most recent claim are excluded too (only
// possible if the session ever claims a word twice); false when no candidate is left.
// Exactly one vocab_rng_below(rng, candidates) draw (none with a single candidate); a
// NULL `rng` deterministically picks the oldest candidate.
bool vocab_streak_pick_target(const vocab_streak_t *streak, vocab_rng_t *rng, uint8_t *out_slot);

// O10: the spot check passed. All claims stand; the streak is cleared.
void vocab_streak_pass(vocab_streak_t *streak);

// O11: the spot check failed. Full-streak rollback on `records` (indexed like the bank,
// `record_count` entries; claims with an out-of-range index are skipped), then clear:
//   - target claim (slot `target_slot`): mastery = min(pre_mastery, 1); due = tick + 1
//     (saturating, vocab_tick_add); lapses + 1 (saturating at 255); the EVER_MASTERED flag
//     restored from pre_flags (SEEN stays set).
//   - every other claim: mastery = pre_mastery; due = min(pre_due, tick + 1); EVER_MASTERED
//     restored from pre_flags; lapses unchanged.
// `tick` is the running session's tick T. An invalid target_slot only rolls back the others.
// Details: restored masteries are clamped to 0..VOCAB_MASTERY_MAX; uid, SEEN and the other
// flags are never changed. If a word appears in several claims (not expected), its earliest
// snapshot (the state before the whole streak) is used, and the target rule wins for it.
void vocab_streak_fail(vocab_streak_t *streak, uint8_t target_slot, vocab_record_t *records,
                       vocab_index_t record_count, vocab_tick_t tick);
