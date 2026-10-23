// main/vocab_tone.h - runtime cue synthesizer (spec 8.1, 8.2).
//
// Cues are sine segments rendered at 16 kHz mono, each segment with a 5 ms attack and a
// 5 ms release, peak at or below -6 dBFS (|sample| <= 16384). A segment with f0 != f1 is a
// linear frequency sweep (CUE_FLIP). No cue files are stored. Per-cue peaks (vocab_tone.c):
// WRONG -6 dBFS, CORRECT / CHEER / LEVEL_UP -9 dBFS, FLIP / TICK -12 dBFS.
//
// Cue table (spec 8.2): CORRECT 1319 Hz 70 ms + 1760 Hz 110 ms; WRONG 392 Hz 90 ms +
// 330 Hz 150 ms; FLIP 600 -> 1200 Hz 60 ms; TICK 1000 Hz 40 ms; CHEER 1047, 1319, 1568 Hz
// x 80 ms + 2093 Hz 160 ms; LEVEL_UP 1047, 1319, 1568, 2093, 1568 Hz x 80 ms + 2093 Hz
// 300 ms.
//
// Pure integer/table code: no libm (host tests link without -lm), no allocation, no
// blocking; any task. The audio worker streams a cue in VOCAB_AUDIO_CHUNK_SAMPLES chunks.
// Owner: assets developer. Contract written by the lead; only additive changes.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vocab_types.h"

#define VOCAB_TONE_ATTACK_MS   5u
#define VOCAB_TONE_RELEASE_MS  5u
#define VOCAB_TONE_PEAK        16384   // -6 dBFS

typedef struct {
    uint16_t f0_hz;   // start frequency
    uint16_t f1_hz;   // end frequency (== f0_hz for a steady tone)
    uint16_t ms;      // duration
} vocab_tone_seg_t;

// Segment table of `cue`. Writes the array pointer (static const storage) to *out and
// returns the segment count; returns 0 and writes NULL for VOCAB_CUE_NONE / unknown cues.
size_t vocab_tone_segments(vocab_cue_t cue, const vocab_tone_seg_t **out);

// Total PCM samples of `cue` at VOCAB_AUDIO_RATE_HZ (0 for NONE / unknown).
uint32_t vocab_tone_total_samples(vocab_cue_t cue);

// Streaming generator state (fields private to the owner; plain data, copyable).
typedef struct {
    vocab_cue_t cue;
    uint32_t pos;         // samples rendered so far
    uint32_t total;       // vocab_tone_total_samples(cue)
    uint32_t phase;       // oscillator phase accumulator
} vocab_tone_t;

// Starts rendering `cue` from its first sample. Returns false for NONE / unknown cues (the
// generator is then finished: render returns 0).
bool vocab_tone_start(vocab_tone_t *tone, vocab_cue_t cue);

// Renders up to `max_samples` samples into `out`; returns the number written (0 when the
// cue has finished). Rendering in chunks of any size yields the same samples as one call.
size_t vocab_tone_render(vocab_tone_t *tone, int16_t *out, size_t max_samples);
