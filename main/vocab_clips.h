// main/vocab_clips.h - word clip pack: self-describing blob and lookup (spec 8.5, amendment A6).
//
// All word clips live in ONE self-describing blob (the index is inside the blob), written by
// tools/vocab_audio/build_clips.py and embedded by main/CMakeLists.txt under the symbols
// _binary_vocab_clips_bin_start/_end, which only the audio worker uses: it installs the
// blob with vocab_clips_install() at boot. The Windows-TTS test clips are private and
// git-ignored (amendment A6): the repository and CI embed the committed empty pack
// (assets/audio/vocab_clips_empty.bin), so every lookup returns NULL and the session uses the
// WORD_PULSE visual substitute; a local build embeds the local pack
// (assets/audio/vocab_test_clips.bin) when it exists and VOCAB_NO_CLIPS is not set.
//
// Blob format, version 1 (little-endian; offsets from the blob start):
//   0   u8[4] magic "VCLP"
//   4   u16   format version (1)
//   6   u16   index entry size (16)
//   8   u32   clip count n (0..VOCAB_CLIPS_MAX)
//   12  u32   blob size in bytes (a multiple of 4)
//   16  u32   CRC-32 (IEEE, as zlib) of bytes 0..15 followed by the n index entries
//   20  u32   reserved (0)
//   24  n index entries of 16 bytes, uid strictly ascending (uid != 0):
//         u16 uid, u16 reserved (0), u32 offset, u32 n_samples, u32 n_bytes
//   then the clips in index order at 4-byte aligned offsets, without overlap, zero padding.
//   Each clip is a 4-byte ADPCM header + codes (vocab_adpcm.h): n_bytes = 4 + ceil(n/2).
// The empty pack is the 24-byte header with n = 0. A blob that breaks any rule (foreign
// data, truncation, CRC, order, alignment, overlap, bounds, sizes, a clip header with a
// step index above 88) is rejected as a whole and behaves like the empty pack.
//
// Pure: no allocation, no blocking, no I/O. The installed pack is written once at boot,
// before any lookup, and is read-only afterwards, so lookups are safe from any task.
// Owner: assets developer. Contract written by the lead; Milestone-1 names kept where
// possible (vocab_clip_t, vocab_clip_find); the generated-data symbols vocab_clip_index[],
// vocab_clip_count and vocab_clip_blob_bytes were replaced by vocab_clip_at(),
// vocab_clips_count() and vocab_clips_blob_bytes() (amendment A6).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vocab_config.h"
#include "vocab_types.h"

#define VOCAB_CLIPS_MAGIC         "VCLP"
#define VOCAB_CLIPS_VERSION       1u
#define VOCAB_CLIPS_HEADER_BYTES  24u
#define VOCAB_CLIPS_ENTRY_BYTES   16u
#define VOCAB_CLIPS_MAX           VOCAB_RECORDS_MAX  // at most one clip per bank word

typedef struct {
    vocab_uid_t uid;
    uint32_t offset;     // byte offset of the clip (its 4-byte ADPCM header) in the blob
    uint32_t n_bytes;    // header + codes, in bytes
    uint32_t n_samples;  // decoded PCM samples at 16 kHz (= number of ADPCM codes)
} vocab_clip_t;

typedef enum {
    VOCAB_CLIPS_OK = 0,
    VOCAB_CLIPS_ERR_ARG,      // NULL data
    VOCAB_CLIPS_ERR_SIZE,     // shorter than the header, size field mismatch, not 4-aligned
    VOCAB_CLIPS_ERR_MAGIC,    // not a clip pack
    VOCAB_CLIPS_ERR_VERSION,  // unknown format version or entry size
    VOCAB_CLIPS_ERR_COUNT,    // more than VOCAB_CLIPS_MAX clips or an index beyond the blob
    VOCAB_CLIPS_ERR_CRC,      // header/index checksum
    VOCAB_CLIPS_ERR_INDEX,    // order, uid 0, reserved bits, alignment, overlap, bounds, sizes
    VOCAB_CLIPS_ERR_CLIP,     // clip header (step index above 88 or reserved byte set)
} vocab_clips_status_t;

// A validated pack over caller-owned memory (flash, a file read by a test, ...).
typedef struct {
    const uint8_t *data;  // NULL unless the last open succeeded
    uint32_t size;
    uint32_t count;       // 0 for the empty pack and for any rejected blob
} vocab_clip_pack_t;

// Validates `data` completely; on failure the pack is empty (count 0, data NULL).
vocab_clips_status_t vocab_clip_pack_open(vocab_clip_pack_t *pack, const uint8_t *data,
                                          size_t size);
// Entry `index` (uid order). False when out of range.
bool vocab_clip_pack_entry(const vocab_clip_pack_t *pack, size_t index, vocab_clip_t *out);
// Binary search by uid. False when absent.
bool vocab_clip_pack_find(const vocab_clip_pack_t *pack, vocab_uid_t uid, vocab_clip_t *out);
// The clip's bytes (ADPCM header + codes) inside the pack, or NULL.
const uint8_t *vocab_clip_pack_bytes(const vocab_clip_pack_t *pack, const vocab_clip_t *clip);
// Short English name of a status for logs ("ok", "bad magic", ...).
const char *vocab_clips_status_name(vocab_clips_status_t status);

// The installed pack (device: vocab_audio_start() installs the embedded blob; host tests
// may install any blob; nothing installed = no clips). Copies the index into a static table
// (VOCAB_CLIPS_MAX entries) so vocab_clip_find() can return stable pointers. A rejected or
// NULL blob installs the empty pack. Call once at boot, before any lookup.
vocab_clips_status_t vocab_clips_install(const uint8_t *data, size_t size);
size_t vocab_clips_count(void);          // clips in the installed pack
uint32_t vocab_clips_blob_bytes(void);   // size of the installed blob (0 = none)
const vocab_clip_t *vocab_clip_at(size_t index);  // uid order; NULL when out of range

// Binary search by uid in the installed pack. Returns NULL when the uid has no clip (the
// app then uses the WORD_PULSE visual substitute).
const vocab_clip_t *vocab_clip_find(vocab_uid_t uid);

// Bytes of an installed clip (ADPCM header + codes, n_bytes long), or NULL.
const uint8_t *vocab_clip_bytes(const vocab_clip_t *clip);
