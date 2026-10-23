// main/vocab_adpcm.h - IMA-ADPCM decoder for the embedded word clips (spec 8.5).
//
// Clip layout (one clip in the blob, located through vocab_clips.h):
//   byte 0-1  int16 initial predictor (little-endian)
//   byte 2    uint8 initial step index (0..88)
//   byte 3    0 (reserved)
//   byte 4..  4-bit IMA-ADPCM codes, low nibble first; nibble k decodes PCM sample k.
// The header predictor is the decoder's initial state only (it is not emitted as a sample);
// a clip has exactly vocab_clip_t.n_samples codes. Standard IMA step and index tables. The
// decoder clamps the predictor to int16 and the index to 0..88 and must match the Python
// reference decoder in tools/vocab_audio/build_clips.py bit-exactly (AC-35).
//
// Pure integer code (no libm), no allocation, no blocking; any task. The audio worker
// decodes VOCAB_AUDIO_CHUNK_SAMPLES samples per call into a static buffer.
// Owner: assets developer. Contract written by the lead; only additive changes.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VOCAB_ADPCM_HEADER_BYTES 4u
#define VOCAB_ADPCM_INDEX_MAX    88u

typedef struct {
    int16_t predictor;   // last decoded sample
    uint8_t step_index;  // 0..VOCAB_ADPCM_INDEX_MAX
} vocab_adpcm_state_t;

// Initializes `st` from a clip's 4-byte header. Returns false (st set to predictor 0,
// index 0) when `clip` is NULL, `clip_bytes` < VOCAB_ADPCM_HEADER_BYTES or the header's
// step index exceeds 88.
bool vocab_adpcm_init(vocab_adpcm_state_t *st, const uint8_t *clip, size_t clip_bytes);

// Decodes one 4-bit code (0..15) and returns the new sample (also stored in st->predictor).
int16_t vocab_adpcm_step(vocab_adpcm_state_t *st, uint8_t code);

// Decodes `n_samples` codes starting at code index `first_code` of `codes` (the bytes after
// the header; code k is the low nibble of byte k/2 when k is even, else the high nibble)
// into `out`. The caller guarantees the codes exist. Returns n_samples.
size_t vocab_adpcm_decode(vocab_adpcm_state_t *st, const uint8_t *codes, size_t first_code,
                          size_t n_samples, int16_t *out);
