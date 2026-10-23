// tests/test_vocab_lure.c - Stage 1 lure picker (spec 5.2): AC-13 lure properties for every
// bank entry over 10,000 seeds (category, meaning, substring, kind, uniform pick), AC-14
// fallbacks with synthetic banks (single category, no valid lure), the `avoid` rule, RNG use,
// and spec 12.1 "every entry has a lure candidate" (words: same-kind lures in at least 8
// other categories; phrases: in at least 2, all from categories 7, 9 and 10).
//
// Owner: Stage 1 developer. Compiled by tools/validate.sh with:
//   tests/test_vocab_lure.c main/vocab_lure.c main/vocab_stage1.c
//   main/vocab_bank.c main/vocab_rng.c main/vocab_view.c
// Deterministic (fixed seeds). Synthetic banks reuse builtin meanings looked up by uid, so
// this file needs no Chinese literals.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vocab_bank.h"
#include "vocab_lure.h"
#include "vocab_test.h"

#define SEEDS 10000u
#define MAX_BANK 64u
#define OUT_SENTINEL ((vocab_index_t)0x1234u)

// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------

static const vocab_entry_t *by_uid(vocab_uid_t uid)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    vocab_index_t index = 0;
    return vocab_bank_find(bank, uid, &index) ? &bank->entries[index] : NULL;
}

static vocab_index_t index_of(vocab_uid_t uid)
{
    vocab_index_t index = VOCAB_INDEX_NONE;
    (void)vocab_bank_find(vocab_bank_builtin(), uid, &index);
    return index;
}

// Meaning of a builtin entry; synthetic banks use these instead of Chinese literals.
static const char *zh(vocab_uid_t uid)
{
    const vocab_entry_t *e = by_uid(uid);
    return e ? e->zh : NULL;
}

// A synthetic entry. The kind follows the text: a space makes it a PHRASE.
static vocab_entry_t make_entry(vocab_uid_t uid, uint8_t category, const char *text,
                                const char *meaning)
{
    vocab_entry_t e;
    memset(&e, 0, sizeof(e));
    e.uid = uid;
    e.category = category;
    e.text = text;
    e.zh = meaning;
    return e;
}

static bool acc(const vocab_entry_t *target, const vocab_entry_t *candidate, bool same_category)
{
    return vocab_lure_acceptable(target, candidate, same_category);
}

// Spec 5.2 step 1, written out independently of the implementation.
static bool spec_meanings_differ(const char *a, const char *b)
{
    return a && b && a[0] != '\0' && b[0] != '\0' && strcmp(a, b) != 0 && strstr(a, b) == NULL &&
           strstr(b, a) == NULL;
}

static bool spec_candidate(const vocab_entry_t *t, const vocab_entry_t *c, bool same_category_ok)
{
    return t->uid != c->uid && (same_category_ok || t->category != c->category) &&
           spec_meanings_differ(t->zh, c->zh);
}

// Spec 5.2 preferred set: the other-category candidates when any exist (step 1), else the
// same-category fallback (step 3); within that pool, the target's kind when any (step 2).
// Marks the members and returns their number.
static unsigned reference_set(const vocab_bank_t *bank, vocab_index_t target, bool member[])
{
    const vocab_entry_t *t = &bank->entries[target];
    const vocab_kind_t kind = vocab_entry_kind(t);
    bool any_other_category = false;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        if (spec_candidate(t, &bank->entries[i], false)) any_other_category = true;
    }
    const bool same_category_ok = !any_other_category;
    bool any_same_kind = false;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *c = &bank->entries[i];
        if (spec_candidate(t, c, same_category_ok) && vocab_entry_kind(c) == kind) {
            any_same_kind = true;
        }
    }
    unsigned n = 0;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *c = &bank->entries[i];
        member[i] = spec_candidate(t, c, same_category_ok) &&
                    (!any_same_kind || vocab_entry_kind(c) == kind);
        if (member[i]) n++;
    }
    return n;
}

// Picks with seeds first_seed .. first_seed + draws - 1 and counts the picks per index.
static void histogram(const vocab_bank_t *bank, vocab_index_t target, vocab_index_t avoid,
                      uint32_t first_seed, unsigned draws, unsigned hist[])
{
    unsigned failed = 0;
    memset(hist, 0, sizeof(unsigned) * bank->count);
    for (unsigned s = 0; s < draws; s++) {
        vocab_rng_t rng;
        vocab_rng_seed(&rng, first_seed + s);
        vocab_index_t lure = VOCAB_INDEX_NONE;
        if (!vocab_lure_pick(bank, target, avoid, &rng, &lure) || lure >= bank->count) {
            failed++;
            continue;
        }
        hist[lure]++;
    }
    CHECK(failed == 0, "target %u: %u of %u picks failed", (unsigned)target, failed, draws);
}

// Every member is drawn within +-40 % of its uniform share (more than 6 sigma for these
// sample sizes); non-members are never drawn.
static void check_uniform(const vocab_bank_t *bank, const unsigned hist[], const bool member[],
                          unsigned members, unsigned draws, const char *what)
{
    if (members == 0) {
        CHECK(false, "%s: empty reference set", what);
        return;
    }
    const double share = (double)draws / (double)members;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        if (member[i]) {
            CHECK(hist[i] >= 0.6 * share && hist[i] <= 1.4 * share,
                  "%s: index %u drawn %u times (uniform share %.0f)", what, (unsigned)i, hist[i],
                  share);
        } else {
            CHECK(hist[i] == 0, "%s: index %u is not a preferred candidate but was drawn %u times",
                  what, (unsigned)i, hist[i]);
        }
    }
}

// `target` always gets `want` (300 seeds).
static void expect_always(const vocab_bank_t *bank, vocab_index_t target, vocab_index_t want,
                          const char *what)
{
    unsigned wrong = 0;
    for (uint32_t seed = 1; seed <= 300u; seed++) {
        vocab_rng_t rng;
        vocab_rng_seed(&rng, seed);
        vocab_index_t lure = VOCAB_INDEX_NONE;
        if (!vocab_lure_pick(bank, target, VOCAB_INDEX_NONE, &rng, &lure) || lure != want) wrong++;
    }
    CHECK(wrong == 0, "%s: expected index %u every time (%u misses)", what, (unsigned)want, wrong);
}

// No lure for `target`: false, *out untouched, no RNG draw.
static void expect_none(const vocab_bank_t *bank, vocab_index_t target, const char *what)
{
    vocab_rng_t rng;
    vocab_rng_seed(&rng, 99);
    const uint64_t before = rng.state;
    vocab_index_t lure = OUT_SENTINEL;
    CHECK(!vocab_lure_pick(bank, target, VOCAB_INDEX_NONE, &rng, &lure), "%s: no lure", what);
    CHECK(lure == OUT_SENTINEL, "%s: output untouched", what);
    CHECK(rng.state == before, "%s: no RNG draw without a lure", what);
}

// ---------------------------------------------------------------------------------------
// vocab_lure_acceptable (spec 5.2 step 1 / step 3)
// ---------------------------------------------------------------------------------------

static void test_acceptable_rules(void)
{
    const vocab_entry_t *apple = by_uid(101);
    const vocab_entry_t *banana = by_uid(102);
    const vocab_entry_t *bus = by_uid(301);
    const vocab_entry_t *book = by_uid(403);
    const vocab_entry_t *ruler = by_uid(404);
    const vocab_entry_t *schoolbag = by_uid(405);
    const vocab_entry_t *thanks = by_uid(902);
    const vocab_entry_t *open_book = by_uid(1003);
    if (!apple || !banana || !bus || !book || !ruler || !schoolbag || !thanks || !open_book) {
        CHECK(false, "builtin test entries present");
        return;
    }

    CHECK(!acc(NULL, apple, true) && !acc(apple, NULL, true) && !acc(NULL, NULL, true),
          "NULL entries are never acceptable");
    CHECK(!acc(apple, apple, true), "an entry never lures itself");
    CHECK(acc(apple, bus, false), "fruit vs transport is a lure");
    CHECK(!acc(apple, banana, false), "same category rejected (step 1)");
    CHECK(acc(apple, banana, true), "same category allowed by the fallback (step 3)");
    CHECK(acc(apple, thanks, false) && acc(thanks, apple, false),
          "the kind does not matter for acceptability (pick prefers it)");
    for (int same = 0; same < 2; same++) {
        CHECK(!acc(book, open_book, same != 0) && !acc(open_book, book, same != 0),
              "spec example: 'book' inside 'open your book' meaning rejected both ways");
        CHECK(!acc(book, schoolbag, same != 0) && !acc(schoolbag, book, same != 0),
              "'book' as a prefix of the 'schoolbag' meaning rejected both ways");
    }
    CHECK(acc(book, ruler, true) && !acc(book, ruler, false),
          "unrelated same-category meanings: only in the fallback");

    // Equal meanings are compared by content, not by pointer.
    char apple_copy[16];
    snprintf(apple_copy, sizeof(apple_copy), "%s", apple->zh);
    const vocab_entry_t same_meaning = make_entry(7001, 7, "x", apple_copy);
    CHECK(!acc(apple, &same_meaning, true) && !acc(&same_meaning, apple, true),
          "equal meanings rejected");
    const vocab_entry_t no_meaning = make_entry(7002, 8, "x", NULL);
    const vocab_entry_t empty_meaning = make_entry(7003, 8, "x", "");
    CHECK(!acc(apple, &no_meaning, true) && !acc(&no_meaning, apple, true),
          "NULL meaning rejected");
    CHECK(!acc(apple, &empty_meaning, true) && !acc(&empty_meaning, apple, true),
          "empty meaning rejected");
    const vocab_entry_t same_uid = make_entry(apple->uid, 9, "x", bus->zh);
    CHECK(!acc(apple, &same_uid, true) && !acc(&same_uid, apple, true),
          "the uid identifies the entry");

    // Byte-wise substring rule on plain strings.
    const vocab_entry_t ab = make_entry(7010, 1, "a", "ab");
    const vocab_entry_t xaby = make_entry(7011, 2, "b", "xaby");
    const vocab_entry_t abab = make_entry(7012, 2, "c", "abab");
    const vocab_entry_t b = make_entry(7013, 2, "d", "b");
    const vocab_entry_t ba = make_entry(7014, 2, "e", "ba");
    CHECK(!acc(&ab, &xaby, false) && !acc(&xaby, &ab, false), "infix rejected");
    CHECK(!acc(&ab, &abab, false) && !acc(&abab, &ab, false), "prefix rejected");
    CHECK(!acc(&ab, &b, false) && !acc(&b, &ab, false), "suffix rejected");
    CHECK(acc(&ab, &ba, false) && acc(&ba, &ab, false), "shared characters alone are fine");
}

static void test_acceptable_on_bank(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    unsigned mismatches = 0;
    unsigned asymmetric = 0;
    unsigned not_monotonic = 0;
    unsigned substring_pairs = 0;
    for (vocab_index_t a = 0; a < bank->count; a++) {
        for (vocab_index_t b = 0; b < bank->count; b++) {
            const vocab_entry_t *ea = &bank->entries[a];
            const vocab_entry_t *eb = &bank->entries[b];
            for (int same = 0; same < 2; same++) {
                const bool got = acc(ea, eb, same != 0);
                if (got != spec_candidate(ea, eb, same != 0)) mismatches++;
                if (got != acc(eb, ea, same != 0)) asymmetric++;
            }
            if (acc(ea, eb, false) && !acc(ea, eb, true)) not_monotonic++;
            if (a < b && (strstr(ea->zh, eb->zh) || strstr(eb->zh, ea->zh))) substring_pairs++;
        }
    }
    CHECK(mismatches == 0, "acceptability matches spec 5.2 on every bank pair (%u mismatches)",
          mismatches);
    CHECK(asymmetric == 0, "acceptability is symmetric (%u asymmetric pairs)", asymmetric);
    CHECK(not_monotonic == 0, "allowing the category never rejects a lure (%u)", not_monotonic);
    // The test bank exercises the substring rule: book/schoolbag and book/open your book.
    CHECK(substring_pairs == 2, "bank substring pairs: %u (expected 2)", substring_pairs);
}

// Spec 12.1: every WORD has same-kind lures in at least 8 other categories; every PHRASE has
// same-kind lures in at least 2 other categories, all of them phrase categories 7, 9, 10.
static void test_bank_lure_coverage(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    unsigned words = 0;
    unsigned phrases = 0;
    CHECK(bank->count == 48u, "test bank has 48 entries (got %u)", (unsigned)bank->count);
    for (vocab_index_t t = 0; t < bank->count; t++) {
        const vocab_entry_t *te = &bank->entries[t];
        const vocab_kind_t kind = vocab_entry_kind(te);
        bool seen[256] = { false };
        unsigned categories = 0;
        bool phrase_categories_only = true;
        for (vocab_index_t c = 0; c < bank->count; c++) {
            const vocab_entry_t *ce = &bank->entries[c];
            if (!acc(te, ce, false) || vocab_entry_kind(ce) != kind) continue;
            if (!seen[ce->category]) {
                seen[ce->category] = true;
                categories++;
            }
            if (ce->category != 7u && ce->category != 9u && ce->category != 10u) {
                phrase_categories_only = false;
            }
        }
        if (kind == VOCAB_KIND_WORD) {
            words++;
            CHECK(categories >= 8u, "word uid %u: same-kind lures in %u other categories",
                  (unsigned)te->uid, categories);
        } else {
            phrases++;
            CHECK(categories >= 2u && phrase_categories_only,
                  "phrase uid %u: same-kind lures in %u other categories (7/9/10 only: %d)",
                  (unsigned)te->uid, categories, (int)phrase_categories_only);
        }
        vocab_rng_t rng;
        vocab_rng_seed(&rng, 1u + t);
        vocab_index_t lure = VOCAB_INDEX_NONE;
        CHECK(vocab_lure_pick(bank, t, VOCAB_INDEX_NONE, &rng, &lure) && lure != t,
              "uid %u has a lure candidate", (unsigned)te->uid);
    }
    CHECK(words == 40u && phrases == 8u, "40 words and 8 phrases (got %u / %u)", words, phrases);
}

// ---------------------------------------------------------------------------------------
// vocab_lure_pick
// ---------------------------------------------------------------------------------------

// AC-13: for every bank entry over 10,000 seeds the lure has a different category, a
// different meaning, no substring relation and the same kind when available; the pick is
// uniform over the spec's preferred set.
static void test_ac13_properties(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    if (bank->count > MAX_BANK) {
        CHECK(false, "bank larger than the test buffers");
        return;
    }
    for (vocab_index_t t = 0; t < bank->count; t++) {
        const vocab_entry_t *te = &bank->entries[t];
        const vocab_kind_t kind = vocab_entry_kind(te);
        bool member[MAX_BANK];
        unsigned hist[MAX_BANK];
        const unsigned members = reference_set(bank, t, member);
        bool same_kind_available = false;
        for (vocab_index_t c = 0; c < bank->count; c++) {
            if (spec_candidate(te, &bank->entries[c], false) &&
                vocab_entry_kind(&bank->entries[c]) == kind) {
                same_kind_available = true;
            }
        }
        CHECK(same_kind_available, "uid %u: a same-kind lure from another category exists",
              (unsigned)te->uid);

        unsigned failed = 0;
        unsigned bad_index = 0;
        unsigned bad_category = 0;
        unsigned bad_meaning = 0;
        unsigned bad_substring = 0;
        unsigned bad_kind = 0;
        memset(hist, 0, sizeof(hist));
        for (uint32_t seed = 1; seed <= SEEDS; seed++) {
            vocab_rng_t rng;
            vocab_rng_seed(&rng, seed);
            vocab_index_t lure = VOCAB_INDEX_NONE;
            if (!vocab_lure_pick(bank, t, VOCAB_INDEX_NONE, &rng, &lure)) {
                failed++;
                continue;
            }
            if (lure >= bank->count || lure == t) {
                bad_index++;
                continue;
            }
            const vocab_entry_t *le = &bank->entries[lure];
            if (le->category == te->category) bad_category++;
            if (strcmp(le->zh, te->zh) == 0) bad_meaning++;
            if (strstr(le->zh, te->zh) || strstr(te->zh, le->zh)) bad_substring++;
            if (same_kind_available && vocab_entry_kind(le) != kind) bad_kind++;
            hist[lure]++;
        }
        CHECK(failed == 0 && bad_index == 0, "uid %u: %u failed picks, %u bad indexes",
              (unsigned)te->uid, failed, bad_index);
        CHECK(bad_category == 0, "uid %u: %u lures from the same category", (unsigned)te->uid,
              bad_category);
        CHECK(bad_meaning == 0, "uid %u: %u lures with the same meaning", (unsigned)te->uid,
              bad_meaning);
        CHECK(bad_substring == 0, "uid %u: %u lures in a substring relation", (unsigned)te->uid,
              bad_substring);
        CHECK(bad_kind == 0, "uid %u: %u lures of the other kind", (unsigned)te->uid, bad_kind);
        char what[48];
        snprintf(what, sizeof(what), "AC-13 uid %u", (unsigned)te->uid);
        check_uniform(bank, hist, member, members, SEEDS, what);
    }
}

// Spec 5.2 step 4: inserted cards avoid the lure the word already used when another
// candidate remains; `avoid` never pushes the pick into a weaker tier.
static void test_avoid(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    const vocab_index_t apple = index_of(101);
    bool member[MAX_BANK];
    unsigned hist[MAX_BANK];
    const unsigned members = reference_set(bank, apple, member);
    vocab_index_t avoid = VOCAB_INDEX_NONE;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        if (member[i]) {
            avoid = i;
            break;
        }
    }
    CHECK(avoid != VOCAB_INDEX_NONE && members > 2u, "apple has several preferred lures");
    if (avoid == VOCAB_INDEX_NONE) return;

    histogram(bank, apple, avoid, 1, SEEDS, hist);
    CHECK(hist[avoid] == 0, "the avoided lure is never picked while others remain (%u)",
          hist[avoid]);
    member[avoid] = false;
    check_uniform(bank, hist, member, members - 1u, SEEDS, "avoid excluded");
    member[avoid] = true;
    histogram(bank, apple, VOCAB_INDEX_NONE, 1, 2000, hist);
    CHECK(hist[avoid] > 0, "without `avoid` that lure is reachable");

    // An `avoid` outside the preferred set changes nothing: same lure, same RNG use.
    const vocab_index_t outside[] = {
        VOCAB_INDEX_NONE,
        apple,                                 // the target itself
        (vocab_index_t)(bank->count + 5u),     // out of range
        index_of(902),                         // a phrase: not preferred for a word
        index_of(102),                         // same category: not preferred
    };
    for (size_t k = 0; k < sizeof(outside) / sizeof(outside[0]); k++) {
        unsigned differ = 0;
        for (uint32_t seed = 1; seed <= 500u; seed++) {
            vocab_rng_t a;
            vocab_rng_t b;
            vocab_rng_seed(&a, seed);
            vocab_rng_seed(&b, seed);
            vocab_index_t la = VOCAB_INDEX_NONE;
            vocab_index_t lb = VOCAB_INDEX_NONE;
            const bool ok_a = vocab_lure_pick(bank, apple, VOCAB_INDEX_NONE, &a, &la);
            const bool ok_b = vocab_lure_pick(bank, apple, outside[k], &b, &lb);
            if (!ok_a || !ok_b || la != lb || a.state != b.state) differ++;
        }
        CHECK(differ == 0, "avoid index %u outside the preferred set has no effect (%u diffs)",
              (unsigned)outside[k], differ);
    }

    // The only preferred candidate is still used when it is the one to avoid.
    const vocab_entry_t e[4] = {
        make_entry(1, 1, "w", zh(101)),    // target word, category 1
        make_entry(2, 2, "w", zh(201)),    // the only other-category word: avoided
        make_entry(3, 3, "p p", zh(902)),  // other-category phrase (weaker tier)
        make_entry(4, 1, "w", zh(102)),    // same-category word (fallback tier)
    };
    const vocab_bank_t small = { e, 4 };
    unsigned wrong = 0;
    for (uint32_t seed = 1; seed <= 300u; seed++) {
        vocab_rng_t rng;
        vocab_rng_seed(&rng, seed);
        vocab_index_t lure = VOCAB_INDEX_NONE;
        if (!vocab_lure_pick(&small, 0, 1, &rng, &lure) || lure != 1) wrong++;
    }
    CHECK(wrong == 0, "a sole preferred lure is kept even when avoided (%u misses)", wrong);
}

// AC-14 part 1: a single-category bank falls back to the same category (spec 5.2 step 3),
// still preferring the same kind and picking uniformly.
static void test_ac14_single_category(void)
{
    const vocab_entry_t e[5] = {
        make_entry(1, 5, "w", zh(101)), make_entry(2, 5, "w", zh(201)),
        make_entry(3, 5, "w", zh(301)), make_entry(4, 5, "w", zh(501)),
        make_entry(5, 5, "w", zh(701)),
    };
    const vocab_bank_t bank = { e, 5 };
    for (vocab_index_t t = 0; t < bank.count; t++) {
        bool member[5];
        unsigned hist[5];
        const unsigned members = reference_set(&bank, t, member);
        CHECK(members == 4u, "single category: 4 fallback candidates (got %u)", members);
        histogram(&bank, t, VOCAB_INDEX_NONE, 1, SEEDS, hist);
        check_uniform(&bank, hist, member, members, SEEDS, "single category");
        for (vocab_index_t l = 0; l < bank.count; l++) {
            if (hist[l] == 0) continue;
            CHECK(e[l].category == e[t].category && acc(&e[t], &e[l], true) &&
                      !acc(&e[t], &e[l], false),
                  "single category: lure %u comes from the same-category fallback", (unsigned)l);
        }
    }

    // Mixed kinds in one category: the fallback tier still prefers the same kind.
    const vocab_entry_t m[4] = {
        make_entry(1, 4, "w", zh(101)), make_entry(2, 4, "w", zh(201)),
        make_entry(3, 4, "p p", zh(902)), make_entry(4, 4, "p p", zh(1001)),
    };
    const vocab_bank_t mixed = { m, 4 };
    expect_always(&mixed, 0, 1, "single category: word -> word");
    expect_always(&mixed, 1, 0, "single category: word -> word");
    expect_always(&mixed, 2, 3, "single category: phrase -> phrase");
    expect_always(&mixed, 3, 2, "single category: phrase -> phrase");

    // The only same-kind candidate fails the meaning rules: the other kind is used.
    const vocab_entry_t s[4] = {
        make_entry(1, 4, "w", zh(403)),     // book
        make_entry(2, 4, "w", zh(405)),     // schoolbag: its meaning contains "book"
        make_entry(3, 4, "p p", zh(1001)),  // stand up
        make_entry(4, 4, "p p", zh(1002)),  // sit down
    };
    const vocab_bank_t sub = { s, 4 };
    bool member[4];
    unsigned hist[4];
    const unsigned members = reference_set(&sub, 0, member);
    CHECK(members == 2u && member[2] && member[3], "fallback to the other kind");
    histogram(&sub, 0, VOCAB_INDEX_NONE, 1, 4000, hist);
    check_uniform(&sub, hist, member, members, 4000, "single category, other kind");
}

// AC-14 part 2: a different category beats the kind preference; other-category entries that
// fail the meaning rules do not count as candidates.
static void test_ac14_category_first(void)
{
    const vocab_entry_t e[3] = {
        make_entry(1, 1, "w", zh(101)),    // target word, category 1
        make_entry(2, 1, "w", zh(102)),    // same category, same kind
        make_entry(3, 2, "p p", zh(902)),  // other category, other kind
    };
    const vocab_bank_t bank = { e, 3 };
    expect_always(&bank, 0, 2, "other category (other kind) before the same category");

    const vocab_entry_t f[3] = {
        make_entry(1, 4, "w", zh(403)),        // book, category 4
        make_entry(2, 10, "p p p", zh(1003)),  // open your book: contains "book"
        make_entry(3, 4, "w", zh(404)),        // ruler, category 4
    };
    const vocab_bank_t fb = { f, 3 };
    expect_always(&fb, 0, 2, "book -> ruler (same-category fallback)");
    expect_always(&fb, 1, 2, "open your book -> ruler (other category, other kind)");
    expect_always(&fb, 2, 1, "ruler -> open your book (other category beats the kind)");
}

// AC-14 part 3: no valid lure -> false (the session then serves Stage 2), output untouched,
// no RNG draw; invalid arguments behave the same way.
static void test_ac14_no_lure(void)
{
    const vocab_entry_t one[1] = { make_entry(1, 1, "w", zh(101)) };
    const vocab_bank_t single = { one, 1 };
    expect_none(&single, 0, "one-entry bank");

    const vocab_entry_t same[2] = {
        make_entry(1, 1, "w", zh(101)),
        make_entry(2, 2, "w", zh(101)),
    };
    const vocab_bank_t equal = { same, 2 };
    expect_none(&equal, 0, "equal meanings");
    expect_none(&equal, 1, "equal meanings");

    const vocab_entry_t nested[2] = {
        make_entry(1, 4, "w", zh(403)),  // book
        make_entry(2, 4, "w", zh(405)),  // schoolbag
    };
    const vocab_bank_t sub = { nested, 2 };
    expect_none(&sub, 0, "substring meanings");
    expect_none(&sub, 1, "substring meanings");

    const vocab_entry_t chain[3] = {
        make_entry(1, 4, "w", zh(403)),        // book
        make_entry(2, 4, "w", zh(405)),        // schoolbag
        make_entry(3, 10, "p p p", zh(1003)),  // open your book
    };
    const vocab_bank_t ch = { chain, 3 };
    expect_none(&ch, 0, "book: every other meaning contains it");
    expect_always(&ch, 1, 2, "schoolbag -> open your book");
    expect_always(&ch, 2, 1, "open your book -> schoolbag");

    const vocab_entry_t dup[2] = {
        make_entry(1, 1, "w", zh(101)),
        make_entry(1, 2, "w", zh(301)),  // same uid as the target
    };
    const vocab_bank_t dups = { dup, 2 };
    expect_none(&dups, 0, "duplicate uid");

    const vocab_entry_t missing[2] = {
        make_entry(1, 1, "w", zh(101)),
        make_entry(2, 2, "w", NULL),
    };
    const vocab_bank_t no_zh = { missing, 2 };
    expect_none(&no_zh, 0, "the only candidate has no meaning");
    expect_none(&no_zh, 1, "the target has no meaning");

    // Invalid arguments.
    const vocab_bank_t *bank = vocab_bank_builtin();
    const vocab_bank_t no_entries = { NULL, 3 };
    const vocab_bank_t empty = { bank->entries, 0 };
    vocab_rng_t rng;
    vocab_rng_seed(&rng, 5);
    const uint64_t before = rng.state;
    vocab_index_t lure = OUT_SENTINEL;
    CHECK(!vocab_lure_pick(NULL, 0, VOCAB_INDEX_NONE, &rng, &lure), "NULL bank");
    CHECK(!vocab_lure_pick(&no_entries, 0, VOCAB_INDEX_NONE, &rng, &lure),
          "bank without entries");
    CHECK(!vocab_lure_pick(&empty, 0, VOCAB_INDEX_NONE, &rng, &lure), "empty bank");
    CHECK(!vocab_lure_pick(bank, bank->count, VOCAB_INDEX_NONE, &rng, &lure),
          "target out of range");
    CHECK(!vocab_lure_pick(bank, VOCAB_INDEX_NONE, VOCAB_INDEX_NONE, &rng, &lure), "target NONE");
    CHECK(!vocab_lure_pick(bank, 0, VOCAB_INDEX_NONE, NULL, &lure), "NULL rng");
    CHECK(!vocab_lure_pick(bank, 0, VOCAB_INDEX_NONE, &rng, NULL), "NULL output");
    CHECK(lure == OUT_SENTINEL && rng.state == before,
          "invalid calls touch neither the output nor the RNG");
}

// Determinism and RNG use: same seed -> same lure and RNG state; a choice among several
// candidates draws from the RNG; a sole candidate is returned without a draw.
static void test_rng_use(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    unsigned nondeterministic = 0;
    unsigned no_draw = 0;
    for (vocab_index_t t = 0; t < bank->count; t++) {
        vocab_rng_t a;
        vocab_rng_t b;
        vocab_rng_seed(&a, 4242u + t);
        vocab_rng_seed(&b, 4242u + t);
        const uint64_t start = a.state;
        vocab_index_t la = VOCAB_INDEX_NONE;
        vocab_index_t lb = 0;  // a different initial value must not matter
        const bool ok_a = vocab_lure_pick(bank, t, VOCAB_INDEX_NONE, &a, &la);
        const bool ok_b = vocab_lure_pick(bank, t, VOCAB_INDEX_NONE, &b, &lb);
        if (!ok_a || !ok_b || la != lb || a.state != b.state) nondeterministic++;
        if (a.state == start) no_draw++;
    }
    CHECK(nondeterministic == 0, "picks are deterministic per seed (%u differ)", nondeterministic);
    CHECK(no_draw == 0, "a pick among several candidates draws from the RNG (%u did not)", no_draw);

    const vocab_entry_t e[2] = { make_entry(1, 1, "w", zh(101)), make_entry(2, 2, "w", zh(201)) };
    const vocab_bank_t two = { e, 2 };
    vocab_rng_t rng;
    vocab_rng_seed(&rng, 7);
    const uint64_t before = rng.state;
    vocab_index_t lure = VOCAB_INDEX_NONE;
    CHECK(vocab_lure_pick(&two, 0, VOCAB_INDEX_NONE, &rng, &lure) && lure == 1 &&
              rng.state == before,
          "a sole candidate is returned without an RNG draw");
}

// Randomized property test over small synthetic banks (random categories, kinds and
// meanings, including equal and nested meanings): every pick lies in the spec's preferred
// set, the set is covered, and a pick succeeds exactly when some entry is acceptable with
// the category relaxed (the session predicts Stage 1 availability that way).
static void test_random_banks(void)
{
    const vocab_uid_t pool[] = { 101, 201, 301, 403, 405, 1003, 902, 1001, 504, 704 };
    const char *texts[] = { "w", "p p" };
    vocab_rng_t g;
    vocab_rng_seed(&g, 2024);
    unsigned wrong_success = 0;
    unsigned outside = 0;
    unsigned uncovered = 0;
    unsigned picks = 0;
    for (unsigned b = 0; b < 3000u; b++) {
        vocab_entry_t e[8];
        const vocab_index_t count = (vocab_index_t)(1u + vocab_rng_below(&g, 8));
        for (vocab_index_t i = 0; i < count; i++) {
            const vocab_uid_t uid = vocab_rng_below(&g, 10) == 0 && i > 0
                                        ? e[i - 1].uid  // occasional duplicate uid
                                        : (vocab_uid_t)(100u + i);
            e[i] = make_entry(uid, (uint8_t)(1u + vocab_rng_below(&g, 3)),
                              texts[vocab_rng_below(&g, 2)],
                              zh(pool[vocab_rng_below(&g, sizeof(pool) / sizeof(pool[0]))]));
        }
        const vocab_bank_t bank = { e, count };
        for (vocab_index_t t = 0; t < count; t++) {
            bool exists = false;
            for (vocab_index_t i = 0; i < count; i++) {
                if (acc(&e[t], &e[i], true)) exists = true;
            }
            bool member[8];
            const unsigned members = reference_set(&bank, t, member);
            bool hit[8] = { false };
            for (uint32_t seed = 1; seed <= 192u; seed++) {
                vocab_rng_t rng;
                vocab_rng_seed(&rng, seed);
                vocab_index_t lure = OUT_SENTINEL;
                const bool ok = vocab_lure_pick(&bank, t, VOCAB_INDEX_NONE, &rng, &lure);
                if (ok != exists || ok != (members > 0)) wrong_success++;
                if (!ok) continue;
                picks++;
                if (lure >= count || !member[lure]) {
                    outside++;
                } else {
                    hit[lure] = true;
                }
            }
            for (vocab_index_t i = 0; i < count; i++) {
                if (member[i] && !hit[i]) uncovered++;  // <= 7 members, 192 draws: P < 1e-12
            }
        }
    }
    CHECK(wrong_success == 0, "pick succeeds exactly when an acceptable entry exists (%u)",
          wrong_success);
    CHECK(outside == 0, "picks stay inside the spec's preferred set (%u outside)", outside);
    CHECK(uncovered == 0, "every preferred candidate is reachable (%u never drawn)", uncovered);
    CHECK(picks > 100000u, "the property test made enough picks (%u)", picks);
}

int main(void)
{
    test_acceptable_rules();
    test_acceptable_on_bank();
    test_bank_lure_coverage();
    test_ac13_properties();
    test_avoid();
    test_ac14_single_category();
    test_ac14_category_first();
    test_ac14_no_lure();
    test_rng_use();
    test_random_banks();
    return vocab_test_report("vocab lure tests");
}
