// main/vocab_input.h - pure input filter: raw BSP key events -> intents (spec 3.1, 3.3).
//
// Applies the input-hygiene rules in spec order and reports activity for the idle timers:
//   R1 normalize: DOUBLE -> one CLICK; PRESS/RELEASE are bookkeeping (and PRESS = activity)
//   R2 wake-only first key: while the screen is off the first PRESS only wakes; every CLICK
//      and LONG is dropped until VOCAB_WAKE_TAIL_MS after that press's RELEASE (safety
//      disarm VOCAB_WAKE_GUARD_MAX_MS after the wake). A second press of the waking key that
//      continues its press sequence (see below) restarts the tail at its own RELEASE, so a
//      waking double tap (one DOUBLE) is swallowed as a whole.
//   R3 epoch: an intent whose PRESS began before the current epoch, or without a recorded
//      PRESS, is dropped
//   R4 cooldown: UP/DOWN CLICKs whose PRESS began within VOCAB_CARD_COOLDOWN_MS after the
//      epoch are dropped (press time, because the CLICK arrives ~180 ms after the release)
//   R5 lock: while the session reports a lock only DOWN LONG passes
//   R6 cross-key phantom: an intent whose PRESS began while another key was held is dropped
//      (held flags older than VOCAB_WAKE_GUARD_MAX_MS are ignored)
//   R7 short press: a CLICK whose press lasted < VOCAB_MIN_PRESS_MS is dropped
//   R8 long-hold rejoin: a press starting < VOCAB_LONG_REJOIN_MS after the same key's
//      release of a press that produced LONG is a bounce; its CLICK/LONG are dropped
//   R9 (one change per epoch) is done by the session emitting VOCAB_CMD_NEW_EPOCH.
//
// Press sequences (DOUBLE): the button driver reports two quick presses of one key as a
// single DOUBLE about 185 ms after the second RELEASE. A PRESS that starts within
// VOCAB_DOUBLE_WINDOW_MS after the same key's RELEASE continues that key's press sequence
// unless the previous press produced LONG or the sequence was already closed by a CLICK,
// DOUBLE or LONG event. For a DOUBLE the rules use the whole sequence: R3/R4 the start of
// its FIRST press, R6 a cross-key start of any of its presses, R7 its longest press (a real
// press inside the gesture keeps it), R8 a bounce start of the sequence.
//
// Caller: the engine on the app task (and host tests); never blocks; no allocation. Event
// times are the raw capture times (ms).
// Owner: lead developer (tests/test_vocab_input.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocab_types.h"

// Why an event produced no intent (for tests and logs).
typedef enum {
    VOCAB_DROP_NONE = 0,     // accepted, or a bookkeeping event (PRESS/RELEASE)
    VOCAB_DROP_INVALID,      // bad key/kind
    VOCAB_DROP_WAKE,         // R2
    VOCAB_DROP_EPOCH,        // R3 (also: no recorded PRESS, or drained)
    VOCAB_DROP_COOLDOWN,     // R4
    VOCAB_DROP_LOCKED,       // R5
    VOCAB_DROP_CROSS_KEY,    // R6
    VOCAB_DROP_SHORT,        // R7
    VOCAB_DROP_REJOIN,       // R8
} vocab_drop_t;

typedef struct {
    vocab_ms_t press_ms;       // start of the key's current/last PRESS
    vocab_ms_t release_ms;     // time of the key's last RELEASE
    bool has_press;            // a PRESS was recorded since init
    bool held;                 // PRESS seen without its RELEASE
    bool released;             // the current press has been released
    bool long_fired;           // the current/last press produced LONG
    bool cross;                // R6: the press began while another key was held
    bool rejoin;               // R8: the press is a bounce continuation of a LONG
    // Press sequence (one DOUBLE = two presses of one sequence).
    vocab_ms_t seq_ms;         // start of the sequence's first PRESS
    uint32_t seq_max_ms;       // longest released press of the sequence (R7 for DOUBLE)
    bool seq_open;             // a following quick PRESS may still join the sequence
    bool seq_cross;            // some press of the sequence began during another key's hold
    bool seq_rejoin;           // the sequence began as a LONG-release bounce
} vocab_key_state_t;

typedef struct {
    vocab_ms_t epoch_ms;                      // start of the current input epoch
    vocab_key_state_t key[VOCAB_KEY_COUNT];
    bool wake_guard;                          // R2 active
    bool wake_released;                       // the waking press has been released
    vocab_key_t wake_key;
    vocab_ms_t wake_ms;                       // wake PRESS time
    vocab_ms_t wake_release_ms;               // its RELEASE time
} vocab_input_t;

typedef struct {
    bool activity;        // a PRESS was seen: restart the idle timeline (spec 9)
    bool wake;            // this PRESS woke the screen (backlight on, new epoch)
    bool intent;          // `event` holds an accepted CLICK / LONG
    vocab_event_t event;
    vocab_drop_t drop;    // reason when a CLICK/LONG/DOUBLE was dropped
} vocab_input_result_t;

// Clears all bookkeeping; the first epoch starts at `now`.
void vocab_input_init(vocab_input_t *in, vocab_ms_t now);

// Starts a new input epoch at `now` (R3/R4).
void vocab_input_new_epoch(vocab_input_t *in, vocab_ms_t now);

// Filters one raw event. `locked`: the session reports a lock window (R5). `screen_off`:
// the backlight is off (R2: a PRESS only wakes and arms the wake guard).
vocab_input_result_t vocab_input_feed(vocab_input_t *in, const vocab_raw_event_t *raw,
                                      bool locked, bool screen_off);

// Queue drain after an epoch (spec 3.3 R3 flush): PRESS/RELEASE still update the
// bookkeeping (and a PRESS still counts as activity) but no intent is ever produced, so a
// RELEASE in the flushed part cannot leave a key looking held.
vocab_input_result_t vocab_input_drain(vocab_input_t *in, const vocab_raw_event_t *raw);
