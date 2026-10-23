// main/vocab_rng.h - injected deterministic random number generator (spec 5.2 step 5).
//
// PCG32 (XSH-RR, 64-bit state, fixed stream). Every module that needs randomness takes a
// `vocab_rng_t *` parameter: the device seeds one instance from esp_random() at boot; host
// tests seed it with fixed values. Consecutive small seeds (1, 2, 3, ...) give independent
// sequences, so "over 10,000 seeds" tests are meaningful.
//
// Pure, no allocation, not thread-safe: one instance is used only by the app task.
// Owner: lead developer.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vocab_types.h"

typedef struct {
    uint64_t state;
} vocab_rng_t;

// Seeds the generator; any seed value (including 0) is valid.
void vocab_rng_seed(vocab_rng_t *rng, uint32_t seed);

// Next uniformly distributed 32-bit value.
uint32_t vocab_rng_next(vocab_rng_t *rng);

// Uniform integer in [0, bound) without modulo bias; returns 0 when bound is 0 or 1.
uint32_t vocab_rng_below(vocab_rng_t *rng, uint32_t bound);

// One fair bit (uses the high bit of the next value).
bool vocab_rng_bit(vocab_rng_t *rng);

// Fisher-Yates shuffle of `count` bank indices in place.
void vocab_rng_shuffle(vocab_rng_t *rng, vocab_index_t *items, size_t count);
