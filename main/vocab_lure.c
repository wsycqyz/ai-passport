// main/vocab_lure.c - Stage 1 lure (distractor meaning) selection (spec 5.2); see vocab_lure.h.
//
// Pure: no allocation, no static mutable state, randomness only from the injected RNG.
// Owner: Stage 1 developer.
#include "vocab_lure.h"

#include <stddef.h>
#include <string.h>

#include "vocab_bank.h"

// One preference tier of spec 5.2. Tiers are tried in order and the first non-empty one is
// the preferred set, so the category rule (steps 1 and 3) dominates the kind preference
// (step 2): the same category is allowed only when no other-category candidate exists.
typedef struct {
    bool same_category;  // step 3: the target's own category is allowed
    bool same_kind;      // step 2: only candidates of the target's kind
} lure_tier_t;

static const lure_tier_t LURE_TIERS[] = {
    { false, true },   // steps 1-2: other category, same kind
    { false, false },  // step 2 fallback: other category, any kind
    { true, true },    // step 3: same category allowed, same kind preferred
    { true, false },   // step 3 fallback: same category allowed, any kind
};

bool vocab_lure_acceptable(const vocab_entry_t *target, const vocab_entry_t *candidate,
                           bool allow_same_category)
{
    if (!target || !candidate) return false;
    if (target->uid == candidate->uid) return false;
    if (!allow_same_category && target->category == candidate->category) return false;
    const char *a = target->zh;
    const char *b = candidate->zh;
    if (!a || !b || a[0] == '\0' || b[0] == '\0') return false;
    if (strcmp(a, b) == 0) return false;
    // Byte-wise search is exact for valid UTF-8: a match can only start on a character
    // boundary, because lead bytes and continuation bytes never coincide.
    return strstr(a, b) == NULL && strstr(b, a) == NULL;
}

static bool in_tier(const vocab_entry_t *target, vocab_kind_t kind,
                    const vocab_entry_t *candidate, const lure_tier_t *tier)
{
    if (!vocab_lure_acceptable(target, candidate, tier->same_category)) return false;
    return !tier->same_kind || vocab_entry_kind(candidate) == kind;
}

bool vocab_lure_pick(const vocab_bank_t *bank, vocab_index_t target, vocab_index_t avoid,
                     vocab_rng_t *rng, vocab_index_t *out_lure)
{
    if (!bank || !bank->entries || target >= bank->count || !rng || !out_lure) return false;
    const vocab_entry_t *t = &bank->entries[target];
    const vocab_kind_t kind = vocab_entry_kind(t);

    for (size_t k = 0; k < sizeof(LURE_TIERS) / sizeof(LURE_TIERS[0]); k++) {
        const lure_tier_t *tier = &LURE_TIERS[k];
        uint32_t members = 0;
        bool avoid_member = false;
        for (vocab_index_t i = 0; i < bank->count; i++) {
            if (!in_tier(t, kind, &bank->entries[i], tier)) continue;
            members++;
            if (i == avoid) avoid_member = true;
        }
        if (members == 0) continue;

        // Step 4: skip the lure this word already used while another candidate remains.
        const bool skip_avoid = avoid_member && members > 1u;
        uint32_t pick = vocab_rng_below(rng, skip_avoid ? members - 1u : members);
        for (vocab_index_t i = 0; i < bank->count; i++) {
            if (!in_tier(t, kind, &bank->entries[i], tier)) continue;
            if (skip_avoid && i == avoid) continue;
            if (pick == 0) {
                *out_lure = i;
                return true;
            }
            pick--;
        }
        return false;  // not reached: the second pass visits the same members
    }
    return false;
}
