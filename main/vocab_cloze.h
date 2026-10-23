// main/vocab_cloze.h - Stage 3 cloze data helpers and the cloze validator (spec 7.1, 7.2).
//
// Runtime part (used by vocab_stage3.c): split an entry around its blank.
// Validation part (host tests, and the deferred word-bank pipeline): the confusion-pair
// whitelist and the "wrong spelling must not be a real word" rule, with the common-word list
// injected as a lookup callback (the firmware never links a word list).
//
// Pure; any task; never blocks; no allocation. Strings are ASCII.
// Owner: Stage 3 developer (also owns assets/wordlists/**). Contract written by the lead;
// only additive changes. Additions by the Stage 3 developer (Milestone 2, all additive):
// vocab_chunk_class_t / vocab_cloze_chunk_class(), vocab_cloze_choose() (spec 7.1 chunk
// priority), vocab_cloze_denied() (the built-in deny-list) and vocab_cloze_row_t /
// vocab_cloze_row_layout() (skeleton row geometry, spec 7.3); the comments of
// vocab_cloze_split() and vocab_cloze_validate() now state the exact checks.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vocab_layout.h"
#include "vocab_types.h"

// A cloze candidate: blank at byte `pos` of `text`, correct chunk `right`, lure `wrong`.
typedef struct {
    const char *text;
    uint8_t pos;
    const char *right;
    const char *wrong;
} vocab_cloze_t;

// Fills *out from a bank entry. Returns false (out untouched) when the entry has no cloze
// data (cloze_right or cloze_wrong NULL).
bool vocab_cloze_from_entry(const vocab_entry_t *entry, vocab_cloze_t *out);

// Copies text[0, pos) to `pre` and text[pos + strlen(right), end) to `suf`. Returns false
// when the cloze is inconsistent (spec 7.2 rule 1: `right` and `wrong` are 1..3 lowercase
// ASCII letters of equal length and differ, and text contains `right` at `pos`) or a buffer
// is too small; on false both buffers hold "" (when cap > 0). Spaces of phrases are kept
// ("ice cream", pos 6, "ea" -> "ice cr" + "m").
bool vocab_cloze_split(const vocab_cloze_t *cloze, char *pre, size_t pre_cap,
                       char *suf, size_t suf_cap);

// Spec 7.2 rule 2: true when {right, wrong} (case-insensitive, either order) is in the
// phonetic-confusion whitelist: any two of {a, e, i, o, u}; i/y; s/z; c/k; c/s; ph/ff;
// ee/ea; oo/ou; oa/ow; ou/ow; ai/ay; ue/oo; ui/oo; any two of {ar, er, ir, or, ur}; sh/ch.
bool vocab_cloze_pair_allowed(const char *right, const char *wrong);

// Writes the lowercased token (maximal run of letters/apostrophes around the blank; spaces
// and punctuation delimit) containing the blank with `chunk` substituted, e.g. "bag" + "i"
// -> "big", "ice cream" pos 6 + "ee" -> "creem". Returns false on inconsistent input or a
// too-small buffer.
bool vocab_cloze_token(const vocab_cloze_t *cloze, const char *chunk, char *out, size_t cap);

// Common-word lookup injected by the caller: true when the lowercase ASCII `word` is in the
// committed common-word list (assets/wordlists/, loaded by the host test).
typedef bool (*vocab_word_lookup_fn)(void *ctx, const char *word);

typedef enum {
    VOCAB_CLOZE_OK = 0,
    VOCAB_CLOZE_ERR_SHAPE,        // rule 1: missing fields, not 1..3 lowercase letters,
                                  // unequal lengths, equal chunks, or text mismatch at pos
    VOCAB_CLOZE_ERR_PAIR,         // rule 2: not a whitelisted confusion pair
    VOCAB_CLOZE_ERR_BANK_WORD,    // rule 3: the substituted token is a token of a bank entry
    VOCAB_CLOZE_ERR_COMMON_WORD,  // rule 3: the substituted token is a common English word
    VOCAB_CLOZE_ERR_DENIED,       // rule 3: the substituted token is in the deny-list
} vocab_cloze_verdict_t;

// Spec 7.2 rules 1-3 for one candidate. `bank` may be NULL (skip the bank-token check);
// `is_common` may be NULL (skip the common-word check). Must reject the negative cases of
// rule 4 (bag a->i, cat a->u, pear ea->ai, pear ea->ee, bread ea->ee, read ea->ai,
// blue ue->ew) given the committed word list.
// Checks in order (the first failure is returned): rule 1 shape, rule 2 pair; then the
// lowercased substituted token (vocab_cloze_token with `wrong`) against the tokens of every
// bank entry's `text` and `example` (case-insensitive), the built-in deny-list
// (vocab_cloze_denied, always active) and finally `is_common`.
vocab_cloze_verdict_t vocab_cloze_validate(const vocab_cloze_t *cloze, const vocab_bank_t *bank,
                                           vocab_word_lookup_fn is_common, void *ctx);

// Stable short name of a verdict for test messages ("ok", "shape", ...).
const char *vocab_cloze_verdict_name(vocab_cloze_verdict_t verdict);

// ---------------------------------------------------------------------------------------
// Additions by the Stage 3 developer (additive, Milestone 2)
// ---------------------------------------------------------------------------------------

// Spec 7.1 chunk priority classes (lower value = higher priority).
typedef enum {
    VOCAB_CHUNK_NONE = 0,         // not an eligible blank chunk (no cloze data, Stage 2 fallback)
    VOCAB_CHUNK_VOWEL_TEAM = 1,   // vowel team or r-controlled vowel: ee ea oo ou ai ay oa ow
                                  // ue ui ar er ir or ur
    VOCAB_CHUNK_DIGRAPH = 2,      // consonant digraph: sh ch th ph ck wh
    VOCAB_CHUNK_SINGLE = 3,       // single vowel letter or sound-alike letter: a e i o u, y,
                                  // s z, c k (in this order of preference)
} vocab_chunk_class_t;

// Class of a lowercase chunk string (exact match; NULL, "" or anything else -> NONE).
vocab_chunk_class_t vocab_cloze_chunk_class(const char *chunk);

// Spec 7.1 for the deferred word-bank pipeline: the first chunk of `text` in priority order
// (class 1, 2, then 3 with vowels before y, s/z and c/k; left to right within a rank; a
// single letter inside a two-letter vowel team or digraph is skipped) together with the
// first of its whitelisted partners (fixed, sound-based order, e.g. a: e u o i) for which
// vocab_cloze_validate() returns OK. Returns false when no candidate passes (the entry gets
// no cloze data and falls back to Stage 2). out->right / out->wrong point to static strings,
// out->text to `text`. Curated bank entries may override the choice with a recorded reason.
bool vocab_cloze_choose(const char *text, const vocab_bank_t *bank,
                        vocab_word_lookup_fn is_common, void *ctx, vocab_cloze_t *out);

// True when the lowercased `word` is in the small built-in deny-list (spec 7.2 rule 3):
// every single-word form that ESDB/SCOWL marks offensive or vulgar, rude words children
// use, and everyday words children know that the size-35 common-word list lacks (known
// collisions). Exact token match, case-insensitive; NULL or "" -> false.
bool vocab_cloze_denied(const char *word);

// Stage 3 skeleton row geometry (spec 7.3, 11.4; AC-25, AC-26): prefix label, blank box and
// suffix label centered as one group on `center_x`, everything measured with `measure` in
// the same font `size`, which the renderer chooses from the full correct text
// (pre + right + suf) with the word-size rule (spec 11.3). The box is
// max(width(right), width(other)) + VOCAB_BLANK_PAD_PX wide. The result depends only on the
// four strings - never on which chunk is correct or whether the blank is filled - so the box
// never leaks the answer and success never re-flows the row. NULL strings measure 0;
// negative measurements are clamped to 0; a NULL `measure` yields an all-zero row.
typedef struct {
    int32_t x;        // left edge of the group (prefix label)
    int32_t pre_w;    // prefix width
    int32_t box_x;    // blank box left edge (= x + pre_w)
    int32_t box_w;    // blank box width
    int32_t suf_x;    // suffix left edge (= box_x + box_w)
    int32_t suf_w;    // suffix width
    int32_t total_w;  // pre_w + box_w + suf_w
} vocab_cloze_row_t;

vocab_cloze_row_t vocab_cloze_row_layout(const char *pre, const char *right, const char *other,
                                         const char *suf, vocab_word_size_t size,
                                         int32_t center_x, vocab_measure_fn measure, void *ctx);
