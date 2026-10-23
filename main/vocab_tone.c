// main/vocab_tone.c - runtime cue synthesizer (spec 8.1, 8.2); see vocab_tone.h.
//
// Integer synthesis, no libm: a 32-bit phase accumulator drives a 256-entry Q15 sine table
// with linear interpolation (error below -90 dB); the instantaneous frequency of a sweep
// moves linearly from f0 to f1. Every segment restarts at phase 0 and has a linear 5 ms
// attack and 5 ms release, so it begins and ends with a 0 sample (no clicks between
// segments). Every sample is a pure function of the stream position and the phase
// accumulator, so chunked rendering equals one-shot rendering.
//
// Per-cue peak amplitudes stay at or below VOCAB_TONE_PEAK (-6 dBFS). Frequent, bright
// cues are quieter so they do not dominate the word clips (speech RMS -18 dBFS):
// CORRECT, CHEER, LEVEL_UP -9 dBFS; FLIP, TICK -12 dBFS; WRONG -6 dBFS (low notes that a
// small speaker reproduces weakly; soft, never a buzzer). Tune on the device (AC-38).
#include "vocab_tone.h"

#include <string.h>

#define SAMPLES_PER_MS  (VOCAB_AUDIO_RATE_HZ / 1000u)
#define ATTACK_SAMPLES  (VOCAB_TONE_ATTACK_MS * SAMPLES_PER_MS)
#define RELEASE_SAMPLES (VOCAB_TONE_RELEASE_MS * SAMPLES_PER_MS)
#define AMP_M6_DB       VOCAB_TONE_PEAK  // 16384
#define AMP_M9_DB       11627
#define AMP_M12_DB      8231

static const vocab_tone_seg_t CUE_CORRECT[] = { { 1319, 1319, 70 }, { 1760, 1760, 110 } };
static const vocab_tone_seg_t CUE_WRONG[] = { { 392, 392, 90 }, { 330, 330, 150 } };
static const vocab_tone_seg_t CUE_FLIP[] = { { 600, 1200, 60 } };
static const vocab_tone_seg_t CUE_TICK[] = { { 1000, 1000, 40 } };
static const vocab_tone_seg_t CUE_CHEER[] = {
    { 1047, 1047, 80 }, { 1319, 1319, 80 }, { 1568, 1568, 80 }, { 2093, 2093, 160 },
};
static const vocab_tone_seg_t CUE_LEVEL_UP[] = {
    { 1047, 1047, 80 }, { 1319, 1319, 80 }, { 1568, 1568, 80 }, { 2093, 2093, 80 },
    { 1568, 1568, 80 }, { 2093, 2093, 300 },
};

// round(32767 * sin(2 * pi * i / 256)), i = 0..256 (the last entry repeats the first).
static const int16_t SINE_Q15[257] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739,
    9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811,
    25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
    32609, 32678, 32728, 32757, 32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
    32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571, 30273, 29956, 29621, 29268,
    28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151,
    15446, 14732, 14010, 13279, 12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179,
    6393, 5602, 4808, 4011, 3212, 2410, 1608, 804, 0, -804, -1608, -2410,
    -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159,
    -20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113,
    -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580,
    -31356, -31113, -30852, -30571, -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403,
    -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011,
    -3212, -2410, -1608, -804, 0,
};

#define SEGS(a) (sizeof(a) / sizeof((a)[0]))

size_t vocab_tone_segments(vocab_cue_t cue, const vocab_tone_seg_t **out)
{
    const vocab_tone_seg_t *table = NULL;
    size_t n = 0;
    switch (cue) {
    case VOCAB_CUE_CORRECT: table = CUE_CORRECT; n = SEGS(CUE_CORRECT); break;
    case VOCAB_CUE_WRONG: table = CUE_WRONG; n = SEGS(CUE_WRONG); break;
    case VOCAB_CUE_FLIP: table = CUE_FLIP; n = SEGS(CUE_FLIP); break;
    case VOCAB_CUE_TICK: table = CUE_TICK; n = SEGS(CUE_TICK); break;
    case VOCAB_CUE_CHEER: table = CUE_CHEER; n = SEGS(CUE_CHEER); break;
    case VOCAB_CUE_LEVEL_UP: table = CUE_LEVEL_UP; n = SEGS(CUE_LEVEL_UP); break;
    case VOCAB_CUE_NONE:
    case VOCAB_CUE_COUNT:
    default:
        break;
    }
    if (out) *out = table;
    return n;
}

uint32_t vocab_tone_total_samples(vocab_cue_t cue)
{
    const vocab_tone_seg_t *segs = NULL;
    const size_t n = vocab_tone_segments(cue, &segs);
    uint32_t total = 0;
    for (size_t i = 0; i < n; i++) total += (uint32_t)segs[i].ms * SAMPLES_PER_MS;
    return total;
}

static int32_t cue_amplitude(vocab_cue_t cue)
{
    switch (cue) {
    case VOCAB_CUE_WRONG: return AMP_M6_DB;
    case VOCAB_CUE_FLIP:
    case VOCAB_CUE_TICK: return AMP_M12_DB;
    case VOCAB_CUE_CORRECT:
    case VOCAB_CUE_CHEER:
    case VOCAB_CUE_LEVEL_UP:
    case VOCAB_CUE_NONE:
    case VOCAB_CUE_COUNT:
    default: return AMP_M9_DB;
    }
}

// Phase increment per sample for `hz` at the output rate (2^32 = one cycle).
static uint32_t phase_inc(uint32_t hz)
{
    return (uint32_t)(((uint64_t)hz << 32) / VOCAB_AUDIO_RATE_HZ);
}

static int32_t sine_q15(uint32_t phase)
{
    const uint32_t i = phase >> 24;
    const int32_t frac = (int32_t)((phase >> 8) & 0xFFFFu);
    const int32_t a = SINE_Q15[i];
    const int32_t b = SINE_Q15[i + 1u];
    return a + ((b - a) * frac) / 65536;
}

bool vocab_tone_start(vocab_tone_t *tone, vocab_cue_t cue)
{
    memset(tone, 0, sizeof(*tone));
    tone->cue = cue;
    tone->total = vocab_tone_total_samples(cue);
    return tone->total > 0;
}

size_t vocab_tone_render(vocab_tone_t *tone, int16_t *out, size_t max_samples)
{
    const vocab_tone_seg_t *segs = NULL;
    const size_t n_segs = vocab_tone_segments(tone->cue, &segs);
    if (n_segs == 0 || tone->pos >= tone->total) return 0;
    const int32_t amp = cue_amplitude(tone->cue);

    size_t written = 0;
    while (written < max_samples && tone->pos < tone->total) {
        // Locate the segment of the current position.
        uint32_t start = 0;
        size_t s = 0;
        for (; s + 1u < n_segs; s++) {
            const uint32_t len = (uint32_t)segs[s].ms * SAMPLES_PER_MS;
            if (tone->pos < start + len) break;
            start += len;
        }
        const uint32_t len = (uint32_t)segs[s].ms * SAMPLES_PER_MS;
        const uint32_t local = tone->pos - start;
        if (local == 0) tone->phase = 0;

        const int32_t level = (amp * sine_q15(tone->phase)) / 32768;
        int32_t env = 32768;
        if (local < ATTACK_SAMPLES) env = (int32_t)((local * 32768u) / ATTACK_SAMPLES);
        const uint32_t to_end = len - 1u - local;
        if (to_end < RELEASE_SAMPLES) {
            const int32_t rel = (int32_t)((to_end * 32768u) / RELEASE_SAMPLES);
            if (rel < env) env = rel;
        }
        out[written++] = (int16_t)((level * env) / 32768);

        uint32_t inc = phase_inc(segs[s].f0_hz);
        if (segs[s].f1_hz != segs[s].f0_hz) {
            const int64_t span = (int64_t)phase_inc(segs[s].f1_hz) - (int64_t)inc;
            inc = (uint32_t)((int64_t)inc + (span * (int64_t)local) / (int64_t)len);
        }
        tone->phase += inc;
        tone->pos++;
    }
    return written;
}