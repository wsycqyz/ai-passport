// main/vocab_anticheat.c - "I know" streak, spot-check target and rollback (spec 4.8); see
// vocab_anticheat.h.
//
// The streak lists the Stage 2 "I know" claims since the last spot check, "forgot" or
// session start, each with the record as it was before the claim's promotion. A spot check
// verifies one of the claims except the most recent one; if it fails, the whole streak is
// voided: the target is demoted and every other claim is restored from its snapshot.
// Pure bookkeeping: the state lives in the caller's vocab_streak_t; no allocation.
#include "vocab_anticheat.h"

#include <string.h>

_Static_assert(VOCAB_SPOT_CHECK_THRESHOLD >= 2u && VOCAB_SPOT_CHECK_THRESHOLD <= 16u,
               "a spot check needs a claim besides the most recent one");

// Number of valid claims; a corrupted count never indexes past the array.
static uint8_t claim_count(const vocab_streak_t *streak)
{
    return streak->count < VOCAB_SPOT_CHECK_THRESHOLD ? streak->count
                                                      : (uint8_t)VOCAB_SPOT_CHECK_THRESHOLD;
}

// Slot of the earliest claim of bank index `index` (its snapshot is the state before the
// streak), or `count` when the word was not claimed.
static uint8_t first_slot_of(const vocab_streak_t *streak, uint8_t count, vocab_index_t index)
{
    for (uint8_t i = 0; i < count; i++) {
        if (streak->claims[i].index == index) return i;
    }
    return count;
}

static uint8_t clamp_mastery(uint8_t mastery)
{
    return mastery > VOCAB_MASTERY_MAX ? (uint8_t)VOCAB_MASTERY_MAX : mastery;
}

static void restore_ever_mastered(vocab_record_t *rec, uint8_t pre_flags)
{
    rec->flags = (uint8_t)((rec->flags & (uint8_t)~VOCAB_REC_EVER_MASTERED) |
                           (pre_flags & VOCAB_REC_EVER_MASTERED));
}

void vocab_streak_clear(vocab_streak_t *streak)
{
    if (streak) memset(streak, 0, sizeof(*streak));
}

bool vocab_streak_claim(vocab_streak_t *streak, vocab_index_t index, const vocab_record_t *pre)
{
    if (!streak || !pre || index == VOCAB_INDEX_NONE) return false;
    uint8_t count = claim_count(streak);
    if (count == VOCAB_SPOT_CHECK_THRESHOLD) {
        // The caller did not clear a full streak: drop the oldest claim.
        memmove(&streak->claims[0], &streak->claims[1],
                (VOCAB_SPOT_CHECK_THRESHOLD - 1u) * sizeof(streak->claims[0]));
        count--;
    }
    vocab_claim_t *claim = &streak->claims[count];
    claim->index = index;
    claim->pre_mastery = pre->mastery;
    claim->pre_flags = pre->flags;
    claim->pre_due = pre->due;
    streak->count = (uint8_t)(count + 1u);
    return streak->count == VOCAB_SPOT_CHECK_THRESHOLD;
}

bool vocab_streak_pick_target(const vocab_streak_t *streak, vocab_rng_t *rng, uint8_t *out_slot)
{
    if (!streak || !out_slot) return false;
    const uint8_t count = claim_count(streak);
    if (count < 2u) return false;
    // Never the most recent claim, nor an earlier claim of that same word: its meaning was
    // on screen a moment ago, so checking it would only test short-term memory.
    const vocab_index_t latest = streak->claims[count - 1u].index;
    uint8_t candidates[VOCAB_SPOT_CHECK_THRESHOLD];
    uint8_t n = 0;
    for (uint8_t i = 0; i + 1u < count; i++) {
        if (streak->claims[i].index != latest) candidates[n++] = i;
    }
    if (n == 0) return false;
    const uint32_t pick = rng ? vocab_rng_below(rng, n) : 0u;
    *out_slot = candidates[pick];
    return true;
}

void vocab_streak_pass(vocab_streak_t *streak)
{
    vocab_streak_clear(streak);
}

void vocab_streak_fail(vocab_streak_t *streak, uint8_t target_slot, vocab_record_t *records,
                       vocab_index_t record_count, vocab_tick_t tick)
{
    if (!streak) return;
    const uint8_t count = claim_count(streak);
    if (records) {
        const vocab_tick_t next = vocab_tick_add(tick, 1u);
        const bool has_target = target_slot < count;
        const vocab_index_t target =
            has_target ? streak->claims[target_slot].index : (vocab_index_t)VOCAB_INDEX_NONE;

        // Void every other claim, newest to oldest, so that a word claimed twice ends with
        // its earliest snapshot (the state before the whole streak).
        for (uint8_t i = count; i-- > 0u;) {
            const vocab_claim_t *claim = &streak->claims[i];
            if (claim->index >= record_count || (has_target && claim->index == target)) continue;
            vocab_record_t *rec = &records[claim->index];
            rec->mastery = clamp_mastery(claim->pre_mastery);
            rec->due = claim->pre_due < next ? claim->pre_due : next;
            restore_ever_mastered(rec, claim->pre_flags);
        }

        // Demote the target from its state before the streak.
        if (has_target && target < record_count) {
            const vocab_claim_t *claim = &streak->claims[first_slot_of(streak, count, target)];
            vocab_record_t *rec = &records[target];
            const uint8_t pre = clamp_mastery(claim->pre_mastery);
            rec->mastery = pre < 1u ? pre : (uint8_t)1u;
            rec->due = next;
            restore_ever_mastered(rec, claim->pre_flags);
            if (rec->lapses < UINT8_MAX) rec->lapses++;
        }
    }
    vocab_streak_clear(streak);
}
