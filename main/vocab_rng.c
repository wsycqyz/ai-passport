// main/vocab_rng.c - PCG32 (XSH-RR) generator, see vocab_rng.h.
#include "vocab_rng.h"

#define PCG_MULT 6364136223846793005ULL
#define PCG_INC  1442695040888963407ULL  // odd stream constant

static uint32_t rotr32(uint32_t value, unsigned rot)
{
    return (value >> rot) | (value << ((32u - rot) & 31u));
}

uint32_t vocab_rng_next(vocab_rng_t *rng)
{
    const uint64_t old = rng->state;
    rng->state = old * PCG_MULT + PCG_INC;
    const uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    const unsigned rot = (unsigned)(old >> 59u);
    return rotr32(xorshifted, rot);
}

void vocab_rng_seed(vocab_rng_t *rng, uint32_t seed)
{
    // Standard PCG initialisation: advance once, add the seed, advance again.
    rng->state = 0u;
    (void)vocab_rng_next(rng);
    rng->state += (uint64_t)seed;
    (void)vocab_rng_next(rng);
}

uint32_t vocab_rng_below(vocab_rng_t *rng, uint32_t bound)
{
    if (bound <= 1u) return 0u;
    // Reject the low values that would bias the modulo (PCG reference technique).
    const uint32_t threshold = (uint32_t)(0u - bound) % bound;
    for (;;) {
        const uint32_t r = vocab_rng_next(rng);
        if (r >= threshold) return r % bound;
    }
}

bool vocab_rng_bit(vocab_rng_t *rng)
{
    return (vocab_rng_next(rng) >> 31) != 0u;
}

void vocab_rng_shuffle(vocab_rng_t *rng, vocab_index_t *items, size_t count)
{
    if (!items || count < 2) return;
    for (size_t i = count - 1; i > 0; i--) {
        const size_t j = (size_t)vocab_rng_below(rng, (uint32_t)(i + 1));
        const vocab_index_t tmp = items[i];
        items[i] = items[j];
        items[j] = tmp;
    }
}
