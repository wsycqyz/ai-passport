// main/vocab_engine.h - pure top-level composition: input filter + idle policy + session.
//
// The app task (vocab_app.c) feeds raw key events and time, executes the returned commands
// and renders the view. The system-level host simulation (tests/test_vocab_sim.c) drives the
// same engine with scripted (time, key) events, so the device and the tests share one
// implementation of the whole state machine.
//
// Commands for the caller, in order:
//   PLAY_WORD / PLAY_CUE / PLAY_CUE_THEN_WORD / STOP_AUDIO   audio worker requests
//   SET_VOLUME (percent)                                     audio worker volume
//   NEW_EPOCH          drain the key queue through vocab_engine_key_drained()
//   SAVE (VOCAB_SAVE_POINT | VOCAB_SAVE_LOW_BATTERY)         run the store save gate
//   BACKLIGHT (percent)                                      bsp_display_backlight
//   READ_BATTERY       read bsp_battery_soc() and report it with vocab_engine_battery()
//   DEEP_SLEEP         run the deep-sleep sequence; if a key is held, report
//                      vocab_engine_sleep_refused() instead
// Power: ACTIVE -> DIM (20 s) -> OFF (30 s: stop audio, save, backlight 0) -> SLEEP (150 s:
// save, deep sleep). Any PRESS returns to ACTIVE; while OFF the first PRESS only wakes
// (backlight, battery reading, new epoch) and the waking press is swallowed (input R2).
//
// Caller: the app task or host tests; never blocks; no allocation.
// Owner: lead developer (tests/test_vocab_sim.c, tests/test_vocab_idle.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocab_idle.h"
#include "vocab_input.h"
#include "vocab_rng.h"
#include "vocab_session.h"
#include "vocab_types.h"
#include "vocab_view.h"

typedef struct {
    vocab_session_config_t session;  // `rng` is ignored: the engine owns its generator
    uint32_t rng_seed;               // device: esp_random(); tests: fixed
} vocab_engine_config_t;

typedef struct {
    vocab_rng_t rng;
    vocab_session_t session;
    vocab_input_t input;
    vocab_idle_t idle;
    vocab_power_t power;
    vocab_ms_t battery_due;          // next periodic battery reading
} vocab_engine_t;

// Shows HOME; emits BACKLIGHT(on) and READ_BATTERY.
void vocab_engine_init(vocab_engine_t *eng, const vocab_engine_config_t *cfg, vocab_ms_t now,
                       vocab_view_t *view, vocab_outbox_t *out);

// One raw key event from the BSP callback queue.
void vocab_engine_key(vocab_engine_t *eng, const vocab_raw_event_t *raw, vocab_ms_t now,
                      vocab_view_t *view, vocab_outbox_t *out);

// A raw event drained from the queue after NEW_EPOCH: bookkeeping and activity only.
void vocab_engine_key_drained(vocab_engine_t *eng, const vocab_raw_event_t *raw, vocab_ms_t now);

// Time passed: fires session timers, idle transitions and the battery poll.
void vocab_engine_poll(vocab_engine_t *eng, vocab_ms_t now, vocab_view_t *view,
                       vocab_outbox_t *out);

// Earliest time at which vocab_engine_poll() has work; false when nothing is scheduled.
bool vocab_engine_next_deadline(const vocab_engine_t *eng, vocab_ms_t *deadline);

// Result of READ_BATTERY (-1 = unavailable).
void vocab_engine_battery(vocab_engine_t *eng, int pct, vocab_view_t *view);

// HOME storage warning after a save attempt (vocab_store_warning()).
void vocab_engine_storage_warning(vocab_engine_t *eng, bool warning, vocab_view_t *view);

// The deep-sleep sequence refused to start (a key is held): retry later (spec 9).
void vocab_engine_sleep_refused(vocab_engine_t *eng, vocab_ms_t now);

// Current power state (the app stops animations while OFF/SLEEP).
vocab_power_t vocab_engine_power(const vocab_engine_t *eng);

// Values to persist (tick, XP, volume, statistics).
vocab_session_persist_t vocab_engine_persist(const vocab_engine_t *eng);
