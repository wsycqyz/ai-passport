// main/vocab_cloze.c - Stage 3 cloze data helpers and the cloze validator (see vocab_cloze.h).
//
// Spec 7.1 (chunk priority), 7.2 (wrong-chunk rules), 7.3 (skeleton row). Pure C11: no
// allocation, no static mutable state, no clock, no RNG. Only vocab_cloze_from_entry() and
// vocab_cloze_split() run on the device (Stage 3); the validator, the chooser and the
// deny-list serve the host tests and the deferred word-bank pipeline.
#include "vocab_cloze.h"

#include <string.h>

// Large enough for any token of a bank text or example (<= 30 chars, spec 12.1).
#define TOKEN_CAP 48u
#define CHUNK_CAP (VOCAB_CLOZE_CHUNK_MAX + 1u)
#define PARTNERS_MAX 5u
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

// ---------------------------------------------------------------------------------------
// Character helpers (ASCII only, locale independent)
// ---------------------------------------------------------------------------------------

static bool is_lower(char c)
{
    return c >= 'a' && c <= 'z';
}

static bool is_alpha(char c)
{
    return is_lower(c) || (c >= 'A' && c <= 'Z');
}

static char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_token_char(char c)
{
    return is_alpha(c) || c == '\'';
}

// Length of `s` when it is a chunk of 1..VOCAB_CLOZE_CHUNK_MAX lowercase letters, else 0.
static size_t chunk_len(const char *s)
{
    if (!s) return 0;
    size_t n = 0;
    while (s[n] != '\0') {
        if (n >= VOCAB_CLOZE_CHUNK_MAX || !is_lower(s[n])) return 0;
        n++;
    }
    return n;
}

// Copies a 1..VOCAB_CLOZE_CHUNK_MAX letter chunk (any case) lowercased into `dst`.
static bool lower_chunk(const char *src, char dst[CHUNK_CAP])
{
    if (!src) return false;
    size_t n = 0;
    while (src[n] != '\0') {
        if (n >= VOCAB_CLOZE_CHUNK_MAX || !is_alpha(src[n])) return false;
        dst[n] = to_lower(src[n]);
        n++;
    }
    dst[n] = '\0';
    return n > 0;
}

// Spec 7.2 rule 1.
static bool shape_ok(const vocab_cloze_t *c)
{
    if (!c || !c->text) return false;
    const size_t len = chunk_len(c->right);
    if (len == 0 || chunk_len(c->wrong) != len || strcmp(c->right, c->wrong) == 0) return false;
    if ((size_t)c->pos + len > strlen(c->text)) return false;
    return memcmp(c->text + c->pos, c->right, len) == 0;
}

// ---------------------------------------------------------------------------------------
// Runtime helpers
// ---------------------------------------------------------------------------------------

bool vocab_cloze_from_entry(const vocab_entry_t *entry, vocab_cloze_t *out)
{
    if (!entry || !entry->text || !entry->cloze_right || !entry->cloze_wrong || !out) return false;
    out->text = entry->text;
    out->pos = entry->cloze_pos;
    out->right = entry->cloze_right;
    out->wrong = entry->cloze_wrong;
    return true;
}

bool vocab_cloze_split(const vocab_cloze_t *cloze, char *pre, size_t pre_cap,
                       char *suf, size_t suf_cap)
{
    if (pre && pre_cap) pre[0] = '\0';
    if (suf && suf_cap) suf[0] = '\0';
    if (!shape_ok(cloze) || !pre || !suf) return false;
    const size_t pre_len = cloze->pos;
    const char *rest = cloze->text + cloze->pos + strlen(cloze->right);
    const size_t suf_len = strlen(rest);
    if (pre_len >= pre_cap || suf_len >= suf_cap) return false;
    memcpy(pre, cloze->text, pre_len);
    pre[pre_len] = '\0';
    memcpy(suf, rest, suf_len + 1u);
    return true;
}

// ---------------------------------------------------------------------------------------
// Rule 2: phonetic-confusion whitelist
// ---------------------------------------------------------------------------------------

static const char *const VOWELS[] = { "a", "e", "i", "o", "u" };
static const char *const R_CONTROLLED[] = { "ar", "er", "ir", "or", "ur" };
static const char *const PAIRS[][2] = {
    { "i", "y" },   { "s", "z" },   { "c", "k" },   { "c", "s" },   { "ph", "ff" },
    { "ee", "ea" }, { "oo", "ou" }, { "oa", "ow" }, { "ou", "ow" }, { "ai", "ay" },
    { "ue", "oo" }, { "ui", "oo" }, { "sh", "ch" },
};

static bool in_set(const char *s, const char *const *set, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(s, set[i]) == 0) return true;
    }
    return false;
}

bool vocab_cloze_pair_allowed(const char *right, const char *wrong)
{
    char a[CHUNK_CAP];
    char b[CHUNK_CAP];
    if (!lower_chunk(right, a) || !lower_chunk(wrong, b) || strcmp(a, b) == 0) return false;
    if (in_set(a, VOWELS, COUNT_OF(VOWELS)) && in_set(b, VOWELS, COUNT_OF(VOWELS))) return true;
    if (in_set(a, R_CONTROLLED, COUNT_OF(R_CONTROLLED)) &&
        in_set(b, R_CONTROLLED, COUNT_OF(R_CONTROLLED))) {
        return true;
    }
    for (size_t i = 0; i < COUNT_OF(PAIRS); i++) {
        if ((strcmp(a, PAIRS[i][0]) == 0 && strcmp(b, PAIRS[i][1]) == 0) ||
            (strcmp(a, PAIRS[i][1]) == 0 && strcmp(b, PAIRS[i][0]) == 0)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// Rule 3: the substituted token
// ---------------------------------------------------------------------------------------

bool vocab_cloze_token(const vocab_cloze_t *cloze, const char *chunk, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!out || !cloze || !cloze->text || !cloze->right || !chunk) return false;
    const char *text = cloze->text;
    const size_t len = strlen(cloze->right);
    const size_t text_len = strlen(text);
    const size_t pos = cloze->pos;
    if (len == 0 || strlen(chunk) != len || pos + len > text_len) return false;
    for (size_t i = 0; i < len; i++) {
        if (!is_alpha(chunk[i]) || !is_alpha(cloze->right[i]) || text[pos + i] != cloze->right[i]) {
            return false;
        }
    }
    size_t start = pos;
    while (start > 0 && is_token_char(text[start - 1u])) start--;
    size_t end = pos + len;
    while (end < text_len && is_token_char(text[end])) end++;
    if (end - start >= cap) return false;
    for (size_t i = start; i < end; i++) {
        const char c = (i >= pos && i < pos + len) ? chunk[i - pos] : text[i];
        out[i - start] = to_lower(c);
    }
    out[end - start] = '\0';
    return true;
}

// True when `text` contains `token` (lowercase) as a whole token, case-insensitively.
static bool text_has_token(const char *text, const char *token)
{
    if (!text) return false;
    const size_t want = strlen(token);
    size_t i = 0;
    while (text[i] != '\0') {
        while (text[i] != '\0' && !is_token_char(text[i])) i++;
        const size_t start = i;
        while (text[i] != '\0' && is_token_char(text[i])) i++;
        if (i - start == want && want > 0) {
            size_t k = 0;
            while (k < want && to_lower(text[start + k]) == token[k]) k++;
            if (k == want) return true;
        }
    }
    return false;
}

static bool bank_has_token(const vocab_bank_t *bank, const char *token)
{
    if (!bank || !bank->entries) return false;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *e = &bank->entries[i];
        if (text_has_token(e->text, token) || text_has_token(e->example, token)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// Deny-list (spec 7.2 rule 3: "a small deny-list (vulgar words and known collisions)")
// ---------------------------------------------------------------------------------------

// The entries are ROT13-encoded so that the source of this children's product never spells
// the offensive words out; the lookup encodes the candidate instead. Decode one entry with
//   python3 -c "import codecs,sys; print(codecs.decode(sys.argv[1], 'rot13'))" ENTRY
// Sections:
// (1) every single-word form that ESDB/SCOWL (en-wl/wordlist v2, commit 1e5b7d3a, the
//     source of assets/wordlists/common-words-35.txt) marks offensive-1/-2 or vulgar-1/-3,
//     at any size: `python3 tools/vocab_wordlist.py scowl-pre.txt --usage-report`;
// (2) rude, sexual and slur words children may know that ESDB leaves unmarked (some are
//     also in the size-35 list; listing them keeps them denied without the word list);
// (3) known collisions: everyday words of young learners missing from the size-35 list.
static const char *const DENY_ROT13[] = {
    // (1) ESDB usage notes offensive-1, offensive-2, vulgar-1, vulgar-3
    "nob", "nobf", "nefrubyr", "nefrubyr'f", "nefrubyrf", "nffubyr", "nffubyr'f", "nffubyrf",
    "ohttre", "ohttre'f", "ohttrerq", "ohttrevat", "ohttref", "ohyyfung", "ohyyfuvg", "ohyyfuvgf",
    "ohyyfuvggrq", "ohyyfuvggre", "ohyyfuvggref", "ohyyfuvggvat", "puvpxrafuvg", "puvpxrafuvgf",
    "pbpxfhpxre", "pbpxfhpxre'f", "pbpxfhpxref", "pbba", "pbba'f", "pbbaf", "penc", "penccrq",
    "penccre", "penccref", "penccvat", "pencf", "phag", "phag'f", "phagf", "qvpx", "qvpx'f",
    "qvpxrq", "qvpxraf", "qvpxrafrf", "qvpxre", "qvpxrerq", "qvpxrevat", "qvpxref", "qvpxvat",
    "qvpxf", "sneg", "sneg'f", "snegrq", "snegvat", "snegf", "shpx", "shpx'f", "shpxrq", "shpxre",
    "shpxre'f", "shpxref", "shpxurnq", "shpxurnqf", "shpxvat", "shpxvatf", "shpxf", "ubefrfuvg",
    "ubefrfuvgf", "xenhg", "xenhgf", "zbgureshpxre", "zbgureshpxre'f", "zbgureshpxref",
    "zbgureshpxvat", "avttn", "avttnf", "avttnm", "avttre", "avttre'f", "avttreqbz", "avttrerq",
    "avttreurnq", "avttrevat", "avttrevfu", "avttrevfz", "avttrevfzf", "avttreyvat", "avttreyvatf",
    "avttref", "avttrel", "cvff", "cvffrq", "cvffre", "cvffref", "cvffrf", "cvffvat", "fung",
    "fuvg", "fuvg'f", "fuvgr", "fuvgrf", "fuvgsnprq", "fuvgurnq", "fuvgurnq'f", "fuvgurnqf",
    "fuvgybnq", "fuvgybnqf", "fuvgf", "fuvggrq", "fuvggvre", "fuvggvrfg", "fuvggvat", "fuvggl",
    "jbc", "jbc'f", "jbccrq", "jbccvat", "jbcf",
    // (2) rude, sexual and slur words unmarked in ESDB
    "nefr", "nefrf", "nff", "nffrf", "onfgneq", "onfgneqf", "ovgpu", "ovgpurf", "ovgpul",
    "obyybpx", "obyybpxf", "obbo", "obbovrf", "obbof", "obbol", "ohz", "ohzf", "ohgg", "ohggf",
    "puvax", "puvaxf", "pbpx", "pbpxf", "phz", "qnza", "qnzarq", "qnzavg", "qvyqb", "qvyqbf",
    "qlxr", "qlxrf", "snt", "snttbg", "snttbgf", "sntf", "tbqqnza", "tbbx", "tbbxf", "uryy",
    "ubzb", "ubzbf", "ubeal", "wnc", "wncf", "wvmm", "xvxr", "xvxrf", "ahqr", "ahqrf", "cnxv",
    "cnxvf", "crr", "crrq", "crrvat", "crrf", "cravf", "cravfrf", "cbb", "cbbrq", "cbbvat", "cbbc",
    "cbbcrq", "cbbcvat", "cbbcf", "cbbcl", "cbbf", "cbea", "cbeab", "cevpx", "cevpxf", "chffvrf",
    "chffl", "erqfxva", "erqfxvaf", "ergneq", "ergneqrq", "ergneqf", "frk", "frkvre", "frkvrfg",
    "frkl", "funt", "funttrq", "funttvat", "funtf", "fyhg", "fyhgf", "fyhggl", "fcnfgvp",
    "fcnfgvpf", "fcnm", "fcnmm", "fcvp", "fcvpx", "fcvpxf", "fcvpf", "gvg", "gvgf", "gvggvrf",
    "gvggl", "genaavrf", "genaal", "gheq", "gheqf", "gjng", "gjngf", "intvan", "intvanf", "jnax",
    "jnaxrq", "jnaxre", "jnaxref", "jnaxvat", "jnaxf", "jrr", "jrrf", "jrgonpx", "jrgonpxf",
    "juber", "juberf", "jvyyvr", "jvyyvrf", "jvyyl", "jbt", "jbtf",
    // (3) known collisions: everyday words of young learners missing from the size-35 list
    "nhagvr", "nhagvrf", "pubcfgvpx", "pubcfgvpxf", "qbttvr", "qbttl", "sevqtr", "sevqtrf",
    "tenaqzn", "tenaqznf", "tenaqcn", "tenaqcnf", "wrrc", "wrrcf", "ynqloveq", "ynqloveqf",
    "ybyyvrf", "ybyyl", "zrgeb", "zrgebf", "anccvrf", "anccl", "bbcf", "cvttl", "cbggl",
    "grqqvrf", "grqql", "gbsh", "lnl", "lblb", "lblbf", "lhpxl", "lhzzl",
};

static char rot13(char c)
{
    return is_lower(c) ? (char)('a' + (c - 'a' + 13) % 26) : c;
}

bool vocab_cloze_denied(const char *word)
{
    char enc[TOKEN_CAP];
    if (!word || word[0] == '\0') return false;
    size_t n = 0;
    while (word[n] != '\0') {
        if (n + 1u >= sizeof(enc)) return false;  // longer than any entry
        enc[n] = rot13(to_lower(word[n]));
        n++;
    }
    enc[n] = '\0';
    for (size_t i = 0; i < COUNT_OF(DENY_ROT13); i++) {
        if (strcmp(enc, DENY_ROT13[i]) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// Validator
// ---------------------------------------------------------------------------------------

vocab_cloze_verdict_t vocab_cloze_validate(const vocab_cloze_t *cloze, const vocab_bank_t *bank,
                                           vocab_word_lookup_fn is_common, void *ctx)
{
    if (!shape_ok(cloze)) return VOCAB_CLOZE_ERR_SHAPE;
    if (!vocab_cloze_pair_allowed(cloze->right, cloze->wrong)) return VOCAB_CLOZE_ERR_PAIR;
    char token[TOKEN_CAP];
    if (!vocab_cloze_token(cloze, cloze->wrong, token, sizeof(token))) return VOCAB_CLOZE_ERR_SHAPE;
    if (bank_has_token(bank, token)) return VOCAB_CLOZE_ERR_BANK_WORD;
    if (vocab_cloze_denied(token)) return VOCAB_CLOZE_ERR_DENIED;
    if (is_common && is_common(ctx, token)) return VOCAB_CLOZE_ERR_COMMON_WORD;
    return VOCAB_CLOZE_OK;
}

const char *vocab_cloze_verdict_name(vocab_cloze_verdict_t verdict)
{
    switch (verdict) {
    case VOCAB_CLOZE_OK: return "ok";
    case VOCAB_CLOZE_ERR_SHAPE: return "shape";
    case VOCAB_CLOZE_ERR_PAIR: return "pair";
    case VOCAB_CLOZE_ERR_BANK_WORD: return "bank-word";
    case VOCAB_CLOZE_ERR_COMMON_WORD: return "common-word";
    case VOCAB_CLOZE_ERR_DENIED: return "denied";
    default: return "unknown";
    }
}

// ---------------------------------------------------------------------------------------
// Spec 7.1 chunk priority
// ---------------------------------------------------------------------------------------

// rank: 1 vowel team / r-controlled, 2 consonant digraph, 3 vowel, 4 y, 5 s/z, 6 c/k (spec
// 7.1 lists class (3) as "a e i o u, y, s/z, c/k"). Partners are whitelisted (rule 2) and
// ordered by the most likely confusion for Chinese-speaking learners.
typedef struct {
    const char *chunk;
    uint8_t rank;
    const char *partners[PARTNERS_MAX];
} chunk_rule_t;

#define RANK_TEAM 1
#define RANK_DIGRAPH 2
#define RANK_LAST 6

static const chunk_rule_t CHUNK_RULES[] = {
    { "ee", 1, { "ea" } },
    { "ea", 1, { "ee" } },
    { "oo", 1, { "ou", "ue", "ui" } },
    { "ou", 1, { "ow", "oo" } },
    { "ai", 1, { "ay" } },
    { "ay", 1, { "ai" } },
    { "oa", 1, { "ow" } },
    { "ow", 1, { "oa", "ou" } },
    { "ue", 1, { "oo" } },
    { "ui", 1, { "oo" } },
    { "ar", 1, { "er", "or", "ir", "ur" } },
    { "er", 1, { "or", "ar", "ir", "ur" } },
    { "ir", 1, { "ur", "er", "ar", "or" } },
    { "or", 1, { "ar", "er", "ur", "ir" } },
    { "ur", 1, { "ir", "er", "ar", "or" } },
    { "sh", 2, { "ch" } },
    { "ch", 2, { "sh" } },
    { "th", 2, { NULL } },  // no whitelisted partner yet
    { "ph", 2, { "ff" } },
    { "ck", 2, { NULL } },  // no whitelisted partner yet
    { "wh", 2, { NULL } },  // no whitelisted partner yet
    { "a", 3, { "e", "u", "o", "i" } },
    { "e", 3, { "a", "i", "u", "o" } },
    { "i", 3, { "e", "y", "a", "u", "o" } },
    { "o", 3, { "a", "u", "e", "i" } },
    { "u", 3, { "a", "o", "e", "i" } },
    { "y", 4, { "i" } },
    { "s", 5, { "z", "c" } },
    { "z", 5, { "s" } },
    { "c", 6, { "k", "s" } },
    { "k", 6, { "c" } },
};

static const chunk_rule_t *find_rule(const char *chunk, size_t len)
{
    for (size_t i = 0; i < COUNT_OF(CHUNK_RULES); i++) {
        const char *c = CHUNK_RULES[i].chunk;
        if (strlen(c) == len && strncmp(c, chunk, len) == 0) return &CHUNK_RULES[i];
    }
    return NULL;
}

vocab_chunk_class_t vocab_cloze_chunk_class(const char *chunk)
{
    const size_t len = chunk_len(chunk);
    const chunk_rule_t *rule = len ? find_rule(chunk, len) : NULL;
    if (!rule) return VOCAB_CHUNK_NONE;
    if (rule->rank == RANK_TEAM) return VOCAB_CHUNK_VOWEL_TEAM;
    if (rule->rank == RANK_DIGRAPH) return VOCAB_CHUNK_DIGRAPH;
    return VOCAB_CHUNK_SINGLE;
}

// True when text[pos] belongs to a two-letter vowel team or digraph (then it is not a
// "single" letter for spec 7.1 class 3, e.g. the e of "sheep" or the c of "duck").
static bool inside_pair(const char *text, size_t pos, size_t text_len)
{
    const chunk_rule_t *before = pos > 0u ? find_rule(text + pos - 1u, 2u) : NULL;
    const chunk_rule_t *after = pos + 1u < text_len ? find_rule(text + pos, 2u) : NULL;
    return (before && before->rank <= RANK_DIGRAPH) || (after && after->rank <= RANK_DIGRAPH);
}

bool vocab_cloze_choose(const char *text, const vocab_bank_t *bank,
                        vocab_word_lookup_fn is_common, void *ctx, vocab_cloze_t *out)
{
    if (!text || !out) return false;
    const size_t text_len = strlen(text);
    if (text_len == 0u || text_len > UINT8_MAX) return false;
    for (uint8_t rank = 1; rank <= RANK_LAST; rank++) {
        for (size_t pos = 0; pos < text_len; pos++) {
            for (size_t r = 0; r < COUNT_OF(CHUNK_RULES); r++) {
                const chunk_rule_t *rule = &CHUNK_RULES[r];
                const size_t len = strlen(rule->chunk);
                if (rule->rank != rank || pos + len > text_len ||
                    memcmp(text + pos, rule->chunk, len) != 0) {
                    continue;
                }
                if (len == 1u && inside_pair(text, pos, text_len)) continue;
                for (size_t p = 0; p < PARTNERS_MAX && rule->partners[p]; p++) {
                    const vocab_cloze_t candidate = { text, (uint8_t)pos, rule->chunk,
                                                      rule->partners[p] };
                    if (vocab_cloze_validate(&candidate, bank, is_common, ctx) == VOCAB_CLOZE_OK) {
                        *out = candidate;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// Skeleton row geometry (spec 7.3)
// ---------------------------------------------------------------------------------------

static int32_t width_of(vocab_measure_fn measure, void *ctx, vocab_word_size_t size,
                        const char *text)
{
    if (!text || text[0] == '\0') return 0;
    const int32_t w = measure(ctx, size, text);
    return w > 0 ? w : 0;
}

vocab_cloze_row_t vocab_cloze_row_layout(const char *pre, const char *right, const char *other,
                                         const char *suf, vocab_word_size_t size,
                                         int32_t center_x, vocab_measure_fn measure, void *ctx)
{
    vocab_cloze_row_t row = { 0, 0, 0, 0, 0, 0, 0 };
    if (!measure) return row;
    const int32_t w_right = width_of(measure, ctx, size, right);
    const int32_t w_other = width_of(measure, ctx, size, other);
    row.pre_w = width_of(measure, ctx, size, pre);
    row.suf_w = width_of(measure, ctx, size, suf);
    row.box_w = (w_right > w_other ? w_right : w_other) + VOCAB_BLANK_PAD_PX;
    row.total_w = row.pre_w + row.box_w + row.suf_w;
    row.x = center_x - row.total_w / 2;
    row.box_x = row.x + row.pre_w;
    row.suf_x = row.box_x + row.box_w;
    return row;
}
