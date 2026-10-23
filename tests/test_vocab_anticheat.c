// tests/test_vocab_anticheat.c - "I know" streak, spot-check target and rollback.
//
// Spec 4.3 rows O3 (claim recorded before the promotion), O4 (forgot clears the streak),
// O10 (pass), O11 (full-streak rollback), 4.5 (the spot check is the next card), 4.6
// (spot-check format), 4.8 (streak, trigger at 3, target excludes the most recent claim,
// resets), 4.9 (a voided claim voids its mastery bonus); AC-19 (target rule and format) and
// AC-20 (rollback, pass, resets). Includes a mini session that follows the call sequence of
// vocab_anticheat.h, an end-to-end run with real Stage 2 cards, and a randomized
// model-based test against an independent reference of the rollback rule.
//
// Compiled by tools/validate.sh with:
//   tests/test_vocab_anticheat.c main/vocab_anticheat.c main/vocab_stage2.c
//   main/vocab_bank.c main/vocab_rng.c main/vocab_view.c
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vocab_anticheat.h"
#include "vocab_bank.h"
#include "vocab_config.h"
#include "vocab_stage2.h"
#include "vocab_test.h"

_Static_assert(VOCAB_SPOT_CHECK_THRESHOLD == 3u, "spec 4.8: three claims trigger a spot check");

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define SEEN VOCAB_REC_SEEN
#define EVER VOCAB_REC_EVER_MASTERED

static const uint8_t INTERVALS[VOCAB_MASTERY_MAX + 1u] = VOCAB_INTERVALS_INIT;

static vocab_record_t rec(vocab_uid_t uid, uint8_t mastery, vocab_tick_t due, uint8_t flags,
                          uint8_t lapses)
{
    vocab_record_t r;
    memset(&r, 0, sizeof(r));
    r.uid = uid;
    r.mastery = mastery;
    r.due = due;
    r.flags = flags;
    r.lapses = lapses;
    return r;
}

static bool rec_same(const vocab_record_t *a, const vocab_record_t *b)
{
    return a->uid == b->uid && a->due == b->due && a->mastery == b->mastery &&
           a->flags == b->flags && a->lapses == b->lapses && a->reserved == b->reserved;
}

static const char *rec_str(const vocab_record_t *r)
{
    static char buf[4][80];
    static int n;
    char *s = buf[n++ & 3];
    snprintf(s, sizeof(buf[0]), "{uid %u m %u due %u flags %02X lapses %u}", (unsigned)r->uid,
             (unsigned)r->mastery, (unsigned)r->due, (unsigned)r->flags, (unsigned)r->lapses);
    return s;
}

// Spec 4.3 O3 exactly as the session applies it after the claim (vocab_srs.c is not linked
// into this test): m' = min(m + 1, 5), due = T + INTERVAL[m'], seen, ever_mastered at 5.
static void promote_o3(vocab_record_t *r, vocab_tick_t tick)
{
    const uint8_t m = r->mastery >= VOCAB_MASTERY_MAX ? (uint8_t)VOCAB_MASTERY_MAX
                                                      : (uint8_t)(r->mastery + 1u);
    r->mastery = m;
    r->due = vocab_tick_add(tick, INTERVALS[m]);
    r->flags |= SEEN;
    if (m == VOCAB_MASTERY_MAX) r->flags |= EVER;
}

// Spec 4.3 O4: m' = min(m, 1), due = T + 1, lapses + 1.
static void forgot_o4(vocab_record_t *r, vocab_tick_t tick)
{
    if (r->mastery > 1u) r->mastery = 1u;
    r->due = vocab_tick_add(tick, 1u);
    r->flags |= SEEN;
    if (r->lapses < 255u) r->lapses++;
}

static uint8_t min_u8(uint8_t a, uint8_t b)
{
    return a < b ? a : b;
}

// =======================================================================================
// Claims
// =======================================================================================

static void test_claims(void)
{
    vocab_streak_t s;
    memset(&s, 0xA5, sizeof(s));
    vocab_streak_clear(&s);
    CHECK(s.count == 0, "clear empties the streak");

    const vocab_record_t a = rec(101, 1, 7, SEEN, 2);
    const vocab_record_t b = rec(202, 4, 65535, SEEN | EVER, 0);
    const vocab_record_t c = rec(303, 0, 0, 0, 9);
    CHECK(!vocab_streak_claim(&s, 3, &a) && s.count == 1, "claim 1 does not trigger");
    CHECK(!vocab_streak_claim(&s, 9, &b) && s.count == 2, "claim 2 does not trigger");
    CHECK(vocab_streak_claim(&s, 12, &c) && s.count == 3, "claim 3 triggers the spot check");
    const vocab_claim_t want[3] = {
        { 3, 1, SEEN, 7 }, { 9, 4, SEEN | EVER, 65535 }, { 12, 0, 0, 0 },
    };
    for (int i = 0; i < 3; i++) {
        const vocab_claim_t *k = &s.claims[i];
        CHECK(k->index == want[i].index && k->pre_mastery == want[i].pre_mastery &&
                  k->pre_flags == want[i].pre_flags && k->pre_due == want[i].pre_due,
              "claim %d keeps the exact pre-claim snapshot", i);
    }

    // A full streak that was not cleared drops its oldest claim (header contract).
    const vocab_record_t d = rec(404, 2, 30, SEEN, 0);
    CHECK(vocab_streak_claim(&s, 20, &d) && s.count == 3, "claim on a full streak triggers again");
    CHECK(s.claims[0].index == 9 && s.claims[1].index == 12 && s.claims[2].index == 20,
          "oldest claim dropped, order kept");
    CHECK(s.claims[2].pre_mastery == 2 && s.claims[2].pre_due == 30, "new snapshot appended");

    // Rejected input records nothing.
    vocab_streak_clear(&s);
    CHECK(!vocab_streak_claim(&s, 1, NULL) && s.count == 0, "NULL record rejected");
    CHECK(!vocab_streak_claim(&s, VOCAB_INDEX_NONE, &a) && s.count == 0, "index NONE rejected");
    CHECK(!vocab_streak_claim(NULL, 1, &a), "NULL streak rejected");

    // A corrupted count never indexes past the array.
    s.count = 200;
    CHECK(vocab_streak_claim(&s, 5, &a) && s.count == 3 && s.claims[2].index == 5,
          "corrupted count is clamped");

    // NULL arguments never crash.
    vocab_streak_clear(NULL);
    vocab_streak_pass(NULL);
    vocab_streak_fail(NULL, 0, NULL, 0, 0);
    uint8_t slot = 0;
    vocab_rng_t rng;
    vocab_rng_seed(&rng, 1);
    CHECK(!vocab_streak_pick_target(NULL, &rng, &slot), "NULL streak: no target");
}

// =======================================================================================
// Target choice (spec 4.8, AC-19)
// =======================================================================================

static void fill(vocab_streak_t *s, const vocab_index_t *indices, int n)
{
    vocab_streak_clear(s);
    for (int i = 0; i < n; i++) {
        const vocab_record_t r = rec((vocab_uid_t)(100u + indices[i]), 1, 1, SEEN, 0);
        vocab_streak_claim(s, indices[i], &r);
    }
}

static void test_pick_target(void)
{
    vocab_streak_t s;
    vocab_rng_t rng;
    uint8_t slot = 77;
    vocab_rng_seed(&rng, 5);

    vocab_streak_clear(&s);
    CHECK(!vocab_streak_pick_target(&s, &rng, &slot) && slot == 77, "no claims: no target");
    const vocab_index_t one[] = { 4 };
    fill(&s, one, 1);
    CHECK(!vocab_streak_pick_target(&s, &rng, &slot), "one claim: nothing but the most recent");
    const vocab_index_t two[] = { 4, 8 };
    fill(&s, two, 2);
    CHECK(!vocab_streak_pick_target(&s, &rng, NULL), "NULL out_slot");
    const uint64_t before = rng.state;
    CHECK(vocab_streak_pick_target(&s, &rng, &slot) && slot == 0, "two claims: the older one");
    CHECK(rng.state == before, "a single candidate needs no RNG draw");

    // Three claims (the trigger): uniform over the first two, never the most recent; exactly
    // one vocab_rng_below(rng, 2) draw (documented for system-simulation determinism).
    const vocab_index_t three[] = { 4, 8, 15 };
    fill(&s, three, 3);
    unsigned tally[3] = { 0, 0, 0 };
    for (uint32_t seed = 1; seed <= 10000u; seed++) {
        vocab_rng_seed(&rng, seed);
        vocab_rng_t copy = rng;
        const uint32_t expected = vocab_rng_below(&copy, 2u);
        slot = 99;
        const bool ok = vocab_streak_pick_target(&s, &rng, &slot);
        CHECK(ok && slot < 3, "seed %u: a target", (unsigned)seed);
        if (!ok || slot >= 3) break;
        CHECK(slot == expected && rng.state == copy.state, "seed %u: one below(2) draw", (unsigned)seed);
        tally[slot]++;
    }
    CHECK(tally[2] == 0, "the most recent claim is never the target");
    CHECK(tally[0] >= 4700 && tally[0] <= 5300 && tally[1] >= 4700 && tally[1] <= 5300,
          "uniform over 10000 seeds: %u / %u", tally[0], tally[1]);
    printf("spot-check target over seeds 1..10000: slot0 %u, slot1 %u, slot2 %u\n", tally[0],
           tally[1], tally[2]);

    // One generator, many picks.
    vocab_rng_seed(&rng, 99);
    unsigned seq[3] = { 0, 0, 0 };
    for (int i = 0; i < 100000; i++) {
        if (vocab_streak_pick_target(&s, &rng, &slot) && slot < 3) seq[slot]++;
    }
    CHECK(seq[2] == 0 && seq[0] >= 49000 && seq[0] <= 51000 && seq[0] + seq[1] == 100000u,
          "uniform over 100000 picks: %u / %u / %u", seq[0], seq[1], seq[2]);

    // Without an RNG the oldest candidate is used.
    CHECK(vocab_streak_pick_target(&s, NULL, &slot) && slot == 0, "NULL rng: oldest candidate");

    // The streak is not modified by picking.
    CHECK(s.count == 3 && s.claims[0].index == 4 && s.claims[2].index == 15, "pick is read-only");

    // A word claimed again as the most recent claim is excluded entirely.
    const vocab_index_t dup_last[] = { 4, 8, 4 };
    fill(&s, dup_last, 3);
    for (uint32_t seed = 1; seed <= 200u; seed++) {
        vocab_rng_seed(&rng, seed);
        CHECK(vocab_streak_pick_target(&s, &rng, &slot) && slot == 1,
              "the other word is the only candidate");
    }
    const vocab_index_t same[] = { 4, 4, 4 };
    fill(&s, same, 3);
    CHECK(!vocab_streak_pick_target(&s, &rng, &slot), "only the most recent word: no target");
    const vocab_index_t dup_old[] = { 4, 4, 8 };
    fill(&s, dup_old, 3);
    CHECK(vocab_streak_pick_target(&s, &rng, &slot) && slot < 2, "earlier duplicates are candidates");
}

// Spec 4.6: the claim snapshot carries what the session needs to choose the spot-check
// format (Stage 3 if the target has cloze data and pre-claim m >= 2, else Stage 1).
static vocab_format_t spot_format(const vocab_streak_t *s, uint8_t slot, const vocab_bank_t *bank)
{
    const vocab_claim_t *target = &s->claims[slot];
    const vocab_entry_t *e = vocab_bank_at(bank, target->index);
    return e && vocab_entry_has_cloze(e) && target->pre_mastery >= 2u ? VOCAB_FORMAT_S3
                                                                      : VOCAB_FORMAT_S1;
}

static void test_spot_format_data(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    vocab_index_t cat = 0;
    vocab_index_t pear = 0;
    CHECK(vocab_bank_find(bank, 201, &cat) && vocab_bank_find(bank, 103, &pear), "bank uids");
    vocab_rng_t rng;
    vocab_rng_seed(&rng, 8);
    for (uint8_t m = 0; m <= VOCAB_MASTERY_MAX; m++) {
        vocab_streak_t s;
        vocab_streak_clear(&s);
        vocab_record_t rc = rec(201, m, 3, SEEN, 0);
        vocab_record_t rp = rec(103, m, 3, SEEN, 0);
        vocab_record_t rx = rec(101, 1, 3, SEEN, 0);
        vocab_streak_claim(&s, cat, &rc);
        promote_o3(&rc, 10);  // the record moves on; the snapshot keeps the pre-claim m
        vocab_streak_claim(&s, pear, &rp);
        promote_o3(&rp, 10);
        CHECK(vocab_streak_claim(&s, 0, &rx), "third claim");
        CHECK(spot_format(&s, 0, bank) == (m >= 2u ? VOCAB_FORMAT_S3 : VOCAB_FORMAT_S1),
              "cat (cloze) at pre-claim m %u", (unsigned)m);
        CHECK(spot_format(&s, 1, bank) == VOCAB_FORMAT_S1, "pear (no cloze) at pre-claim m %u",
              (unsigned)m);
        uint8_t slot = 9;
        CHECK(vocab_streak_pick_target(&s, &rng, &slot) && slot < 2 &&
                  (s.claims[slot].index == cat || s.claims[slot].index == pear),
              "the target is a claimed word");
    }
}

// =======================================================================================
// Pass (O10) and fail (O11)
// =======================================================================================

#define WORDS 8

static void test_pass(void)
{
    vocab_record_t records[WORDS];
    for (int i = 0; i < WORDS; i++) records[i] = rec((vocab_uid_t)(10 + i), 1, 2, SEEN, 0);
    vocab_streak_t s;
    vocab_streak_clear(&s);
    const vocab_tick_t T = 9;
    const vocab_index_t idx[] = { 1, 4, 6 };
    for (int i = 0; i < 3; i++) {
        vocab_streak_claim(&s, idx[i], &records[idx[i]]);
        promote_o3(&records[idx[i]], T);
    }
    vocab_record_t after[WORDS];
    memcpy(after, records, sizeof(after));
    vocab_streak_pass(&s);
    CHECK(s.count == 0, "pass clears the streak");
    CHECK(memcmp(after, records, sizeof(after)) == 0, "pass leaves every claim standing");
    CHECK(records[1].mastery == 2 && records[1].due == T + 2, "claims keep their promotion");
    const vocab_record_t r = rec(1, 1, 1, SEEN, 0);
    CHECK(!vocab_streak_claim(&s, 2, &r) && s.count == 1, "a new streak starts after a pass");
}

// Expected O11 result for one claim given its pre-claim record (spec 4.3, header contract).
static vocab_record_t expect_rolled_back(const vocab_record_t *pre, const vocab_record_t *now,
                                         bool target, vocab_tick_t tick)
{
    vocab_record_t r = *now;
    const vocab_tick_t next = vocab_tick_add(tick, 1u);
    const uint8_t pre_m = min_u8(pre->mastery, (uint8_t)VOCAB_MASTERY_MAX);
    r.flags = (uint8_t)((now->flags & ~EVER) | (pre->flags & EVER));
    if (target) {
        r.mastery = min_u8(pre_m, 1u);
        r.due = next;
        r.lapses = now->lapses < 255u ? (uint8_t)(now->lapses + 1u) : (uint8_t)255u;
    } else {
        r.mastery = pre_m;
        r.due = pre->due < next ? pre->due : next;
    }
    return r;
}

// Every combination of pre-claim mastery for the three claims, both target slots, varied
// dues / flags / lapses: the target is demoted and every other claim is restored exactly.
static void test_fail_exhaustive(void)
{
    static const vocab_tick_t dues[] = { 0, 1, 39, 40, 41, 42, 45, 300, 65535 };
    static const uint8_t lapses[] = { 0, 1, 7, 254, 255 };
    const vocab_tick_t T = 40;
    const vocab_index_t idx[3] = { 2, 5, 6 };
    vocab_rng_t pick;
    vocab_rng_seed(&pick, 77);
    unsigned cases = 0;
    for (uint8_t target_slot = 0; target_slot < 2; target_slot++) {
        for (unsigned combo = 0; combo < 6u * 6u * 6u; combo++) {
            const uint8_t ms[3] = { (uint8_t)(combo % 6u), (uint8_t)(combo / 6u % 6u),
                                    (uint8_t)(combo / 36u) };
            vocab_record_t records[WORDS];
            vocab_record_t pre[3];
            for (int i = 0; i < WORDS; i++) {
                records[i] = rec((vocab_uid_t)(500 + i), (uint8_t)(i % 6), (vocab_tick_t)(3 * i),
                                 SEEN, (uint8_t)i);
                records[i].reserved = (uint8_t)(0x40 + i);
            }
            vocab_streak_t s;
            vocab_streak_clear(&s);
            for (int k = 0; k < 3; k++) {
                vocab_record_t *r = &records[idx[k]];
                r->mastery = ms[k];
                r->due = dues[vocab_rng_below(&pick, ARRAY_LEN(dues))];
                r->lapses = lapses[vocab_rng_below(&pick, ARRAY_LEN(lapses))];
                r->flags = (uint8_t)((vocab_rng_bit(&pick) ? SEEN : 0u) |
                                     (ms[k] == VOCAB_MASTERY_MAX || vocab_rng_bit(&pick) ? EVER : 0u));
                pre[k] = *r;
                const bool trig = vocab_streak_claim(&s, idx[k], r);  // BEFORE the promotion
                CHECK(trig == (k == 2), "trigger only on the third claim");
                promote_o3(r, T);
            }
            vocab_record_t before[WORDS];
            memcpy(before, records, sizeof(before));
            vocab_record_t want[WORDS];
            memcpy(want, records, sizeof(want));
            for (int k = 0; k < 3; k++) {
                want[idx[k]] = expect_rolled_back(&pre[k], &before[idx[k]], k == target_slot, T);
            }
            vocab_streak_fail(&s, target_slot, records, WORDS, T);
            CHECK(s.count == 0, "fail clears the streak");
            for (int i = 0; i < WORDS; i++) {
                CHECK(rec_same(&records[i], &want[i]), "slot %u combo %u word %d: %s (want %s)",
                      (unsigned)target_slot, combo, i, rec_str(&records[i]), rec_str(&want[i]));
            }
            for (int k = 0; k < 3; k++) {
                const vocab_record_t *r = &records[idx[k]];
                CHECK(r->due <= T + 1u, "AC-20: every claim is due by the next session");
                CHECK((r->flags & SEEN) == ((pre[k].flags | SEEN) & SEEN), "SEEN stays set");
                if (k == target_slot) {
                    CHECK(r->mastery <= 1u && r->mastery == min_u8(pre[k].mastery, 1u),
                          "the target is demoted");
                } else {
                    CHECK(r->mastery == pre[k].mastery && (r->flags & EVER) == (pre[k].flags & EVER),
                          "every other claim restored to its pre-claim state");
                }
            }
            cases++;
        }
    }
    CHECK(cases == 432u, "rollback cases run: %u", cases);
}

static void test_fail_edges(void)
{
    vocab_record_t records[WORDS];
    vocab_streak_t s;
    const vocab_index_t idx[3] = { 0, 3, 7 };

    // Tick saturation (spec 4.2): due arithmetic saturates at 65535.
    const vocab_tick_t ticks[] = { 65535, 65534 };
    for (size_t t = 0; t < ARRAY_LEN(ticks); t++) {
        for (int i = 0; i < WORDS; i++) records[i] = rec((vocab_uid_t)(i + 1), 2, 65535, SEEN, 0);
        vocab_streak_clear(&s);
        for (int k = 0; k < 3; k++) {
            vocab_streak_claim(&s, idx[k], &records[idx[k]]);
            promote_o3(&records[idx[k]], ticks[t]);
        }
        vocab_streak_fail(&s, 1, records, WORDS, ticks[t]);
        CHECK(records[3].due == 65535 && records[3].mastery == 1, "target due saturates");
        CHECK(records[0].due == 65535 && records[0].mastery == 2, "others: min(pre, T+1) saturated");
    }

    // Lapses saturate at 255; only the target gets a lapse.
    for (int i = 0; i < WORDS; i++) records[i] = rec((vocab_uid_t)(i + 1), 3, 5, SEEN, 255);
    records[3].lapses = 254;
    vocab_streak_clear(&s);
    for (int k = 0; k < 3; k++) {
        vocab_streak_claim(&s, idx[k], &records[idx[k]]);
        promote_o3(&records[idx[k]], 10);
    }
    vocab_streak_fail(&s, 1, records, WORDS, 10);
    CHECK(records[3].lapses == 255 && records[0].lapses == 255 && records[7].lapses == 255,
          "lapses saturate");
    for (int i = 0; i < WORDS; i++) records[i] = rec((vocab_uid_t)(i + 1), 3, 5, SEEN, 255);
    vocab_streak_clear(&s);
    for (int k = 0; k < 3; k++) {
        vocab_streak_claim(&s, idx[k], &records[idx[k]]);
        promote_o3(&records[idx[k]], 10);
    }
    vocab_streak_fail(&s, 2, records, WORDS, 10);
    CHECK(records[7].lapses == 255, "lapses stay at 255");

    // An invalid target slot only rolls back the others (no demotion, no lapse).
    const uint8_t bad_slots[] = { 3, 200, 255 };
    for (size_t b = 0; b < ARRAY_LEN(bad_slots); b++) {
        for (int i = 0; i < WORDS; i++) records[i] = rec((vocab_uid_t)(i + 1), 2, 50, SEEN, 1);
        vocab_streak_clear(&s);
        for (int k = 0; k < 3; k++) {
            vocab_streak_claim(&s, idx[k], &records[idx[k]]);
            promote_o3(&records[idx[k]], 20);
        }
        vocab_streak_fail(&s, bad_slots[b], records, WORDS, 20);
        for (int k = 0; k < 3; k++) {
            const vocab_record_t *r = &records[idx[k]];
            CHECK(r->mastery == 2 && r->due == 21 && r->lapses == 1,
                  "invalid slot %u: claim %d restored, not demoted", (unsigned)bad_slots[b], k);
        }
        CHECK(s.count == 0, "invalid slot still clears");
    }

    // Claims outside the records are skipped; the rest is rolled back.
    for (int i = 0; i < WORDS; i++) records[i] = rec((vocab_uid_t)(i + 1), 1, 4, SEEN, 0);
    vocab_streak_clear(&s);
    const vocab_record_t outside = rec(99, 0, 0, SEEN, 0);
    vocab_streak_claim(&s, 1, &records[1]);
    promote_o3(&records[1], 30);
    vocab_streak_claim(&s, 50, &outside);
    vocab_streak_claim(&s, 2, &records[2]);
    promote_o3(&records[2], 30);
    vocab_record_t snapshot[WORDS];
    memcpy(snapshot, records, sizeof(snapshot));
    vocab_streak_fail(&s, 1, records, WORDS, 30);  // the target is the out-of-range claim
    CHECK(records[1].mastery == 1 && records[1].due == 4 && records[2].mastery == 1,
          "in-range claims restored when the target is out of range");
    for (int i = 3; i < WORDS; i++) CHECK(rec_same(&records[i], &snapshot[i]), "unrelated word %d", i);

    // No records: only the streak is cleared. Zero records: nothing changes.
    vocab_streak_clear(&s);
    vocab_streak_claim(&s, 1, &records[1]);
    vocab_streak_fail(&s, 0, NULL, WORDS, 30);
    CHECK(s.count == 0, "NULL records: streak cleared");
    memcpy(snapshot, records, sizeof(snapshot));
    vocab_streak_claim(&s, 1, &records[1]);
    records[1].mastery = 5;
    snapshot[1].mastery = 5;
    vocab_streak_fail(&s, 0, records, 0, 30);
    CHECK(memcmp(snapshot, records, sizeof(snapshot)) == 0, "record_count 0: nothing changes");

    // A pre-claim mastery outside 0..5 is clamped on restore.
    vocab_streak_clear(&s);
    const vocab_record_t odd = rec(1, 9, 3, SEEN, 0);
    vocab_streak_claim(&s, 4, &odd);
    vocab_streak_claim(&s, 5, &records[5]);
    vocab_streak_fail(&s, 1, records, WORDS, 30);
    CHECK(records[4].mastery == VOCAB_MASTERY_MAX, "restored mastery clamped to 5");
}

// A word claimed twice (not produced by the session): the earliest snapshot wins.
static void test_fail_duplicates(void)
{
    const vocab_tick_t T = 12;
    vocab_record_t records[WORDS];
    for (int i = 0; i < WORDS; i++) records[i] = rec((vocab_uid_t)(i + 1), 1, 5, SEEN, 0);
    records[2] = rec(3, 4, 9, SEEN, 3);
    vocab_streak_t s;
    vocab_streak_clear(&s);
    vocab_streak_claim(&s, 1, &records[1]);  // A at m 1
    promote_o3(&records[1], T);
    vocab_streak_claim(&s, 2, &records[2]);  // B at m 4 -> 5 (new mastery)
    promote_o3(&records[2], T);
    vocab_streak_claim(&s, 1, &records[1]);  // A again at m 2
    promote_o3(&records[1], T);
    CHECK(records[1].mastery == 3 && records[2].mastery == 5, "two promotions applied");
    vocab_rng_t rng;
    vocab_rng_seed(&rng, 3);
    uint8_t slot = 9;
    CHECK(vocab_streak_pick_target(&s, &rng, &slot) && slot == 1, "target: B (A was just shown)");

    vocab_streak_t copy = s;
    vocab_record_t saved[WORDS];
    memcpy(saved, records, sizeof(saved));
    vocab_streak_fail(&s, 1, records, WORDS, T);
    CHECK(records[1].mastery == 1 && records[1].due == 5 && records[1].lapses == 0,
          "A restored to its state before the streak: %s", rec_str(&records[1]));
    CHECK(records[2].mastery == 1 && records[2].due == T + 1 && records[2].lapses == 4 &&
              !(records[2].flags & EVER),
          "B demoted with its mastery bonus voided: %s", rec_str(&records[2]));

    // If a caller targets the duplicated word, it is demoted once from its earliest snapshot.
    memcpy(records, saved, sizeof(saved));
    vocab_streak_fail(&copy, 2, records, WORDS, T);
    CHECK(records[1].mastery == 1 && records[1].due == T + 1 && records[1].lapses == 1,
          "duplicated target demoted once: %s", rec_str(&records[1]));
    CHECK(records[2].mastery == 4 && records[2].due == 9 && records[2].lapses == 3 &&
              !(records[2].flags & EVER),
          "other claim restored: %s", rec_str(&records[2]));

    // A word claimed twice whose second promotion reaches mastery 5: one bonus, not two.
    for (int i = 0; i < WORDS; i++) records[i] = rec((vocab_uid_t)(i + 1), 1, 5, SEEN, 0);
    records[4] = rec(5, 3, 2, SEEN, 0);
    vocab_streak_clear(&s);
    vocab_streak_claim(&s, 4, &records[4]);  // 3 -> 4
    promote_o3(&records[4], T);
    vocab_streak_claim(&s, 6, &records[6]);
    promote_o3(&records[6], T);
    vocab_streak_claim(&s, 4, &records[4]);  // 4 -> 5: first mastery
    promote_o3(&records[4], T);
    CHECK(records[4].mastery == 5 && (records[4].flags & EVER), "mastered by the second claim");
    vocab_streak_fail(&s, 1, records, WORDS, T);
    CHECK(records[4].mastery == 3 && !(records[4].flags & EVER) && records[4].due == 2,
          "both claims voided: %s", rec_str(&records[4]));

    // A duplicated target is demoted from its earliest snapshot, even via its later slot.
    for (int i = 0; i < WORDS; i++) records[i] = rec((vocab_uid_t)(i + 1), 1, 5, SEEN, 0);
    records[3] = rec(4, 4, 7, SEEN, 0);
    records[5] = rec(6, 0, 0, 0, 0);
    vocab_streak_clear(&s);
    vocab_streak_claim(&s, 3, &records[3]);  // 4 -> 5 sets EVER
    promote_o3(&records[3], T);
    vocab_streak_claim(&s, 1, &records[1]);
    promote_o3(&records[1], T);
    vocab_streak_claim(&s, 3, &records[3]);  // snapshot already has EVER
    promote_o3(&records[3], T);
    vocab_streak_fail(&s, 2, records, WORDS, T);
    CHECK(records[3].mastery == 1 && !(records[3].flags & EVER) && records[3].lapses == 1,
          "earliest snapshot voids the first mastery: %s", rec_str(&records[3]));
    vocab_streak_clear(&s);
    vocab_streak_claim(&s, 5, &records[5]);  // unseen word (Stage 2 without a lure): 0 -> 1
    promote_o3(&records[5], T);
    vocab_streak_claim(&s, 1, &records[1]);
    promote_o3(&records[1], T);
    vocab_streak_claim(&s, 5, &records[5]);  // 1 -> 2
    promote_o3(&records[5], T);
    vocab_streak_fail(&s, 2, records, WORDS, T);
    CHECK(records[5].mastery == 0 && records[5].due == T + 1 && (records[5].flags & SEEN),
          "demoted to min(earliest pre-claim, 1) = 0: %s", rec_str(&records[5]));
}

// =======================================================================================
// Mini session following the vocab_anticheat.h call sequence (AC-19, AC-20)
// =======================================================================================

typedef struct {
    const vocab_bank_t *bank;
    vocab_record_t records[64];
    bool base_ever[64];         // EVER_MASTERED at session start (SUMMARY XP, spec 4.9)
    vocab_streak_t streak;
    vocab_tick_t tick;
    vocab_rng_t rng;
    bool spot_pending;          // a SPOT_CHECK card is the next card
    uint8_t spot_slot;
    vocab_index_t spot_index;
    vocab_format_t spot_format;
} mini_t;

static void mini_init(mini_t *m, uint32_t seed)
{
    memset(m, 0, sizeof(*m));
    m->bank = vocab_bank_builtin();
    for (vocab_index_t i = 0; i < m->bank->count; i++) m->records[i].uid = m->bank->entries[i].uid;
    vocab_rng_seed(&m->rng, seed);
    m->tick = 10;
}

static vocab_index_t uid_index(const mini_t *m, vocab_uid_t uid)
{
    vocab_index_t index = VOCAB_INDEX_NONE;
    CHECK(vocab_bank_find(m->bank, uid, &index), "uid %u", (unsigned)uid);
    return index;
}

static void mini_session_start(mini_t *m)
{
    vocab_streak_clear(&m->streak);
    m->spot_pending = false;
    for (vocab_index_t i = 0; i < m->bank->count; i++) {
        m->base_ever[i] = (m->records[i].flags & EVER) != 0;
    }
}

// Words that reached mastery 5 for the first time this session (the SUMMARY bonus).
static unsigned mini_new_mastered(const mini_t *m)
{
    unsigned n = 0;
    for (vocab_index_t i = 0; i < m->bank->count; i++) {
        if (!m->base_ever[i] && (m->records[i].flags & EVER)) n++;
    }
    return n;
}

// Stage 2 KNOW (O3): claim with the pre-claim record, then promote.
static bool mini_know(mini_t *m, vocab_uid_t uid)
{
    CHECK(!m->spot_pending, "a pending spot check is always the very next card");
    const vocab_index_t i = uid_index(m, uid);
    const bool trigger = vocab_streak_claim(&m->streak, i, &m->records[i]);
    promote_o3(&m->records[i], m->tick);
    if (trigger) {
        CHECK(vocab_streak_pick_target(&m->streak, &m->rng, &m->spot_slot), "target chosen");
        m->spot_index = m->streak.claims[m->spot_slot].index;
        m->spot_format = spot_format(&m->streak, m->spot_slot, m->bank);
        m->spot_pending = true;
    }
    return trigger;
}

// Stage 2 FORGOT (O4): the streak is cleared.
static void mini_forgot(mini_t *m, vocab_uid_t uid)
{
    const vocab_index_t i = uid_index(m, uid);
    vocab_streak_clear(&m->streak);
    forgot_o4(&m->records[i], m->tick);
}

// A Stage 1 / Stage 3 card of another word: the streak is not touched.
static void mini_other_card(mini_t *m, vocab_uid_t uid)
{
    const vocab_index_t i = uid_index(m, uid);
    m->records[i].mastery = 1;
    m->records[i].due = vocab_tick_add(m->tick, 1u);
    m->records[i].flags |= SEEN;
}

static void mini_spot(mini_t *m, bool pass)
{
    CHECK(m->spot_pending, "a spot check is pending");
    if (pass) {
        vocab_streak_pass(&m->streak);
    } else {
        vocab_streak_fail(&m->streak, m->spot_slot, m->records, m->bank->count, m->tick);
    }
    m->spot_pending = false;
}

static void test_mini_session(void)
{
    mini_t m;
    // AC-19: the third consecutive claim triggers; Stage 1/3 cards in between do not reset.
    for (uint32_t seed = 1; seed <= 400u; seed++) {
        mini_init(&m, seed);
        mini_session_start(&m);
        m.records[uid_index(&m, 201)] = rec(201, 2, 10, SEEN, 0);   // cat, cloze, m 2
        m.records[uid_index(&m, 103)] = rec(103, 1, 9, SEEN, 0);    // pear, no cloze
        m.records[uid_index(&m, 303)] = rec(303, 3, 8, SEEN, 0);    // bike
        CHECK(!mini_know(&m, 201), "claim 1");
        mini_other_card(&m, 101);                                   // Stage 1 card
        CHECK(!mini_know(&m, 103), "claim 2");
        mini_other_card(&m, 204);                                   // Stage 3 card
        CHECK(mini_know(&m, 303), "claim 3 inserts the spot check as the next card");
        CHECK(m.spot_index == uid_index(&m, 201) || m.spot_index == uid_index(&m, 103),
              "the target is one of the first two claims");
        const vocab_format_t want = m.spot_index == uid_index(&m, 201) ? VOCAB_FORMAT_S3
                                                                       : VOCAB_FORMAT_S1;
        CHECK(m.spot_format == want, "spot-check format per spec 4.6");
        mini_spot(&m, true);
        CHECK(m.streak.count == 0, "any spot check resets the streak");
        CHECK(m.records[uid_index(&m, 201)].mastery == 3 && m.records[uid_index(&m, 103)].mastery == 2 &&
                  m.records[uid_index(&m, 303)].mastery == 4,
              "pass: every claim stands");
    }

    // AC-20: forgot resets the streak.
    mini_init(&m, 7);
    mini_session_start(&m);
    CHECK(!mini_know(&m, 101) && !mini_know(&m, 102), "two claims");
    mini_forgot(&m, 103);
    CHECK(m.streak.count == 0, "forgot clears the streak");
    CHECK(!mini_know(&m, 104) && !mini_know(&m, 201), "two claims after forgot do not trigger");
    CHECK(mini_know(&m, 202), "the third claim after forgot triggers");
    mini_spot(&m, true);

    // AC-20: fail demotes the target and restores the other claims (due <= T+1).
    mini_init(&m, 11);
    mini_session_start(&m);
    const vocab_record_t pre_a = rec(301, 4, 25, SEEN, 1);   // bus: 4 -> 5 (new mastery)
    const vocab_record_t pre_b = rec(302, 2, 14, SEEN, 0);   // car
    const vocab_record_t pre_c = rec(303, 1, 3, SEEN, 2);    // bike
    m.records[uid_index(&m, 301)] = pre_a;
    m.records[uid_index(&m, 302)] = pre_b;
    m.records[uid_index(&m, 303)] = pre_c;
    mini_know(&m, 301);
    mini_know(&m, 302);
    CHECK(mini_know(&m, 303), "trigger");
    CHECK(mini_new_mastered(&m) == 1, "the O3 promotion of bus reached mastery 5");
    const vocab_index_t target = m.spot_index;
    mini_spot(&m, false);
    CHECK(mini_new_mastered(&m) == 0, "the voided claim's mastery bonus is gone");
    const vocab_record_t *pres[3] = { &pre_a, &pre_b, &pre_c };
    const vocab_uid_t uids[3] = { 301, 302, 303 };
    for (int k = 0; k < 3; k++) {
        const vocab_record_t *r = &m.records[uid_index(&m, uids[k])];
        if (uid_index(&m, uids[k]) == target) {
            CHECK(r->mastery == min_u8(pres[k]->mastery, 1u) && r->due == m.tick + 1u &&
                      r->lapses == pres[k]->lapses + 1u,
                  "target demoted: %s", rec_str(r));
        } else {
            CHECK(r->mastery == pres[k]->mastery && r->due <= m.tick + 1u &&
                      r->due == (pres[k]->due < m.tick + 1u ? pres[k]->due : m.tick + 1u) &&
                      r->lapses == pres[k]->lapses && (r->flags & EVER) == (pres[k]->flags & EVER),
                  "claim restored: %s", rec_str(r));
        }
    }
    CHECK(m.streak.count == 0, "fail resets the streak");

    // Session start resets the streak; claims left at SUMMARY stand.
    mini_init(&m, 13);
    mini_session_start(&m);
    mini_know(&m, 401);
    mini_know(&m, 402);
    const uint8_t m401 = m.records[uid_index(&m, 401)].mastery;
    m.tick++;  // SUMMARY: the claims stand
    mini_session_start(&m);
    CHECK(m.streak.count == 0 && m.records[uid_index(&m, 401)].mastery == m401,
          "new session: empty streak, previous claims stand");
    CHECK(!mini_know(&m, 403), "one claim in the new session does not trigger");

    // Pause -> HOME discards the streak too (the session calls clear).
    mini_know(&m, 404);
    vocab_streak_clear(&m.streak);
    CHECK(!mini_know(&m, 405) && m.streak.count == 1, "pause -> HOME: a fresh streak");
}

// End to end with real Stage 2 cards: the stage's KNOW / FORGOT results drive the streak
// exactly as the session does (claim before promotion, spot check after the third claim).
static vocab_result_t run_stage2_card(vocab_index_t index, bool know, vocab_ms_t t0)
{
    vocab_stage2_t st;
    vocab_view_t view;
    vocab_outbox_t out;
    vocab_rng_t rng;
    vocab_rng_seed(&rng, 1);
    vocab_view_init(&view);
    vocab_view_reset_card(&view);
    const vocab_stage_input_t in = {
        .bank = vocab_bank_builtin(), .index = index, .purpose = VOCAB_PURPOSE_LEARN,
        .format = VOCAB_FORMAT_S2, .mastery = 1, .lure_index = VOCAB_INDEX_NONE,
        .cloze_front = false, .allow_too_easy = true, .rng = &rng,
    };
    vocab_result_t result = VOCAB_RESULT_NONE;
    int outcomes = 0;
    vocab_outbox_clear(&out);
    vocab_stage_result_t r = vocab_stage2_begin(&st, &in, t0, &view, &out);
    const vocab_event_t steps[] = {
        vocab_event_timer(VOCAB_TIMER_STAGE),
        vocab_event_click(VOCAB_KEY_OK),
        vocab_event_click(know ? VOCAB_KEY_UP : VOCAB_KEY_DOWN),
        vocab_event_timer(VOCAB_TIMER_STAGE),
    };
    for (size_t i = 0; i < ARRAY_LEN(steps) && r.status == VOCAB_STAGE_RUNNING; i++) {
        vocab_outbox_clear(&out);
        r = vocab_stage2_handle(&st, &steps[i], t0 + 1000u + 500u * (vocab_ms_t)i, &view, &out);
        if (r.outcome_ready) {
            outcomes++;
            result = r.result;
        }
    }
    CHECK(r.status == VOCAB_STAGE_DONE && outcomes == 1, "Stage 2 card finished with one outcome");
    return result;
}

static void test_with_stage2_cards(void)
{
    mini_t m;
    mini_init(&m, 2026);
    mini_session_start(&m);
    const vocab_uid_t uids[] = { 202, 401, 601, 703, 801 };  // words without cloze data
    const bool answers[] = { true, true, false, true, true };
    int triggers = 0;
    for (size_t i = 0; i < ARRAY_LEN(uids); i++) {
        const vocab_index_t index = uid_index(&m, uids[i]);
        m.records[index] = rec(uids[i], 1, 9, SEEN, 0);
        const vocab_result_t res = run_stage2_card(index, answers[i], 1000u * (vocab_ms_t)i);
        CHECK(res == (answers[i] ? VOCAB_RESULT_KNOW : VOCAB_RESULT_FORGOT), "stage result");
        if (res == VOCAB_RESULT_KNOW) {
            triggers += mini_know(&m, uids[i]) ? 1 : 0;
        } else {
            mini_forgot(&m, uids[i]);
        }
    }
    CHECK(triggers == 0 && m.streak.count == 2, "forgot in the middle: no spot check yet");
    const vocab_index_t index = uid_index(&m, 805);
    m.records[index] = rec(805, 2, 9, SEEN, 0);
    CHECK(run_stage2_card(index, true, 9000) == VOCAB_RESULT_KNOW, "third claim");
    CHECK(mini_know(&m, 805), "three Stage 2 claims after the forgot trigger the check");
    CHECK(m.spot_index == uid_index(&m, 703) || m.spot_index == uid_index(&m, 801),
          "target among the claims before the most recent");
    CHECK(m.spot_format == VOCAB_FORMAT_S1, "targets without cloze data: Stage 1 check");
    mini_spot(&m, false);
    CHECK(m.streak.count == 0, "rollback done");
}

// =======================================================================================
// Randomized model-based test against an independent reference of the rules
// =======================================================================================

typedef struct {
    vocab_index_t index;
    vocab_record_t pre;
} ref_claim_t;

// Independent reference of O11: for each distinct claimed word, its earliest snapshot is the
// state before the streak; the target word is demoted, every other word restored.
static void ref_rollback(vocab_record_t *records, vocab_index_t count, const ref_claim_t *claims,
                         int n, int target_slot, vocab_tick_t tick)
{
    const vocab_tick_t next = vocab_tick_add(tick, 1u);
    const bool has_target = target_slot >= 0 && target_slot < n;
    for (int i = 0; i < n; i++) {
        bool first = true;
        for (int j = 0; j < i; j++) first = first && claims[j].index != claims[i].index;
        if (!first || claims[i].index >= count) continue;
        const vocab_record_t *pre = &claims[i].pre;
        vocab_record_t *r = &records[claims[i].index];
        const uint8_t pre_m = min_u8(pre->mastery, (uint8_t)VOCAB_MASTERY_MAX);
        r->flags = (uint8_t)((r->flags & ~EVER) | (pre->flags & EVER));
        if (has_target && claims[i].index == claims[target_slot].index) {
            r->mastery = min_u8(pre_m, 1u);
            r->due = next;
            if (r->lapses < 255u) r->lapses++;
        } else {
            r->mastery = pre_m;
            if (pre->due < next) {
                r->due = pre->due;
            } else {
                r->due = next;
            }
        }
    }
}

static void test_random_model(void)
{
    enum { N = 10 };
    vocab_rng_t r;
    vocab_rng_seed(&r, 4815u);
    const int failures_before = vocab_test_failures;
    unsigned fails = 0;
    unsigned passes = 0;
    unsigned target_slots[2] = { 0, 0 };
    for (int run = 0; run < 300 && vocab_test_failures == failures_before; run++) {
        vocab_record_t records[N];
        for (int i = 0; i < N; i++) {
            records[i] = rec((vocab_uid_t)(1000 + i), (uint8_t)vocab_rng_below(&r, 6u),
                             (vocab_tick_t)vocab_rng_below(&r, 80u),
                             (uint8_t)(vocab_rng_bit(&r) ? SEEN : 0u), (uint8_t)vocab_rng_below(&r, 256u));
            if (records[i].mastery == VOCAB_MASTERY_MAX) records[i].flags |= EVER;
        }
        vocab_tick_t tick = (vocab_tick_t)vocab_rng_below(&r, 60u);
        if (vocab_rng_below(&r, 10u) == 0) tick = (vocab_tick_t)(65530u + vocab_rng_below(&r, 6u));
        vocab_streak_t s;
        vocab_streak_clear(&s);
        ref_claim_t ref[VOCAB_SPOT_CHECK_THRESHOLD];
        int n = 0;
        bool pending = false;
        for (int op = 0; op < 200 && vocab_test_failures == failures_before; op++) {
            const uint32_t action = vocab_rng_below(&r, 100u);
            if (pending) {
                // The spot check is the next card: pick the target, then pass or fail.
                uint8_t slot = 0xFF;
                const bool ok = vocab_streak_pick_target(&s, &r, &slot);
                CHECK(ok && slot < n - 1 && ref[slot].index != ref[n - 1].index,
                      "run %d: target excludes the most recent claim", run);
                if (!ok) break;
                if (slot < 2) target_slots[slot]++;
                if (vocab_rng_bit(&r)) {
                    vocab_record_t want[N];
                    memcpy(want, records, sizeof(want));
                    vocab_streak_pass(&s);
                    CHECK(memcmp(want, records, sizeof(want)) == 0, "run %d: pass changes no record", run);
                    passes++;
                } else {
                    vocab_record_t want[N];
                    memcpy(want, records, sizeof(want));
                    ref_rollback(want, N, ref, n, slot, tick);
                    vocab_streak_fail(&s, slot, records, N, tick);
                    for (int i = 0; i < N; i++) {
                        CHECK(rec_same(&records[i], &want[i]), "run %d op %d word %d: %s (want %s)",
                              run, op, i, rec_str(&records[i]), rec_str(&want[i]));
                    }
                    fails++;
                }
                n = 0;
                pending = false;
            } else if (action < 55u) {
                // Stage 2 KNOW on a word that is not already in the streak (session flow).
                vocab_index_t w = (vocab_index_t)vocab_rng_below(&r, N);
                bool in_streak = false;
                for (int k = 0; k < n; k++) in_streak = in_streak || ref[k].index == w;
                if (in_streak) continue;
                const bool trig = vocab_streak_claim(&s, w, &records[w]);
                ref[n].index = w;
                ref[n].pre = records[w];
                n++;
                CHECK(trig == (n == (int)VOCAB_SPOT_CHECK_THRESHOLD), "run %d: trigger at 3", run);
                promote_o3(&records[w], tick);
                pending = trig;
            } else if (action < 70u) {
                // Stage 2 FORGOT.
                const vocab_index_t w = (vocab_index_t)vocab_rng_below(&r, N);
                vocab_streak_clear(&s);
                n = 0;
                forgot_o4(&records[w], tick);
            } else if (action < 92u) {
                // Stage 1 / 3 card of a word outside the streak: the streak is untouched.
                const vocab_index_t w = (vocab_index_t)vocab_rng_below(&r, N);
                bool in_streak = false;
                for (int k = 0; k < n; k++) in_streak = in_streak || ref[k].index == w;
                if (in_streak) continue;
                records[w].mastery = (uint8_t)vocab_rng_below(&r, 6u);
                records[w].due = vocab_tick_add(tick, vocab_rng_below(&r, 21u));
                records[w].flags |= SEEN;
            } else {
                // Session start or pause -> HOME.
                vocab_streak_clear(&s);
                n = 0;
                if (vocab_rng_bit(&r)) tick = vocab_tick_add(tick, 1u);
            }
            CHECK(s.count == n, "run %d: streak count %u (model %d)", run, (unsigned)s.count, n);
            for (int k = 0; k < n && k < (int)s.count; k++) {
                const vocab_claim_t *c = &s.claims[k];
                CHECK(c->index == ref[k].index && c->pre_mastery == ref[k].pre.mastery &&
                          c->pre_due == ref[k].pre.due && c->pre_flags == ref[k].pre.flags,
                      "run %d: claim %d snapshot", run, k);
            }
        }
    }
    CHECK(fails > 300 && passes > 300, "model coverage: %u fails, %u passes", fails, passes);
    CHECK(target_slots[0] > 0 && target_slots[1] > 0, "both target slots used");
    printf("anti-cheat model: %u spot-check fails, %u passes (targets %u / %u)\n", fails, passes,
           target_slots[0], target_slots[1]);
}

int main(void)
{
    test_claims();
    test_pick_target();
    test_spot_format_data();
    test_pass();
    test_fail_exhaustive();
    test_fail_edges();
    test_fail_duplicates();
    test_mini_session();
    test_with_stage2_cards();
    test_random_model();
    return vocab_test_report("vocab anti-cheat tests");
}
