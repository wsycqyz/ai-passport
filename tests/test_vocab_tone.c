// tests/test_vocab_tone.c - runtime cue synthesizer (spec 8.1, 8.2).
//
// Checks every cue of spec 8.2 against main/vocab_tone.c: the segment table (frequencies,
// sweep, durations), total length, peak at or below -6 dBFS (|sample| <= 16384) and the
// per-cue level, 5 ms linear attack/release with exact zero samples at every segment edge,
// the frequency of every steady segment (zero crossings) and the rising FLIP sweep, the
// sustain RMS of a sine, chunked rendering == one-shot rendering for any chunk size,
// copyable state, determinism, and NONE / unknown cues. No libm (the compile line has no
// -lm). Compiled by tools/validate.sh with:
//   tests/test_vocab_tone.c main/vocab_tone.c
//   main/vocab_bank.c main/vocab_rng.c main/vocab_view.c
#include <string.h>

#include "vocab_config.h"
#include "vocab_test.h"
#include "vocab_tone.h"

#define SPMS        (VOCAB_AUDIO_RATE_HZ / 1000u)  // samples per millisecond
#define EDGE        (VOCAB_TONE_ATTACK_MS * SPMS)  // 80 samples of attack (and of release)
#define MAX_SAMPLES 12000u

typedef struct {
    vocab_cue_t cue;
    const char *name;
    size_t n;
    vocab_tone_seg_t segs[6];
    int32_t amp;  // expected peak amplitude of the cue
} expect_t;

static const expect_t EXPECT[] = {
    { VOCAB_CUE_CORRECT, "CORRECT", 2, { { 1319, 1319, 70 }, { 1760, 1760, 110 } }, 11627 },
    { VOCAB_CUE_WRONG, "WRONG", 2, { { 392, 392, 90 }, { 330, 330, 150 } }, 16384 },
    { VOCAB_CUE_FLIP, "FLIP", 1, { { 600, 1200, 60 } }, 8231 },
    { VOCAB_CUE_TICK, "TICK", 1, { { 1000, 1000, 40 } }, 8231 },
    { VOCAB_CUE_CHEER, "CHEER", 4,
      { { 1047, 1047, 80 }, { 1319, 1319, 80 }, { 1568, 1568, 80 }, { 2093, 2093, 160 } }, 11627 },
    { VOCAB_CUE_LEVEL_UP, "LEVEL_UP", 6,
      { { 1047, 1047, 80 }, { 1319, 1319, 80 }, { 1568, 1568, 80 }, { 2093, 2093, 80 },
        { 1568, 1568, 80 }, { 2093, 2093, 300 } }, 11627 },
};
#define N_EXPECT (sizeof(EXPECT) / sizeof(EXPECT[0]))

static int16_t s_one[MAX_SAMPLES];
static int16_t s_chunked[MAX_SAMPLES];

static int32_t iabs32(int32_t v)
{
    return v < 0 ? -v : v;
}

// Sign changes between strictly positive and strictly negative samples in [from, to).
static uint32_t zero_crossings(const int16_t *x, uint32_t from, uint32_t to)
{
    uint32_t count = 0;
    int last = 0;
    for (uint32_t i = from; i < to; i++) {
        const int sign = x[i] > 0 ? 1 : (x[i] < 0 ? -1 : 0);
        if (sign != 0) {
            if (last != 0 && sign != last) count++;
            last = sign;
        }
    }
    return count;
}

static size_t render_all(vocab_cue_t cue, int16_t *out, size_t chunk)
{
    vocab_tone_t tone;
    if (!vocab_tone_start(&tone, cue)) return 0;
    size_t total = 0;
    for (;;) {
        const size_t room = MAX_SAMPLES - total;
        const size_t n = vocab_tone_render(&tone, out + total, chunk < room ? chunk : room);
        if (n == 0) break;
        total += n;
    }
    return total;
}

static void test_tables(void)
{
    for (size_t i = 0; i < N_EXPECT; i++) {
        const expect_t *e = &EXPECT[i];
        const vocab_tone_seg_t *segs = NULL;
        const size_t n = vocab_tone_segments(e->cue, &segs);
        CHECK(n == e->n && segs != NULL, "%s: %zu segments", e->name, n);
        uint32_t total_ms = 0;
        for (size_t s = 0; s < n && s < e->n; s++) {
            CHECK(segs[s].f0_hz == e->segs[s].f0_hz && segs[s].f1_hz == e->segs[s].f1_hz &&
                      segs[s].ms == e->segs[s].ms,
                  "%s segment %zu: %u -> %u Hz, %u ms", e->name, s, (unsigned)segs[s].f0_hz,
                  (unsigned)segs[s].f1_hz, (unsigned)segs[s].ms);
            total_ms += e->segs[s].ms;
        }
        CHECK(vocab_tone_total_samples(e->cue) == total_ms * SPMS, "%s: total %u samples", e->name,
              (unsigned)vocab_tone_total_samples(e->cue));
        CHECK(vocab_tone_total_samples(e->cue) <= MAX_SAMPLES, "%s fits the test buffer", e->name);
    }
    CHECK(vocab_tone_total_samples(VOCAB_CUE_TICK) == 40u * SPMS, "TICK is 40 ms");
    CHECK(vocab_tone_total_samples(VOCAB_CUE_LEVEL_UP) == 700u * SPMS, "LEVEL_UP is 700 ms");
}

static void test_none_and_unknown(void)
{
    const vocab_cue_t bad[] = { VOCAB_CUE_NONE, VOCAB_CUE_COUNT, (vocab_cue_t)99 };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const vocab_tone_seg_t *segs = (const vocab_tone_seg_t *)&bad;
        CHECK(vocab_tone_segments(bad[i], &segs) == 0 && segs == NULL, "cue %d: no segments",
              (int)bad[i]);
        CHECK(vocab_tone_segments(bad[i], NULL) == 0, "cue %d: NULL out accepted", (int)bad[i]);
        CHECK(vocab_tone_total_samples(bad[i]) == 0, "cue %d: no samples", (int)bad[i]);
        vocab_tone_t tone;
        CHECK(!vocab_tone_start(&tone, bad[i]), "cue %d: start fails", (int)bad[i]);
        int16_t out[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
        CHECK(vocab_tone_render(&tone, out, 8) == 0 && out[0] == 1, "cue %d: renders nothing",
              (int)bad[i]);
    }
}

static void test_waveform(const expect_t *e)
{
    const uint32_t total = vocab_tone_total_samples(e->cue);
    memset(s_one, 0x55, sizeof(s_one));
    CHECK(render_all(e->cue, s_one, MAX_SAMPLES) == total, "%s: one-shot length", e->name);

    int32_t peak = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (iabs32(s_one[i]) > peak) peak = iabs32(s_one[i]);
    }
    CHECK(peak <= VOCAB_TONE_PEAK, "%s: peak %d <= -6 dBFS", e->name, (int)peak);
    CHECK(peak <= e->amp && peak >= e->amp * 98 / 100, "%s: peak %d near the cue level %d",
          e->name, (int)peak, (int)e->amp);

    uint32_t start = 0;
    for (size_t s = 0; s < e->n; s++) {
        const vocab_tone_seg_t *seg = &e->segs[s];
        const uint32_t len = seg->ms * SPMS;
        const int16_t *x = s_one + start;
        CHECK(x[0] == 0 && x[len - 1] == 0, "%s segment %zu: starts and ends at 0", e->name, s);
        for (uint32_t i = 0; i < EDGE; i++) {
            const int32_t attack_bound = e->amp * (int32_t)i / (int32_t)EDGE + 1;
            CHECK(iabs32(x[i]) <= attack_bound, "%s segment %zu: attack sample %u = %d", e->name,
                  s, (unsigned)i, x[i]);
            const uint32_t j = len - 1u - i;
            CHECK(iabs32(x[j]) <= attack_bound, "%s segment %zu: release sample %u = %d", e->name,
                  s, (unsigned)j, x[j]);
        }
        // Sustain: frequency from zero crossings, level from the mean square.
        const uint32_t a = EDGE;
        const uint32_t b = len - EDGE;
        const uint32_t crossings = zero_crossings(x, a, b);
        uint64_t sumsq = 0;
        for (uint32_t i = a; i < b; i++) sumsq += (uint64_t)((int64_t)x[i] * x[i]);
        if (seg->f0_hz == seg->f1_hz) {
            const uint32_t expected = 2u * seg->f0_hz * (b - a) / VOCAB_AUDIO_RATE_HZ;
            CHECK(crossings + 2u >= expected && crossings <= expected + 2u,
                  "%s segment %zu: %u crossings, expected about %u (%u Hz)", e->name, s,
                  (unsigned)crossings, (unsigned)expected, (unsigned)seg->f0_hz);
            // Mean square of a sine of amplitude A is A^2 / 2 (within 3 %).
            const uint64_t ms = sumsq / (b - a);
            const uint64_t ref = (uint64_t)((int64_t)e->amp * e->amp) / 2u;
            CHECK(ms * 100u >= ref * 97u && ms * 100u <= ref * 103u,
                  "%s segment %zu: sustain mean square %llu, sine %llu", e->name, s,
                  (unsigned long long)ms, (unsigned long long)ref);
        } else {
            // Linear sweep: the average frequency over [a, b) is the mid value there.
            const uint32_t f_a = seg->f0_hz + (seg->f1_hz - seg->f0_hz) * a / len;
            const uint32_t f_b = seg->f0_hz + (seg->f1_hz - seg->f0_hz) * b / len;
            const uint32_t expected = (f_a + f_b) * (b - a) / VOCAB_AUDIO_RATE_HZ;
            CHECK(crossings + 3u >= expected && crossings <= expected + 3u,
                  "%s sweep: %u crossings, expected about %u", e->name, (unsigned)crossings,
                  (unsigned)expected);
            const uint32_t mid = (a + b) / 2u;
            const uint32_t first = zero_crossings(x, a, mid);
            const uint32_t second = zero_crossings(x, mid, b);
            CHECK(second > first + first / 5u, "%s sweep rises: %u then %u crossings", e->name,
                  (unsigned)first, (unsigned)second);
        }
        start += len;
    }
    CHECK(start == total, "%s: segments cover the cue", e->name);
}

static void test_chunking(const expect_t *e)
{
    const uint32_t total = vocab_tone_total_samples(e->cue);
    render_all(e->cue, s_one, MAX_SAMPLES);
    const size_t chunks[] = { 1, 2, 7, 64, 255, VOCAB_AUDIO_CHUNK_SAMPLES, total - 1u, total + 5u };
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        memset(s_chunked, 0x55, sizeof(s_chunked));
        CHECK(render_all(e->cue, s_chunked, chunks[c]) == total, "%s chunk %zu: length", e->name,
              chunks[c]);
        CHECK(memcmp(s_one, s_chunked, total * sizeof(int16_t)) == 0,
              "%s chunk %zu: same samples as one call", e->name, chunks[c]);
    }

    // Plain-data state: a copy continues exactly like the original; zero-size requests
    // do not advance; a finished cue renders nothing.
    vocab_tone_t tone;
    CHECK(vocab_tone_start(&tone, e->cue), "%s: start", e->name);
    int16_t head[100];
    CHECK(vocab_tone_render(&tone, head, 100) == 100, "%s: first 100", e->name);
    CHECK(vocab_tone_render(&tone, head, 0) == 0, "%s: zero-size request", e->name);
    vocab_tone_t copy = tone;
    const size_t rest = total - 100u;
    CHECK(vocab_tone_render(&tone, s_chunked, MAX_SAMPLES) == rest, "%s: rest", e->name);
    CHECK(memcmp(s_chunked, s_one + 100, rest * sizeof(int16_t)) == 0, "%s: rest matches", e->name);
    CHECK(vocab_tone_render(&copy, s_chunked, MAX_SAMPLES) == rest &&
              memcmp(s_chunked, s_one + 100, rest * sizeof(int16_t)) == 0,
          "%s: copied state continues identically", e->name);
    CHECK(vocab_tone_render(&tone, s_chunked, MAX_SAMPLES) == 0, "%s: finished", e->name);

    // Restarting reproduces the cue (no hidden state).
    render_all(e->cue, s_chunked, 333);
    CHECK(memcmp(s_one, s_chunked, total * sizeof(int16_t)) == 0, "%s: restart identical", e->name);
}

int main(void)
{
    test_tables();
    test_none_and_unknown();
    for (size_t i = 0; i < N_EXPECT; i++) {
        test_waveform(&EXPECT[i]);
        test_chunking(&EXPECT[i]);
    }
    return vocab_test_report("vocab tone tests");
}
