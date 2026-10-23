// main/vocab_progress.h - persistent-state codec: meta blob, progress chunks, migration and
// the NVS budget model (spec 10.1 - 10.3, 10.6).
//
// Explicit little-endian serialization (no struct memcpy), CRC-32/IEEE. Pure; any task;
// never blocks; no allocation. NVS access and the save policy live in vocab_store.
// Owner: lead developer (tests/test_vocab_progress.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vocab_types.h"

// Decoded `meta` blob (40 bytes on flash: u16 magic, u8 version, u8 volume index, u16 tick,
// u16 chunk count, u32 XP, u32 bank fingerprint, u32 objective answers, u32 first-try
// correct, 12 reserved bytes, u32 CRC over bytes 0..35).
typedef struct {
    uint8_t version;            // VOCAB_FORMAT_VERSION when written by this firmware
    uint8_t volume_index;       // 0..VOCAB_VOL_LEVEL_COUNT-1
    vocab_tick_t tick;          // completed sessions
    uint16_t chunk_count;       // progress chunks p00..pNN
    uint32_t xp;                // cumulative XP
    uint32_t bank_fingerprint;  // vocab_bank_fingerprint() of the bank that wrote it
    uint32_t objective_answers; // lifetime statistics
    uint32_t first_try_correct;
} vocab_meta_t;

// Decoded chunk header (chunk = 8-byte header + 6 bytes per record + u32 CRC).
typedef struct {
    uint8_t version;
    uint8_t count;              // records in the chunk (<= VOCAB_NVS_RECORDS_PER_CHUNK)
    vocab_tick_t tick;          // session tick when the chunk was written
    uint16_t index;             // chunk number i (records [64 i, 64 i + 64) of the uid order)
} vocab_chunk_header_t;

#define VOCAB_CHUNK_MAX_BYTES \
    (VOCAB_CHUNK_HEADER_BYTES + VOCAB_NVS_RECORDS_PER_CHUNK * VOCAB_CHUNK_RECORD_BYTES + \
     VOCAB_CHUNK_CRC_BYTES)

// CRC-32/IEEE (reflected, init 0xFFFFFFFF, final xor 0xFFFFFFFF).
uint32_t vocab_crc32(const uint8_t *data, size_t len);

// FNV-1a 32 over the bank's uids sorted ascending, each as 2 little-endian bytes.
uint32_t vocab_bank_fingerprint(const vocab_bank_t *bank);

// Defaults for a fresh device: version current, volume default, everything else 0.
void vocab_meta_defaults(vocab_meta_t *meta);

// Encodes `meta` into exactly VOCAB_META_BYTES bytes; returns the size or 0 if cap is short.
size_t vocab_meta_encode(const vocab_meta_t *meta, uint8_t *buf, size_t cap);

// Decodes and validates length, magic and CRC. A newer version decodes successfully with
// meta->version > VOCAB_FORMAT_VERSION (the caller must then not overwrite anything).
bool vocab_meta_decode(const uint8_t *buf, size_t len, vocab_meta_t *meta);

// Encodes `count` (<= 64) records, which must already be in uid order, as chunk `index`
// written at `tick`. State byte: bits 0-2 mastery, bit 3 seen, bit 4 ever_mastered.
// Returns the size (8 + 6 count + 4) or 0 when count or cap is invalid.
size_t vocab_chunk_encode(const vocab_record_t *records, uint8_t count, vocab_tick_t tick,
                          uint16_t index, uint8_t *buf, size_t cap);

// Decodes a chunk: validates magic, version (must equal VOCAB_FORMAT_VERSION), the length
// implied by the count, and the CRC. Writes up to `cap` records (uid, due, mastery 0..7 as
// stored, flags, lapses). Returns false for any corruption.
bool vocab_chunk_decode(const uint8_t *buf, size_t len, vocab_chunk_header_t *header,
                        vocab_record_t *records, size_t cap);

// Spec 10.3 rule 2: applies `n` loaded records to the bank-parallel `records`. Matching is by
// uid; unknown uids are dropped; a bank entry already marked in `assigned` keeps its first
// record (duplicates); mastery > 5 is clamped to 5; a due later than tick + 2 x INTERVAL[5] is
// reset to tick + INTERVAL[m]; flags are masked to SEEN | EVER_MASTERED. `assigned` has
// bank->count entries. Returns the number of records applied.
size_t vocab_progress_merge(const vocab_bank_t *bank, vocab_record_t *records, bool *assigned,
                            const vocab_record_t *loaded, size_t n, vocab_tick_t tick);

// NVS budget model (spec 10.6): a blob of b bytes costs 2 + ceil(b / 32) entries (index +
// data header + data), plus one namespace entry; usable entries = (pages - 1) x 126.
typedef struct {
    uint32_t chunks;            // progress chunks for `words`
    uint32_t entries;           // entries used by meta + chunks + namespace
    uint32_t usable;            // usable entries of the partition
    uint32_t percent;           // entries * 100 / usable (rounded)
    bool too_tight;             // > 50 %: rewriting every chunk at a save needs headroom
} vocab_nvs_budget_t;

vocab_nvs_budget_t vocab_nvs_budget(uint32_t words, uint32_t partition_bytes);
