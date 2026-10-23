// main/vocab_bank.h - embedded word bank access (spec 12).
//
// The test bank is one const table in flash (vocab_bank.c) in curriculum order; its order
// defines "new word" order (spec 4.4). The table layout (vocab_entry_t in vocab_types.h) is
// the data contract for the deferred word-bank pipeline: a future generator emits the same
// table. Audio is looked up by uid (vocab_clips.h), never stored in the entry.
//
// IPA convention: British broad transcription (OALD style as in Chinese textbooks): no
// "(r)", no syllable dots, length marks kept, U+0261 for g, between slashes. Code points
// used: ASCII plus U+02C8 U+02CC U+02D0 U+00E6 U+0259 U+0251 U+0252 U+0254 U+028A U+028C
// U+026A U+014B U+03B8 U+0283 U+0292 U+0261. Sentence-like phrases have no IPA (NULL).
// The word clips stay en-US (accepted by the user).
//
// All functions are pure, never allocate and may be called from any task.
// Owner: lead developer.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vocab_types.h"

// The compiled-in test bank (48 entries: 40 words, 8 phrases; 34 with cloze data).
const vocab_bank_t *vocab_bank_builtin(void);

// Entry at `index`, or NULL when bank is NULL or index >= bank->count.
const vocab_entry_t *vocab_bank_at(const vocab_bank_t *bank, vocab_index_t index);

// Linear search by uid. Returns true and writes the index when found.
bool vocab_bank_find(const vocab_bank_t *bank, vocab_uid_t uid, vocab_index_t *out_index);

// WORD or PHRASE: an entry whose text contains a space is a PHRASE (spec 4.7).
vocab_kind_t vocab_entry_kind(const vocab_entry_t *entry);

// True when cloze_right and cloze_wrong are both present (Stage 3 eligible, spec 7.1).
bool vocab_entry_has_cloze(const vocab_entry_t *entry);

// True when the entry has a non-empty IPA string (otherwise the IPA line is hidden).
bool vocab_entry_has_ipa(const vocab_entry_t *entry);

// True when the entry has a cloze sentence (Stage 2 cloze front candidate, spec 4.6).
bool vocab_entry_has_example_cloze(const vocab_entry_t *entry);
