// main/vocab_idle.h - pure idle / power timeline (spec 9).
//
// ACTIVE -> DIM at VOCAB_IDLE_DIM_MS -> OFF at VOCAB_IDLE_OFF_MS -> SLEEP
// VOCAB_DEEP_SLEEP_AFTER_OFF_MS after OFF, all measured from the last PRESS. Any PRESS
// returns to ACTIVE. A refused deep sleep (key held) retries after VOCAB_SLEEP_RETRY_MS.
// The engine executes the transitions (backlight, save, deep-sleep sequence).
//
// Caller: the engine on the app task (and host tests); never blocks; no allocation.
// Owner: lead developer (Milestone 1 skeleton; tested in tests/test_vocab_idle.c in M2).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocab_types.h"

typedef enum {
    VOCAB_POWER_ACTIVE = 0,  // backlight VOCAB_BACKLIGHT_ON
    VOCAB_POWER_DIM,         // backlight VOCAB_BACKLIGHT_DIM, keys act normally
    VOCAB_POWER_OFF,         // backlight 0, state frozen, next key only wakes
    VOCAB_POWER_SLEEP,       // deep sleep requested
} vocab_power_t;

typedef struct {
    vocab_power_t state;
    vocab_ms_t last_activity;   // last PRESS
    vocab_ms_t sleep_at;        // when SLEEP is due (valid in OFF)
} vocab_idle_t;

void vocab_idle_init(vocab_idle_t *idle, vocab_ms_t now);

// Any PRESS (even if its intent is dropped): back to ACTIVE, timeline restarts.
void vocab_idle_activity(vocab_idle_t *idle, vocab_ms_t now);

// Advances the timeline to `now`; returns the (possibly new) state.
vocab_power_t vocab_idle_update(vocab_idle_t *idle, vocab_ms_t now);

// Deep sleep was refused because a key is held: stay OFF and retry later.
void vocab_idle_sleep_refused(vocab_idle_t *idle, vocab_ms_t now);

// Next time the state can change by itself; false when none (SLEEP).
bool vocab_idle_next_deadline(const vocab_idle_t *idle, vocab_ms_t *deadline);
