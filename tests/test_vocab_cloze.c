// tests/test_vocab_cloze.c - Stage 3 cloze data: skeleton split, chunk priority, the
// validator with the committed common-word list, the deny-list and the skeleton row
// geometry (spec 7.1, 7.2, 7.3; AC-23, AC-25 blank width).
//
// Owner: Stage 3 developer. Compiled by tools/validate.sh with:
//   tests/test_vocab_cloze.c main/vocab_cloze.c main/vocab_stage3.c
//   main/vocab_bank.c main/vocab_rng.c main/vocab_view.c
// The static gate runs it from the repository root: it reads the committed list
// assets/wordlists/common-words-35.txt (tools/vocab_wordlist.py, SCOWL/ESDB size <= 35).
// No offensive word is spelled out here: deny-list cases are built from innocent words by
// substituting a chunk at run time.
#include <stdio.h>
#include <string.h>

#include "vocab_bank.h"
#include "vocab_cloze.h"
#include "vocab_config.h"
#include "vocab_test.h"
#include "vocab_view.h"

#define LIST_PATH       "assets/wordlists/common-words-35.txt"
#define COPYRIGHT_PATH  "assets/wordlists/SCOWL-Copyright.txt"
#define SOURCE_COMMIT   "1e5b7d3a72f47a71da5d28686c1dd4b397178485"
#define SOURCE_SHA256   "a5fe3f11682a6ab9d31f2ea2a185833b1a01be7af83409c671bd0756a870f6c7"
#define LIST_BUF_BYTES  (1024u * 1024u)
#define LIST_MAX_WORDS  65536u
#define COUNT_OF(a)     (sizeof(a) / sizeof((a)[0]))

typedef struct {
    const char *words[LIST_MAX_WORDS];
    size_t count;
} word_list_t;

static char g_list_buf[LIST_BUF_BYTES];
static char g_copyright_buf[16384];
static word_list_t g_list;

// ---------------------------------------------------------------------------------------
// The committed common-word list
// ---------------------------------------------------------------------------------------

// Reads a whole file into buf (NUL-terminated). Returns false when missing or too large.
static bool read_file(const char *path, char *buf, size_t cap, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    const size_t n = fread(buf, 1, cap - 1u, f);
    const bool complete = feof(f) != 0 && !ferror(f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return complete;
}

static bool is_word(const char *w)
{
    bool letter = false;
    for (const char *p = w; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
            letter = true;
        } else if (*p != '\'') {
            return false;
        }
    }
    return letter;
}

static bool list_has(void *ctx, const char *word)
{
    const word_list_t *list = (const word_list_t *)ctx;
    size_t lo = 0;
    size_t hi = list->count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2u;
        const int c = strcmp(list->words[mid], word);
        if (c == 0) return true;
        if (c < 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return false;
}

static void test_word_list(void)
{
    size_t len = 0;
    const bool ok = read_file(LIST_PATH, g_list_buf, sizeof(g_list_buf), &len);
    CHECK(ok, "cannot read %s completely (run from the repository root)", LIST_PATH);
    if (!ok) return;

    bool commit = false;
    bool sha = false;
    unsigned header_count = 0;
    unsigned bad_words = 0;
    unsigned unsorted = 0;
    char *p = g_list_buf;
    while (*p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p += strlen(p);
        }
        size_t n = strlen(line);
        if (n > 0 && line[n - 1u] == '\r') line[--n] = '\0';
        if (line[0] == '#') {
            if (strstr(line, SOURCE_COMMIT)) commit = true;
            if (strstr(line, SOURCE_SHA256)) sha = true;
            unsigned v = 0;
            if (sscanf(line, "# Words: %u", &v) == 1) header_count = v;
            continue;
        }
        if (!is_word(line)) {
            if (bad_words++ < 5u) CHECK(false, "list entry '%s' is not a lowercase [a-z'] word", line);
            continue;
        }
        if (g_list.count > 0 && strcmp(g_list.words[g_list.count - 1u], line) >= 0) {
            if (unsorted++ < 5u) CHECK(false, "list not strictly sorted at '%s'", line);
        }
        CHECK(g_list.count < LIST_MAX_WORDS, "list too long");
        if (g_list.count < LIST_MAX_WORDS) g_list.words[g_list.count++] = line;
    }
    CHECK(len > 0 && g_list_buf[0] != '\0', "list not empty");
    CHECK(commit && sha, "list header names the pinned commit and the source SHA-256");
    CHECK(header_count == g_list.count, "header count %u == %u words", header_count,
          (unsigned)g_list.count);
    CHECK(g_list.count >= 2000u, "at least 2,000 common words (spec 7.2), got %u",
          (unsigned)g_list.count);

    // The license notice travels with the list (AU entries included: the AU section too).
    CHECK(read_file(COPYRIGHT_PATH, g_copyright_buf, sizeof(g_copyright_buf), NULL),
          "%s readable", COPYRIGHT_PATH);
    CHECK(strstr(g_copyright_buf, "Kevin Atkinson") && strstr(g_copyright_buf, SOURCE_COMMIT) &&
          strstr(g_copyright_buf, "=== AU") && strstr(g_copyright_buf, "Permission to use"),
          "license notice with source commit and the AU section");

    static const char *const present[] = {
        "the", "and", "cat", "bus", "apple", "school", "morning", "cream", "big", "cut",
        "pair", "peer", "breed", "raid", "blew", "aardvark",
    };
    for (size_t i = 0; i < COUNT_OF(present); i++) {
        CHECK(list_has(&g_list, present[i]), "common word '%s' in the list", present[i]);
    }
    CHECK(!list_has(&g_list, "") && !list_has(&g_list, "Cat"), "lookup is exact");
}

// ---------------------------------------------------------------------------------------
// AC-23: every cloze entry of the bank passes rules 1-3; spec 12.2 substituted spellings
// ---------------------------------------------------------------------------------------

static void test_bank_entries(void)
{
    // Spec 12.2: the substituted spellings in bank order (never displayed, never words).
    static const char *const SUBSTITUTED[] = {
        "epple", "benana", "watormelon", "cet", "dack", "sheap", "tigor", "eleffant", "bas",
        "cer", "byke", "bowt", "trayn", "pancil", "bouk", "rulor", "schoulbag", "eer", "noze",
        "mowth", "hend", "fout", "bloo", "grean", "yallow", "melk", "woter", "jooce", "creem",
        "jamp", "swem", "seng", "sleap", "marning",
    };
    const vocab_bank_t *bank = vocab_bank_builtin();
    size_t with_cloze = 0;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *e = &bank->entries[i];
        vocab_cloze_t c;
        if (!vocab_cloze_from_entry(e, &c)) {
            CHECK(!vocab_entry_has_cloze(e), "uid %u: cloze data but no cloze", e->uid);
            continue;
        }
        with_cloze++;
        const vocab_cloze_verdict_t v = vocab_cloze_validate(&c, bank, list_has, &g_list);
        CHECK(v == VOCAB_CLOZE_OK, "uid %u '%s' (%u: %s / %s) rejected: %s", e->uid, e->text,
              c.pos, c.right, c.wrong, vocab_cloze_verdict_name(v));
        CHECK(vocab_cloze_validate(&c, NULL, NULL, NULL) == VOCAB_CLOZE_OK,
              "uid %u valid without lists", e->uid);
        CHECK(vocab_cloze_chunk_class(c.right) != VOCAB_CHUNK_NONE,
              "uid %u: right chunk '%s' is a spec 7.1 chunk", e->uid, c.right);

        char pre[VOCAB_VIEW_PART_CAP];
        char suf[VOCAB_VIEW_PART_CAP];
        CHECK(vocab_cloze_split(&c, pre, sizeof(pre), suf, sizeof(suf)), "uid %u split", e->uid);
        char joined[64];
        snprintf(joined, sizeof(joined), "%s%s%s", pre, c.right, suf);
        CHECK(strcmp(joined, e->text) == 0, "uid %u: pre + right + suf == text ('%s')", e->uid,
              joined);
        CHECK(strlen(c.right) < VOCAB_VIEW_CHUNK_CAP && strlen(c.wrong) < VOCAB_VIEW_CHUNK_CAP,
              "uid %u: chunks fit the view", e->uid);

        char token[48];
        CHECK(vocab_cloze_token(&c, c.wrong, token, sizeof(token)), "uid %u token", e->uid);
        CHECK(!list_has(&g_list, token) && !vocab_cloze_denied(token),
              "uid %u: substituted '%s' is not a word", e->uid, token);
        if (with_cloze <= COUNT_OF(SUBSTITUTED)) {
            CHECK(strcmp(token, SUBSTITUTED[with_cloze - 1u]) == 0,
                  "uid %u: substituted '%s', spec 12.2 lists '%s'", e->uid, token,
                  SUBSTITUTED[with_cloze - 1u]);
        }
        char original[48];
        CHECK(vocab_cloze_token(&c, c.right, original, sizeof(original)) &&
              list_has(&g_list, original) == (strcmp(original, "schoolbag") != 0),
              "uid %u: the real token '%s' is a common word (schoolbag excepted)", e->uid,
              original);
    }
    CHECK(with_cloze == 34u, "34 cloze entries (got %u)", (unsigned)with_cloze);
}

// Spec 7.2 rule 4: negative cases that MUST be rejected.
static void test_negative_cases(void)
{
    static const struct {
        const char *text;
        uint8_t pos;
        const char *right;
        const char *wrong;
        const char *token;
        vocab_cloze_verdict_t verdict;  // without the bank (rule 2 before rule 3)
    } NEG[] = {
        { "bag", 1, "a", "i", "big", VOCAB_CLOZE_ERR_COMMON_WORD },
        { "cat", 1, "a", "u", "cut", VOCAB_CLOZE_ERR_COMMON_WORD },
        { "pear", 1, "ea", "ai", "pair", VOCAB_CLOZE_ERR_PAIR },
        { "pear", 1, "ea", "ee", "peer", VOCAB_CLOZE_ERR_COMMON_WORD },
        { "bread", 2, "ea", "ee", "breed", VOCAB_CLOZE_ERR_COMMON_WORD },
        { "read", 1, "ea", "ai", "raid", VOCAB_CLOZE_ERR_PAIR },
        { "blue", 2, "ue", "ew", "blew", VOCAB_CLOZE_ERR_PAIR },
    };
    const vocab_bank_t *bank = vocab_bank_builtin();
    for (size_t i = 0; i < COUNT_OF(NEG); i++) {
        const vocab_cloze_t c = { NEG[i].text, NEG[i].pos, NEG[i].right, NEG[i].wrong };
        const vocab_cloze_verdict_t v = vocab_cloze_validate(&c, NULL, list_has, &g_list);
        CHECK(v == NEG[i].verdict, "%s %s->%s: %s (expected %s)", NEG[i].text, NEG[i].right,
              NEG[i].wrong, vocab_cloze_verdict_name(v), vocab_cloze_verdict_name(NEG[i].verdict));
        CHECK(vocab_cloze_validate(&c, bank, list_has, &g_list) != VOCAB_CLOZE_OK,
              "%s %s->%s rejected with the bank", NEG[i].text, NEG[i].right, NEG[i].wrong);
        // Rule 3 alone would also reject every case: the token is a common word.
        char token[16];
        CHECK(vocab_cloze_token(&c, NEG[i].wrong, token, sizeof(token)) &&
              strcmp(token, NEG[i].token) == 0 && list_has(&g_list, token),
              "%s -> '%s' is a common word", NEG[i].text, NEG[i].token);
    }
    // "big" is also a token of the bank example "The tiger is big." (rule 3, bank part).
    const vocab_cloze_t bag = { "bag", 1, "a", "i" };
    CHECK(vocab_cloze_validate(&bag, bank, list_has, &g_list) == VOCAB_CLOZE_ERR_BANK_WORD,
          "bag a->i hits the bank example token 'big'");
    // The real cat entry with the lure of rule 4.
    vocab_index_t cat = 0;
    CHECK(vocab_bank_find(bank, 201, &cat), "cat in the bank");
    vocab_cloze_t c;
    CHECK(vocab_cloze_from_entry(&bank->entries[cat], &c), "cat has cloze data");
    c.wrong = "u";
    CHECK(vocab_cloze_validate(&c, bank, list_has, &g_list) == VOCAB_CLOZE_ERR_COMMON_WORD,
          "cat with u -> cut rejected");
}

// ---------------------------------------------------------------------------------------
// Rule 1 (shape), split and token
// ---------------------------------------------------------------------------------------

static void test_shape(void)
{
    static const vocab_cloze_t BAD[] = {
        { NULL, 0, "a", "e" },        // no text
        { "bus", 1, NULL, "a" },      // no right
        { "bus", 1, "u", NULL },      // no wrong
        { "bus", 1, "", "" },         // empty chunks
        { "watermelon", 3, "ermel", "ormel" },  // longer than VOCAB_CLOZE_CHUNK_MAX
        { "sheep", 2, "ee", "e" },    // unequal lengths
        { "bus", 1, "u", "u" },       // equal chunks
        { "Bus", 0, "B", "P" },       // uppercase chunks
        { "bus", 1, "u", "A" },       // uppercase wrong chunk
        { "bus", 1, "u", "-" },       // not a letter
        { "ice cream", 3, " ", "a" }, // a space is not a chunk
        { "bus", 1, "a", "e" },       // text mismatch at pos
        { "bus", 2, "us", "as" },     // runs past the end
        { "bus", 200, "u", "a" },     // pos beyond the end
    };
    for (size_t i = 0; i < COUNT_OF(BAD); i++) {
        CHECK(vocab_cloze_validate(&BAD[i], NULL, NULL, NULL) == VOCAB_CLOZE_ERR_SHAPE,
              "bad shape %u rejected", (unsigned)i);
        char pre[8] = "x";
        char suf[8] = "x";
        CHECK(!vocab_cloze_split(&BAD[i], pre, sizeof(pre), suf, sizeof(suf)) && pre[0] == '\0' &&
              suf[0] == '\0', "bad shape %u: split fails with empty parts", (unsigned)i);
    }
    CHECK(vocab_cloze_validate(NULL, NULL, NULL, NULL) == VOCAB_CLOZE_ERR_SHAPE, "NULL cloze");
    const vocab_cloze_t three = { "high", 1, "igh", "eye" };  // 3 letters is a legal shape
    CHECK(vocab_cloze_validate(&three, NULL, NULL, NULL) == VOCAB_CLOZE_ERR_PAIR,
          "3-letter chunks pass rule 1 and fail rule 2");

    vocab_entry_t plain = { 0 };
    vocab_cloze_t out = { "keep", 9, "k", "c" };
    CHECK(!vocab_cloze_from_entry(&plain, &out) && strcmp(out.text, "keep") == 0 && out.pos == 9,
          "no cloze data: false, out untouched");
    CHECK(!vocab_cloze_from_entry(NULL, &out), "NULL entry");
}

static void test_split(void)
{
    static const struct {
        vocab_cloze_t c;
        const char *pre;
        const char *suf;
    } CASES[] = {
        { { "bus", 1, "u", "a" }, "b", "s" },
        { { "apple", 0, "a", "e" }, "", "pple" },
        { { "ear", 0, "ea", "ee" }, "", "r" },
        { { "tiger", 3, "er", "or" }, "tig", "" },
        { { "ice cream", 6, "ea", "ee" }, "ice cr", "m" },      // phrase: the space is kept
        { { "Good morning", 6, "or", "ar" }, "Good m", "ning" },
        { { "schoolbag", 3, "oo", "ou" }, "sch", "lbag" },
    };
    for (size_t i = 0; i < COUNT_OF(CASES); i++) {
        char pre[VOCAB_VIEW_PART_CAP];
        char suf[VOCAB_VIEW_PART_CAP];
        CHECK(vocab_cloze_split(&CASES[i].c, pre, sizeof(pre), suf, sizeof(suf)) &&
              strcmp(pre, CASES[i].pre) == 0 && strcmp(suf, CASES[i].suf) == 0,
              "split '%s' -> '%s' + '%s' (got '%s' + '%s')", CASES[i].c.text, CASES[i].pre,
              CASES[i].suf, pre, suf);
    }
    // Buffer limits: exact fit works, one byte less fails with empty outputs.
    const vocab_cloze_t gm = { "Good morning", 6, "or", "ar" };
    char pre[7];
    char suf[5];
    CHECK(vocab_cloze_split(&gm, pre, 7, suf, 5), "exact-fit buffers");
    CHECK(!vocab_cloze_split(&gm, pre, 6, suf, 5) && pre[0] == '\0' && suf[0] == '\0',
          "prefix buffer too small");
    CHECK(!vocab_cloze_split(&gm, pre, 7, suf, 4) && pre[0] == '\0' && suf[0] == '\0',
          "suffix buffer too small");
    CHECK(!vocab_cloze_split(&gm, NULL, 0, suf, 5) && suf[0] == '\0', "NULL prefix buffer");
}

static void test_token(void)
{
    static const struct {
        vocab_cloze_t c;
        const char *chunk;
        const char *token;
    } CASES[] = {
        { { "bag", 1, "a", "i" }, "i", "big" },
        { { "ice cream", 6, "ea", "ee" }, "ee", "creem" },
        { { "ice cream", 6, "ea", "ee" }, "ea", "cream" },
        { { "Good morning", 6, "or", "ar" }, "ar", "marning" },
        { { "Good morning", 1, "oo", "ou" }, "ou", "goud" },         // lowercased
        { { "Let's go", 1, "e", "a" }, "a", "lat's" },               // apostrophes join
        { { "Nice to meet you", 9, "ee", "ea" }, "ea", "meat" },
        { { "How are you?", 9, "o", "a" }, "a", "yau" },             // punctuation delimits
        { { "ear", 0, "ea", "ee" }, "ee", "eer" },
    };
    for (size_t i = 0; i < COUNT_OF(CASES); i++) {
        char out[32];
        CHECK(vocab_cloze_token(&CASES[i].c, CASES[i].chunk, out, sizeof(out)) &&
              strcmp(out, CASES[i].token) == 0, "token '%s' + '%s' -> '%s' (got '%s')",
              CASES[i].c.text, CASES[i].chunk, CASES[i].token, out);
    }
    const vocab_cloze_t bag = { "bag", 1, "a", "i" };
    char out[4];
    CHECK(vocab_cloze_token(&bag, "i", out, 4) && strcmp(out, "big") == 0, "exact-fit token");
    CHECK(!vocab_cloze_token(&bag, "i", out, 3) && out[0] == '\0', "token buffer too small");
    CHECK(!vocab_cloze_token(&bag, "ii", out, 4), "chunk length mismatch");
    CHECK(!vocab_cloze_token(&bag, "1", out, 4), "chunk must be letters");
    const vocab_cloze_t bad = { "bag", 1, "o", "i" };
    CHECK(!vocab_cloze_token(&bad, "i", out, 4), "text mismatch at pos");
    CHECK(!vocab_cloze_token(NULL, "i", out, 4) && !vocab_cloze_token(&bag, NULL, out, 4) &&
          !vocab_cloze_token(&bag, "i", NULL, 4), "NULL arguments");
}

// ---------------------------------------------------------------------------------------
// Rule 2: the whitelist, exhaustively for one- and two-letter chunks
// ---------------------------------------------------------------------------------------

static void test_pairs(void)
{
    static const char *const ALLOWED[][2] = {
        { "a", "e" }, { "a", "i" }, { "a", "o" }, { "a", "u" }, { "e", "i" }, { "e", "o" },
        { "e", "u" }, { "i", "o" }, { "i", "u" }, { "o", "u" }, { "i", "y" }, { "s", "z" },
        { "c", "k" }, { "c", "s" }, { "ph", "ff" }, { "ee", "ea" }, { "oo", "ou" },
        { "oa", "ow" }, { "ou", "ow" }, { "ai", "ay" }, { "ue", "oo" }, { "ui", "oo" },
        { "ar", "er" }, { "ar", "ir" }, { "ar", "or" }, { "ar", "ur" }, { "er", "ir" },
        { "er", "or" }, { "er", "ur" }, { "ir", "or" }, { "ir", "ur" }, { "or", "ur" },
        { "sh", "ch" },
    };
    for (size_t i = 0; i < COUNT_OF(ALLOWED); i++) {
        CHECK(vocab_cloze_pair_allowed(ALLOWED[i][0], ALLOWED[i][1]) &&
              vocab_cloze_pair_allowed(ALLOWED[i][1], ALLOWED[i][0]),
              "%s/%s allowed in both orders", ALLOWED[i][0], ALLOWED[i][1]);
    }
    CHECK(vocab_cloze_pair_allowed("EE", "eA") && vocab_cloze_pair_allowed("Sh", "CH"),
          "case-insensitive");
    static const char *const REJECTED[][2] = {
        { "a", "y" }, { "ea", "ai" }, { "ue", "ew" }, { "ee", "oo" }, { "th", "ff" },
        { "b", "p" }, { "sh", "th" }, { "ar", "ar" }, { "a", "A" }, { "k", "s" }, { "z", "c" },
        { "ck", "kk" }, { "oa", "ou" }, { "ai", "ei" }, { "igh", "eye" }, { "ee", "e" },
        { "abcd", "abce" }, { "", "" }, { "-", "a" },
    };
    for (size_t i = 0; i < COUNT_OF(REJECTED); i++) {
        CHECK(!vocab_cloze_pair_allowed(REJECTED[i][0], REJECTED[i][1]),
              "%s/%s not whitelisted", REJECTED[i][0], REJECTED[i][1]);
    }
    CHECK(!vocab_cloze_pair_allowed(NULL, "a") && !vocab_cloze_pair_allowed("a", NULL), "NULL");

    // Exhaustive: exactly 10 vowel pairs + i/y + s/z + c/k + c/s = 14 unordered single-letter
    // pairs, and 9 listed + 10 r-controlled = 19 unordered two-letter pairs.
    unsigned singles = 0;
    unsigned doubles = 0;
    char a[3] = { 0 };
    char b[3] = { 0 };
    for (char x = 'a'; x <= 'z'; x++) {
        for (char y = 'a'; y <= 'z'; y++) {
            a[0] = x;
            b[0] = y;
            a[1] = b[1] = '\0';
            if (vocab_cloze_pair_allowed(a, b)) singles++;
        }
    }
    for (int i = 0; i < 26 * 26; i++) {
        for (int j = 0; j < 26 * 26; j++) {
            a[0] = (char)('a' + i / 26);
            a[1] = (char)('a' + i % 26);
            b[0] = (char)('a' + j / 26);
            b[1] = (char)('a' + j % 26);
            if (vocab_cloze_pair_allowed(a, b)) doubles++;
        }
    }
    CHECK(singles == 28u, "28 ordered single-letter pairs (got %u)", singles);
    CHECK(doubles == 38u, "38 ordered two-letter pairs (got %u)", doubles);
}

// ---------------------------------------------------------------------------------------
// Rule 3: bank tokens and the deny-list
// ---------------------------------------------------------------------------------------

static void test_bank_word_rule(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    static const struct {
        vocab_cloze_t c;
        vocab_cloze_verdict_t with_bank;
        vocab_cloze_verdict_t without_bank;  // common list only
    } CASES[] = {
        // Example token ("The sky is blue."), also a common word.
        { { "ski", 2, "i", "y" }, VOCAB_CLOZE_ERR_BANK_WORD, VOCAB_CLOZE_ERR_COMMON_WORD },
        // Case-insensitive against "Good morning" / "Goodbye".
        { { "Goud", 1, "ou", "oo" }, VOCAB_CLOZE_ERR_BANK_WORD, VOCAB_CLOZE_ERR_COMMON_WORD },
        { { "Good marning", 6, "ar", "or" }, VOCAB_CLOZE_ERR_BANK_WORD,
          VOCAB_CLOZE_ERR_COMMON_WORD },
        // A bank word missing from the size-35 list: only the bank check stops it.
        { { "schoulbag", 3, "ou", "oo" }, VOCAB_CLOZE_ERR_BANK_WORD, VOCAB_CLOZE_OK },
        // Whole tokens only: "cre" is part of "cream", not a token.
        { { "cra", 2, "a", "e" }, VOCAB_CLOZE_OK, VOCAB_CLOZE_OK },
    };
    for (size_t i = 0; i < COUNT_OF(CASES); i++) {
        const vocab_cloze_verdict_t a = vocab_cloze_validate(&CASES[i].c, bank, list_has, &g_list);
        const vocab_cloze_verdict_t b = vocab_cloze_validate(&CASES[i].c, NULL, list_has, &g_list);
        CHECK(a == CASES[i].with_bank && b == CASES[i].without_bank,
              "'%s' %s->%s: %s / %s", CASES[i].c.text, CASES[i].c.right, CASES[i].c.wrong,
              vocab_cloze_verdict_name(a), vocab_cloze_verdict_name(b));
    }
}

static void test_deny_list(void)
{
    // Innocent words whose substitution forms a denied token. The second group is NOT in the
    // size-35 list, so only the deny-list stops it.
    static const struct {
        vocab_cloze_t c;
        bool in_common_list;
    } CASES[] = {
        { { "where", 2, "e", "o" }, true },   { { "six", 1, "i", "e" }, true },
        { { "hall", 1, "a", "e" }, true },    { { "slot", 2, "o", "u" }, true },
        { { "chunk", 2, "u", "i" }, true },   { { "dike", 1, "i", "y" }, true },
        { { "shot", 2, "o", "i" }, false },   { { "shut", 2, "u", "i" }, false },
        { { "duck", 1, "u", "i" }, false },   { { "deck", 1, "e", "i" }, false },
        { { "fort", 1, "or", "ar" }, false }, { { "pass", 1, "a", "i" }, false },
        { { "pea", 1, "ea", "ee" }, false },  { { "fog", 1, "o", "a" }, false },
        { { "twit", 2, "i", "a" }, false },   { { "wink", 1, "i", "a" }, false },
        { { "fredge", 2, "e", "i" }, false }, { { "jeap", 1, "ea", "ee" }, false },
        { { "tefu", 1, "e", "o" }, false },   { { "yammy", 1, "a", "u" }, false },
        { { "taddy", 1, "a", "e" }, false },  { { "grandpo", 6, "o", "a" }, false },
    };
    for (size_t i = 0; i < COUNT_OF(CASES); i++) {
        const vocab_cloze_t *c = &CASES[i].c;
        char token[16];
        CHECK(vocab_cloze_token(c, c->wrong, token, sizeof(token)), "case %u token", (unsigned)i);
        CHECK(vocab_cloze_denied(token), "case %u ('%s' %s->%s) denied", (unsigned)i, c->text,
              c->right, c->wrong);
        char upper[16];
        for (size_t k = 0; k < sizeof(upper); k++) {
            const char ch = token[k];
            upper[k] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
            if (ch == '\0') break;
        }
        CHECK(vocab_cloze_denied(upper), "case %u denied in upper case", (unsigned)i);
        CHECK(list_has(&g_list, token) == CASES[i].in_common_list,
              "case %u: common-list membership as documented", (unsigned)i);
        CHECK(vocab_cloze_validate(c, NULL, list_has, &g_list) == VOCAB_CLOZE_ERR_DENIED,
              "case %u: the deny-list is checked before the common list", (unsigned)i);
        CHECK(vocab_cloze_validate(c, NULL, NULL, NULL) == VOCAB_CLOZE_ERR_DENIED,
              "case %u: the deny-list works without the word list", (unsigned)i);
        CHECK(!vocab_cloze_denied(c->text), "case %u: the original word is not denied",
              (unsigned)i);
    }
    // Exact tokens only, never substrings; every bank token passes.
    static const char *const INNOCENT[] = {
        "class", "scrap", "hello", "shell", "assess", "cocktail", "title", "therapist", "sussex",
        "pencil", "duck", "deck", "cat", "a", "i",
    };
    for (size_t i = 0; i < COUNT_OF(INNOCENT); i++) {
        CHECK(!vocab_cloze_denied(INNOCENT[i]), "'%s' not denied", INNOCENT[i]);
    }
    const vocab_bank_t *bank = vocab_bank_builtin();
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const char *fields[2] = { bank->entries[i].text, bank->entries[i].example };
        for (size_t f = 0; f < 2u; f++) {
            char token[32];
            size_t n = 0;
            for (const char *p = fields[f];; p++) {
                const bool letter = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '\'';
                if (letter && n + 1u < sizeof(token)) {
                    token[n++] = *p;
                    continue;
                }
                if (n > 0) {
                    token[n] = '\0';
                    CHECK(!vocab_cloze_denied(token), "bank token '%s' not denied", token);
                    n = 0;
                }
                if (*p == '\0') break;
            }
        }
    }
    CHECK(!vocab_cloze_denied(NULL) && !vocab_cloze_denied(""), "NULL / empty");
    char longword[80];
    memset(longword, 'a', sizeof(longword) - 1u);
    longword[sizeof(longword) - 1u] = '\0';
    CHECK(!vocab_cloze_denied(longword), "long word");
}

// ---------------------------------------------------------------------------------------
// Spec 7.1: chunk classes and the priority chooser
// ---------------------------------------------------------------------------------------

static void test_chunk_class(void)
{
    static const char *const TEAMS[] = { "ee", "ea", "oo", "ou", "ai", "ay", "oa", "ow", "ue",
                                         "ui", "ar", "er", "ir", "or", "ur" };
    static const char *const DIGRAPHS[] = { "sh", "ch", "th", "ph", "ck", "wh" };
    static const char *const SINGLES[] = { "a", "e", "i", "o", "u", "y", "s", "z", "c", "k" };
    static const char *const NONE[] = { "b", "ff", "pp", "ll", "ng", "igh", "eee", "EE", "A", "",
                                        "e e", "tch" };
    for (size_t i = 0; i < COUNT_OF(TEAMS); i++) {
        CHECK(vocab_cloze_chunk_class(TEAMS[i]) == VOCAB_CHUNK_VOWEL_TEAM, "%s team", TEAMS[i]);
    }
    for (size_t i = 0; i < COUNT_OF(DIGRAPHS); i++) {
        CHECK(vocab_cloze_chunk_class(DIGRAPHS[i]) == VOCAB_CHUNK_DIGRAPH, "%s digraph",
              DIGRAPHS[i]);
    }
    for (size_t i = 0; i < COUNT_OF(SINGLES); i++) {
        CHECK(vocab_cloze_chunk_class(SINGLES[i]) == VOCAB_CHUNK_SINGLE, "%s single", SINGLES[i]);
    }
    for (size_t i = 0; i < COUNT_OF(NONE); i++) {
        CHECK(vocab_cloze_chunk_class(NONE[i]) == VOCAB_CHUNK_NONE, "'%s' not a chunk", NONE[i]);
    }
    CHECK(vocab_cloze_chunk_class(NULL) == VOCAB_CHUNK_NONE, "NULL");
}

// Curated bank entries that deliberately differ from the spec 7.1 automatic choice
// ("curated entries may override with a recorded reason"). Requested: record these reasons
// next to the entries in main/vocab_bank.c (lead-owned).
static const struct {
    vocab_uid_t uid;
    const char *reason;
} CURATED_OVERRIDES[] = {
    { 303, "bike: magic-e long i sounds like y (byke); i/e would test a short vowel" },
    { 502, "nose: the s says /z/, the real spelling trap; o/a is a weaker lure" },
    { 604, "yellow: e/a (yallow) is the classic vowel trap; the final ow is easy" },
    { 704, "water: a says /o:/, the real trap (woter); the final er is a weak schwa" },
    { 901, "Good morning: practise the content word morning, not the known Good" },
};

static const char *override_reason(vocab_uid_t uid)
{
    for (size_t i = 0; i < COUNT_OF(CURATED_OVERRIDES); i++) {
        if (CURATED_OVERRIDES[i].uid == uid) return CURATED_OVERRIDES[i].reason;
    }
    return NULL;
}

static void test_choose(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    static const struct {
        const char *text;
        bool found;
        uint8_t pos;
        const char *right;
        const char *wrong;
    } CASES[] = {
        { "sheep", true, 2, "ee", "ea" },     // vowel team before the sh digraph
        { "elephant", true, 3, "ph", "ff" },  // digraph before single vowels
        { "cat", true, 1, "a", "e" },         // vowels before c/k although c comes first
        { "duck", true, 1, "u", "a" },        // ck has no partner; its c is not a single
        { "bag", false, 0, NULL, NULL },      // beg, bug, bog, big: all real words
        { "ice cream", true, 6, "ea", "ee" }, // phrase: creem
        { "", false, 0, NULL, NULL },
        { "hmm", false, 0, NULL, NULL },      // no eligible chunk at all
    };
    for (size_t i = 0; i < COUNT_OF(CASES); i++) {
        vocab_cloze_t out = { NULL, 0, NULL, NULL };
        const bool found = vocab_cloze_choose(CASES[i].text, bank, list_has, &g_list, &out);
        CHECK(found == CASES[i].found, "choose '%s' found=%d", CASES[i].text, (int)found);
        if (found && CASES[i].found) {
            CHECK(out.text == CASES[i].text && out.pos == CASES[i].pos &&
                  strcmp(out.right, CASES[i].right) == 0 && strcmp(out.wrong, CASES[i].wrong) == 0,
                  "choose '%s' -> %u %s/%s (got %u %s/%s)", CASES[i].text, CASES[i].pos,
                  CASES[i].right, CASES[i].wrong, out.pos, out.right, out.wrong);
        }
    }
    vocab_cloze_t out;
    CHECK(!vocab_cloze_choose(NULL, bank, NULL, NULL, &out) &&
          !vocab_cloze_choose("cat", bank, NULL, NULL, NULL), "NULL arguments");

    // Every curated cloze entry either is the automatic choice or a recorded override.
    unsigned overrides = 0;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *e = &bank->entries[i];
        vocab_cloze_t curated;
        if (!vocab_cloze_from_entry(e, &curated)) continue;
        vocab_cloze_t auto_pick;
        const bool found = vocab_cloze_choose(e->text, bank, list_has, &g_list, &auto_pick);
        CHECK(found, "uid %u: a valid automatic choice exists", e->uid);
        if (!found) continue;
        CHECK(vocab_cloze_validate(&auto_pick, bank, list_has, &g_list) == VOCAB_CLOZE_OK,
              "uid %u: the automatic choice validates", e->uid);
        const bool same = auto_pick.pos == curated.pos &&
                          strcmp(auto_pick.right, curated.right) == 0 &&
                          strcmp(auto_pick.wrong, curated.wrong) == 0;
        const char *reason = override_reason(e->uid);
        CHECK(same || reason, "uid %u '%s': curated %u %s/%s differs from 7.1 choice %u %s/%s "
              "without a recorded reason", e->uid, e->text, curated.pos, curated.right,
              curated.wrong, auto_pick.pos, auto_pick.right, auto_pick.wrong);
        CHECK(!(same && reason), "uid %u: stale override entry", e->uid);
        if (!same) overrides++;
    }
    CHECK(overrides == COUNT_OF(CURATED_OVERRIDES), "%u overrides recorded", overrides);
}

// ---------------------------------------------------------------------------------------
// Spec 7.3 / AC-25: blank box = max(width(right), width(wrong)) + 8 with stub widths
// ---------------------------------------------------------------------------------------

// Stub proportional font: narrow i/l/j/space, wide m/w, scaled per word size.
static int32_t stub_measure(void *ctx, vocab_word_size_t size, const char *text)
{
    (void)ctx;
    int32_t w = 0;
    for (const char *p = text; *p; p++) {
        switch (*p) {
        case 'i': case 'l': case 'j': case '\'': w += 6; break;
        case ' ': w += 7; break;
        case 'm': case 'w': case 'M': case 'W': w += 22; break;
        case 'G': case 'o': case 'u': w += 15; break;
        default: w += 13; break;
        }
    }
    return size == VOCAB_WORD_SIZE_L ? w * 9 / 7 : (size == VOCAB_WORD_SIZE_S ? w * 11 / 14 : w);
}

static int32_t negative_measure(void *ctx, vocab_word_size_t size, const char *text)
{
    (void)ctx;
    (void)size;
    (void)text;
    return -5;
}

static void test_row_layout(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    static const vocab_word_size_t SIZES[] = { VOCAB_WORD_SIZE_L, VOCAB_WORD_SIZE_M,
                                               VOCAB_WORD_SIZE_S };
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *e = &bank->entries[i];
        vocab_cloze_t c;
        if (!vocab_cloze_from_entry(e, &c)) continue;
        char pre[VOCAB_VIEW_PART_CAP];
        char suf[VOCAB_VIEW_PART_CAP];
        CHECK(vocab_cloze_split(&c, pre, sizeof(pre), suf, sizeof(suf)), "split %u", e->uid);
        for (size_t s = 0; s < COUNT_OF(SIZES); s++) {
            const vocab_word_size_t size = SIZES[s];
            const vocab_cloze_row_t row =
                vocab_cloze_row_layout(pre, c.right, c.wrong, suf, size, 120, stub_measure, NULL);
            const int32_t wr = stub_measure(NULL, size, c.right);
            const int32_t ww = stub_measure(NULL, size, c.wrong);
            const int32_t expect = (wr > ww ? wr : ww) + VOCAB_BLANK_PAD_PX;
            CHECK(row.box_w == expect, "uid %u size %d: box %d == max(%d, %d) + 8", e->uid,
                  (int)size, (int)row.box_w, (int)wr, (int)ww);
            CHECK(row.pre_w == (pre[0] ? stub_measure(NULL, size, pre) : 0) &&
                  row.suf_w == (suf[0] ? stub_measure(NULL, size, suf) : 0) &&
                  row.total_w == row.pre_w + row.box_w + row.suf_w &&
                  row.x == 120 - row.total_w / 2 && row.box_x == row.x + row.pre_w &&
                  row.suf_x == row.box_x + row.box_w, "uid %u size %d: row arithmetic", e->uid,
                  (int)size);
            // Swapping the chunks (the other side being correct) changes nothing: no leak.
            const vocab_cloze_row_t swapped =
                vocab_cloze_row_layout(pre, c.wrong, c.right, suf, size, 120, stub_measure, NULL);
            CHECK(memcmp(&row, &swapped, sizeof(row)) == 0, "uid %u: geometry ignores which "
                  "chunk is right", e->uid);
            CHECK(row.box_w >= wr + VOCAB_BLANK_PAD_PX && row.box_w >= ww + VOCAB_BLANK_PAD_PX,
                  "uid %u: both chunks fit the box", e->uid);
        }
    }
    // Different chunk widths: the wider one sets the box (bike i/y: 6 vs 13 px).
    const vocab_cloze_row_t bike =
        vocab_cloze_row_layout("b", "i", "y", "ke", VOCAB_WORD_SIZE_M, 120, stub_measure, NULL);
    CHECK(bike.box_w == 13 + VOCAB_BLANK_PAD_PX && bike.pre_w == 13 && bike.suf_w == 26 &&
          bike.total_w == 60 && bike.x == 90, "bike row");
    const vocab_cloze_row_t empty =
        vocab_cloze_row_layout(NULL, NULL, NULL, NULL, VOCAB_WORD_SIZE_M, 120, stub_measure, NULL);
    CHECK(empty.box_w == VOCAB_BLANK_PAD_PX && empty.total_w == VOCAB_BLANK_PAD_PX &&
          empty.x == 116, "NULL strings measure 0");
    const vocab_cloze_row_t clamped =
        vocab_cloze_row_layout("a", "b", "c", "d", VOCAB_WORD_SIZE_M, 120, negative_measure, NULL);
    CHECK(clamped.pre_w == 0 && clamped.box_w == VOCAB_BLANK_PAD_PX && clamped.suf_w == 0,
          "negative widths clamp to 0");
    const vocab_cloze_row_t none =
        vocab_cloze_row_layout("a", "b", "c", "d", VOCAB_WORD_SIZE_M, 120, NULL, NULL);
    CHECK(none.x == 0 && none.box_w == 0 && none.total_w == 0, "NULL measure -> zero row");
}

int main(void)
{
    test_word_list();
    test_bank_entries();
    test_negative_cases();
    test_shape();
    test_split();
    test_token();
    test_pairs();
    test_bank_word_rule();
    test_deny_list();
    test_chunk_class();
    test_choose();
    test_row_layout();
    CHECK(strcmp(vocab_cloze_verdict_name(VOCAB_CLOZE_OK), "ok") == 0 &&
          strcmp(vocab_cloze_verdict_name(VOCAB_CLOZE_ERR_DENIED), "denied") == 0 &&
          strcmp(vocab_cloze_verdict_name((vocab_cloze_verdict_t)99), "unknown") == 0,
          "verdict names");
    return vocab_test_report("vocab cloze tests");
}
