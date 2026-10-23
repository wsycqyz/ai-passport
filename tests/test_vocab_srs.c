// tests/test_vocab_srs.c - outcome rows O1-O14 and session composition (spec 4.1-4.4;
// AC-6, AC-7, AC-8 composition part, AC-11 record transitions).
//
// Owner: lead developer. O11 (spot-check rollback) is vocab_streak_fail(); the session test
// covers it end to end.
#include <string.h>

#include "vocab_bank.h"
#include "vocab_config.h"
#include "vocab_srs.h"
#include "vocab_test.h"

#define T 100u

static vocab_record_t rec(uint8_t m, bool seen, bool ever)
{
    vocab_record_t r = { 1, 50, m, 0, 2, 0 };
    if (seen) r.flags |= VOCAB_REC_SEEN;
    if (ever) r.flags |= VOCAB_REC_EVER_MASTERED;
    return r;
}

static vocab_srs_effect_t apply(vocab_record_t *r, vocab_purpose_t p, vocab_format_t f, vocab_result_t res)
{
    return vocab_srs_apply(r, p, f, res, T, vocab_record_seen(r));
}

static void test_rows(void)
{
    static const uint8_t interval[6] = VOCAB_INTERVALS_INIT;
    vocab_record_t r = rec(0, false, false);
    vocab_srs_effect_t e = apply(&r, VOCAB_PURPOSE_LEARN, VOCAB_FORMAT_S1, VOCAB_RESULT_CORRECT_FIRST);
    CHECK(r.mastery == 1 && r.due == T + 1 && vocab_record_seen(&r) && e.first_try && e.objective, "O1");

    r = rec(0, false, false);
    e = vocab_srs_apply(&r, VOCAB_PURPOSE_LEARN, VOCAB_FORMAT_S1, VOCAB_RESULT_WRONG, T, false);
    CHECK(r.mastery == 0 && r.due == T + 1 && r.lapses == 2 && !e.first_try && e.objective,
          "O2 first encounter: no lapse");
    r = rec(1, true, false);
    vocab_srs_apply(&r, VOCAB_PURPOSE_LEARN, VOCAB_FORMAT_S1, VOCAB_RESULT_WRONG, T, true);
    CHECK(r.mastery == 0 && r.due == T + 1 && r.lapses == 3, "O2 seen before: lapse");

    for (uint8_t m = 1; m <= 5; m++) {
        r = rec(m, true, m == 5);
        e = apply(&r, VOCAB_PURPOSE_LEARN, VOCAB_FORMAT_S2, VOCAB_RESULT_KNOW);
        const uint8_t next = m < 5 ? (uint8_t)(m + 1) : 5;
        CHECK(r.mastery == next && r.due == T + interval[next] && !e.first_try && !e.objective,
              "O3 from m=%u", m);
        CHECK(e.new_mastered == (m == 4), "O3 new mastery only the first time (m=%u)", m);
        CHECK(((r.flags & VOCAB_REC_EVER_MASTERED) != 0) == (next == 5), "ever_mastered at 5");
    }
    r = rec(2, true, false);
    e = apply(&r, VOCAB_PURPOSE_LEARN, VOCAB_FORMAT_S2, VOCAB_RESULT_FORGOT);
    CHECK(r.mastery == 1 && r.due == T + 1 && r.lapses == 3, "O4");

    r = rec(0, true, false);
    e = apply(&r, VOCAB_PURPOSE_TEST_LATER, VOCAB_FORMAT_S1, VOCAB_RESULT_CORRECT_FIRST);
    CHECK(r.mastery == 0 && r.due == 50 && e.first_try && e.objective, "O5 unchanged");
    r = rec(1, true, false);
    e = apply(&r, VOCAB_PURPOSE_TEST_LATER, VOCAB_FORMAT_S1, VOCAB_RESULT_WRONG);
    CHECK(r.mastery == 0 && r.due == T + 1 && r.lapses == 3 && !e.first_try, "O6");

    r = rec(3, true, false);
    e = apply(&r, VOCAB_PURPOSE_LEARN, VOCAB_FORMAT_S3, VOCAB_RESULT_CORRECT_FIRST);
    CHECK(r.mastery == 4 && r.due == T + interval[4] && e.first_try && e.objective, "O7");
    r = rec(4, true, false);
    e = apply(&r, VOCAB_PURPOSE_LEARN, VOCAB_FORMAT_S3, VOCAB_RESULT_CORRECT_FIRST);
    CHECK(r.mastery == 5 && e.new_mastered, "O7 to mastery 5");
    r = rec(4, true, false);
    e = apply(&r, VOCAB_PURPOSE_LEARN, VOCAB_FORMAT_S3, VOCAB_RESULT_CORRECT_SECOND);
    CHECK(r.mastery == 4 && r.due == T + 1 && !e.first_try && e.objective && r.lapses == 2, "O8");
    r = rec(5, true, true);
    e = apply(&r, VOCAB_PURPOSE_LEARN, VOCAB_FORMAT_S3, VOCAB_RESULT_WRONG);
    CHECK(r.mastery == 2 && r.due == T + 1 && r.lapses == 3 && (r.flags & VOCAB_REC_EVER_MASTERED), "O9");

    for (int f = 0; f < 2; f++) {
        const vocab_format_t fmt = f ? VOCAB_FORMAT_S3 : VOCAB_FORMAT_S1;
        r = rec(3, true, false);
        e = apply(&r, VOCAB_PURPOSE_SPOT_CHECK, fmt, VOCAB_RESULT_CORRECT_FIRST);
        CHECK(r.mastery == 3 && r.due == 50 && e.first_try, "O10 format %d", f);
        r = rec(3, true, false);
        e = apply(&r, VOCAB_PURPOSE_SPOT_CHECK, fmt, VOCAB_RESULT_WRONG);
        CHECK(r.mastery == 3 && r.due == 50 && r.lapses == 2 && e.objective && !e.first_try,
              "O11 leaves the record to vocab_streak_fail (format %d)", f);

        r = rec(2, true, false);
        e = apply(&r, VOCAB_PURPOSE_TOO_EASY_CHECK, fmt, VOCAB_RESULT_CORRECT_FIRST);
        CHECK(r.mastery == 5 && r.due == T + VOCAB_TOO_EASY_INTERVAL && e.first_try && e.new_mastered,
              "O13 format %d", f);
        r = rec(3, true, false);
        e = apply(&r, VOCAB_PURPOSE_TOO_EASY_CHECK, fmt, VOCAB_RESULT_WRONG);
        CHECK(r.mastery == 1 && r.due == T + 1 && r.lapses == 3, "O14 format %d", f);
    }
    r = rec(0, false, false);
    e = apply(&r, VOCAB_PURPOSE_LEARN, VOCAB_FORMAT_S2, VOCAB_RESULT_TOO_EASY);
    CHECK(r.mastery == 0 && r.due == 50 && vocab_record_seen(&r) && !e.objective, "O12 marks seen only");

    r = rec(4, true, false);
    r.lapses = 255;
    vocab_srs_apply(&r, VOCAB_PURPOSE_LEARN, VOCAB_FORMAT_S2, VOCAB_RESULT_FORGOT, 65534, true);
    CHECK(r.lapses == 255 && r.due == 65535, "lapses and due saturate");
    r = rec(3, true, false);
    vocab_srs_apply(&r, VOCAB_PURPOSE_LEARN, VOCAB_FORMAT_S2, VOCAB_RESULT_KNOW, 65530, true);
    CHECK(r.due == 65535, "promotion due saturates");
}

// ---------------------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------------------

static vocab_record_t records[64];

static void fresh(const vocab_bank_t *bank)
{
    memset(records, 0, sizeof(records));
    for (vocab_index_t i = 0; i < bank->count; i++) records[i].uid = bank->entries[i].uid;
}

static void seen(vocab_index_t i, uint8_t m, vocab_tick_t due)
{
    records[i].flags = VOCAB_REC_SEEN;
    records[i].mastery = m;
    records[i].due = due;
}

static bool contains(const vocab_index_t *set, size_t n, vocab_index_t v)
{
    for (size_t i = 0; i < n; i++) {
        if (set[i] == v) return true;
    }
    return false;
}

static void test_compose(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    vocab_index_t out[VOCAB_SESSION_WORDS];
    vocab_rng_t rng;

    // AC-6: fresh records -> the first 5 bank words, in the seeded shuffle order.
    fresh(bank);
    vocab_rng_seed(&rng, 77);
    size_t n = vocab_srs_compose(bank, records, 0, &rng, out, VOCAB_SESSION_WORDS);
    vocab_index_t expect[5] = { 0, 1, 2, 3, 4 };
    vocab_rng_t ref;
    vocab_rng_seed(&ref, 77);
    vocab_rng_shuffle(&ref, expect, 5);
    CHECK(n == 5 && memcmp(out, expect, sizeof(expect)) == 0, "AC-6 fresh session order");

    // AC-7: 7 due -> the 4 most overdue (ties: lower m, then lower uid) + 1 new.
    fresh(bank);
    seen(10, 2, 95);   // overdue 5
    seen(11, 1, 95);   // overdue 5, lower m -> before 10
    seen(12, 3, 90);   // overdue 10 -> first
    seen(13, 1, 99);   // overdue 1
    seen(14, 1, 99);   // overdue 1, higher uid than 13
    seen(15, 4, 100);  // due now
    seen(16, 2, 97);   // overdue 3
    n = vocab_srs_compose(bank, records, T, NULL, out, VOCAB_SESSION_WORDS);
    const vocab_index_t due4[4] = { 12, 11, 10, 16 };
    CHECK(n == 5 && memcmp(out, due4, sizeof(due4)) == 0 && out[4] == 0,
          "AC-7: 4 most overdue + 1 new (got %u %u %u %u %u)", out[0], out[1], out[2], out[3], out[4]);

    // 2 due -> 2 + 3 new.
    fresh(bank);
    seen(20, 1, 50);
    seen(21, 2, 60);
    n = vocab_srs_compose(bank, records, T, NULL, out, VOCAB_SESSION_WORDS);
    CHECK(n == 5 && out[0] == 20 && out[1] == 21 && out[2] == 0 && out[3] == 1 && out[4] == 2,
          "AC-7: 2 due + 3 new");

    // 1 due, no early reviews: the new-word cap is exceeded to fill the session (step 6).
    fresh(bank);
    seen(30, 1, 10);
    n = vocab_srs_compose(bank, records, T, NULL, out, VOCAB_SESSION_WORDS);
    CHECK(n == 5 && out[0] == 30 && out[1] == 0 && out[2] == 1 && out[3] == 2 && out[4] == 3,
          "1 due + 3 new + 1 more new");

    // Nothing due and nothing new: the 5 soonest early reviews (AC-7).
    for (vocab_index_t i = 0; i < bank->count; i++) seen(i, (uint8_t)(i % 5), (vocab_tick_t)(T + 50 + i));
    seen(40, 3, T + 2);
    seen(41, 2, T + 2);   // same due, lower m -> before 40
    seen(42, 4, T + 1);
    seen(7, 1, T + 3);
    seen(8, 1, T + 3);
    n = vocab_srs_compose(bank, records, T, NULL, out, VOCAB_SESSION_WORDS);
    const vocab_index_t early[5] = { 42, 41, 40, 7, 8 };
    CHECK(n == 5 && memcmp(out, early, sizeof(early)) == 0, "AC-7: soonest early reviews");

    // Due words beyond the session and nothing new: 5 most overdue.
    for (vocab_index_t i = 0; i < bank->count; i++) seen(i, 2, (vocab_tick_t)(i));
    n = vocab_srs_compose(bank, records, T, NULL, out, VOCAB_SESSION_WORDS);
    CHECK(n == 5 && out[0] == 0 && out[4] == 4, "all due, none new: 5 most overdue");

    // AC-8: small banks.
    const vocab_bank_t three = { bank->entries, 3 };
    fresh(&three);
    n = vocab_srs_compose(&three, records, 0, NULL, out, VOCAB_SESSION_WORDS);
    CHECK(n == 3 && contains(out, n, 0) && contains(out, n, 1) && contains(out, n, 2), "3-entry bank");
    const vocab_bank_t one = { bank->entries, 1 };
    fresh(&one);
    n = vocab_srs_compose(&one, records, 0, NULL, out, VOCAB_SESSION_WORDS);
    CHECK(n == 1 && out[0] == 0, "1-entry bank");
    const vocab_bank_t empty = { bank->entries, 0 };
    CHECK(vocab_srs_compose(&empty, records, 0, NULL, out, VOCAB_SESSION_WORDS) == 0, "empty bank");
    const vocab_record_t seen_rec = { 1, 3, 0, VOCAB_REC_SEEN, 0, 0 };
    const vocab_record_t unseen_rec = { 1, 0, 0, 0, 0, 0 };
    CHECK(vocab_srs_is_due(&seen_rec, 3) && !vocab_srs_is_due(&seen_rec, 2) &&
              !vocab_srs_is_due(&unseen_rec, 5), "due = seen && due <= T; unseen words are never due");
}

int main(void)
{
    test_rows();
    test_compose();
    return vocab_test_report("vocab SRS tests");
}
