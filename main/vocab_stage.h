// main/vocab_stage.h - the common contract between the session and the three stage modules.
//
// A stage module runs ONE card: Stage 1 (vocab_stage1.h, two-choice meaning), Stage 2
// (vocab_stage2.h, flashcard + self-rating) or Stage 3 (vocab_stage3.h, phonics cloze).
// Stages are pure state machines: no ESP-IDF/LVGL includes, no hardware calls, no clock
// reads, no allocation, no static mutable state. They receive events, update the view
// model's stage region and append commands to an outbox. Host tests drive them directly.
//
// Every stage module exposes exactly these three functions (N = 1, 2, 3):
//
//   vocab_stage_result_t vocab_stageN_begin(vocab_stageN_t *st, const vocab_stage_input_t *in,
//                                            vocab_ms_t now, vocab_view_t *view,
//                                            vocab_outbox_t *out);
//   vocab_stage_result_t vocab_stageN_handle(vocab_stageN_t *st, const vocab_event_t *ev,
//                                             vocab_ms_t now, vocab_view_t *view,
//                                             vocab_outbox_t *out);
//   vocab_stage_result_t vocab_stageN_resume(vocab_stageN_t *st, vocab_ms_t now,
//                                             vocab_view_t *view, vocab_outbox_t *out);
//
// Caller and context: only the session (vocab_session.c) calls them, on the app task (or a
// host test). Calls never block and return within microseconds. The session owns the
// vocab_stageN_t storage (a union member); the struct's fields are private to the stage
// owner. `out` is cleared by the caller; a stage only appends. `view` is never NULL.
//
// 1. Lifecycle
//    - begin(): called once per card after the session reset the stage region
//      (vocab_view_reset_card) and started the card's input epoch. Copy what you need from
//      `in` (the pointer is not valid after the call; in->bank and in->rng stay valid for
//      the whole card). Fill the view, request the entry audio (VOCAB_CMD_PLAY_WORD) and
//      arm timers as needed.
//    - handle(): one event per call (see 2). Return DONE when the card is finished; the
//      session then cancels the stage timers and moves on. No call follows DONE.
//    - resume(): after a pause card (OK LONG) the session restores the stage region of the
//      view exactly as it was, cancelled both stage timers before the pause and starts a
//      new input epoch. Re-arm any timer the current state needs (e.g. the Stage 2 front
//      minimum) and adjust the view if needed. Do not replay audio, do not report an
//      outcome. A card the child leaves through pause -> HOME just stops: no call.
//    - If `in` is unusable (programming error, e.g. Stage 3 without cloze data), begin()
//      returns DONE without an outcome; the session logs and skips the card.
//
// 2. Events a stage receives
//    - VOCAB_EV_CLICK with key UP, DOWN or OK (BSP CLICK and DOUBLE already merged).
//    - VOCAB_EV_LONG with key UP only ("too easy"). DOWN LONG (volume) and OK LONG (pause)
//      are global and handled by the session; they never reach a stage.
//    - VOCAB_EV_TIMER for VOCAB_TIMER_STAGE / VOCAB_TIMER_STAGE_AUX that the stage armed.
//    - No intents are delivered while the last returned result had `locked` set; timers
//      are. Input hygiene (cooldown, wake swallowing, phantom keys) is done before the
//      stage sees anything; a stage must still ignore events that make no sense in its
//      current state (no view change, return the current status). In particular a key
//      that reaches a stage during its own lock window (possible when tests drive a stage
//      directly) is ignored, so lock behavior (e.g. AC-15) is testable at stage level.
//
// 3. Outcome (spec 4.3)
//    - Report the learning result exactly once per card, in the call where it becomes
//      final: set outcome_ready = true and `result`. "The first press decides" objective
//      cards; Stage 3 LEARN decides at the second press after a first miss (O8/O9).
//    - The session applies it immediately (SRS update, XP counters, insertions), so the
//      result counts even if the child pauses and leaves during the following hold.
//    - VOCAB_RESULT_TOO_EASY: only when in->allow_too_easy is true and the stage is in the
//      Stage 1 question / Stage 2 front state; return DONE in the same call (the session
//      shows the 800 ms acknowledgement and inserts the check card). Otherwise ignore UP LONG.
//    - A card may end (DONE) in a later call than the outcome (e.g. after an error hold).
//
// 4. Timers
//    - Arm with vocab_out_timer_start(out, VOCAB_TIMER_STAGE[_AUX], ms); one-shot; re-arming
//      replaces the pending deadline; vocab_out_timer_cancel() disarms. The deadline is
//      measured from the `now` of the call that armed it. Each arm fires at most once.
//    - Use the constants of vocab_config.h (VOCAB_ERROR_HOLD_MS, VOCAB_CORRECT_FEEDBACK_MS,
//      VOCAB_S3_RETRY_LOCK_MS, VOCAB_SUCCESS_HOLD_MS, VOCAB_S2_*). Never sleep or busy-wait.
//    - Repeating visual effects (NUDGE every 1000 ms, BLANK_BLINK) are renderer flags, not
//      timers: set panel[i].nudge_repeat / card.skeleton.blink.
//
// 5. Lock windows (spec 3.2 R5)
//    - Return locked = true while the spec ignores every key except DOWN LONG: correct
//      feedback, error-hold / reveal-hold lock part, Stage 3 retry lock, Stage 3 success
//      hold, Stage 2 confirm and forgot acknowledgement. The session then drops all
//      intents except DOWN LONG (volume) and does not offer the pause card.
//    - The Stage 2 front minimum (VOCAB_S2_FRONT_MIN_MS) is NOT a lock: UP LONG (too easy)
//      and OK LONG (pause) stay available; the stage itself ignores clicks during it.
//
// 6. Input epochs (spec 3.3 R3/R4/R9)
//    - Append VOCAB_CMD_NEW_EPOCH (vocab_out_new_epoch) on every state change caused by an
//      accepted key or a timer: hold start, hold end (lock -> wait), flip to the back,
//      feedback start, retry-lock start and end. Never in begin() or resume(): the session
//      starts those epochs. The epoch flushes queued input and restarts the 250 ms UP/DOWN
//      cooldown, so leftovers of the previous state never act on the new one.
//
// 7. Audio (spec 8.3)
//    - Request sounds with vocab_out_play_word / vocab_out_play_cue /
//      vocab_out_play_cue_then_word / vocab_out_stop_audio using the card's uid.
//    - Never check volume or clip availability: when a word clip cannot play the session
//      drops the request and runs WORD_PULSE instead (anim.pulse_seq), skips cues when
//      muted, and resolves VOCAB_HINT_REPLAY to "look again". Do not touch anim.pulse_seq.
//
// 8. View (vocab_view.h)
//    - Write only the stage region: card.frame/word/ipa/meaning/sentence/example/
//      s3_meaning/skeleton/prompt, panel[0..1], badge, hint.kind, anim.shake_seq,
//      anim.nudge_seq[], anim.confirm_seq[]. Leave card.banner hidden.
//    - Use the helpers vocab_view_show/hide/set_panel/hide_panel/set_skeleton, the strings
//      of vocab_strings.h, the palette VOCAB_RGB_*, vocab_view_card_badge() for the
//      default badge. Do not write Chinese literals in stage sources.
//    - Never show a misspelled English string (spec 1 / AC-27). Allowed English texts:
//      the entry text, its example and example_cloze, its IPA, an isolated cloze chunk in
//      an option panel (text_en = true), and the Stage 3 skeleton (pre/suf of the entry
//      text, the right chunk in the blank, the other chunk only in skeleton.other). The
//      wrong chunk is never placed in the blank, the word, or any other label.
//
// Owner: lead developer (contract). After Milestone 1 only additive changes.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocab_rng.h"
#include "vocab_types.h"
#include "vocab_view.h"

// Everything a stage needs to run one card. Built by the session (spec 4.5, 4.6).
typedef struct {
    const vocab_bank_t *bank;      // the whole bank (lure lookup); valid for the whole card
    vocab_index_t index;           // bank index of the card's word
    vocab_purpose_t purpose;       // LEARN, TEST_LATER, SPOT_CHECK, TOO_EASY_CHECK
    vocab_format_t format;         // the stage being called (S1, S2 or S3)
    uint8_t mastery;               // the word's mastery when the card is shown (info only)
    vocab_index_t lure_index;      // S1 only: bank index of the lure entry (!= index), chosen
                                   // by the session with vocab_lure_pick(); else NONE
    bool cloze_front;              // S2 only: show the cloze-sentence front (the session
                                   // checked example_cloze AND word audio, spec 4.6)
    bool allow_too_easy;           // UP LONG may end the card with VOCAB_RESULT_TOO_EASY
                                   // (LEARN card and the check card can be inserted)
    vocab_rng_t *rng;              // injected RNG (correct-side bit); valid for the card
} vocab_stage_input_t;

typedef enum {
    VOCAB_STAGE_RUNNING = 0,
    VOCAB_STAGE_DONE = 1,
} vocab_stage_status_t;

// Returned by every begin/handle/resume call.
typedef struct {
    vocab_stage_status_t status;  // DONE: the card is finished
    bool locked;                  // lock window active (see 5); ignored when DONE
    bool outcome_ready;           // `result` is final and must be applied now (see 3)
    vocab_result_t result;        // valid when outcome_ready
} vocab_stage_result_t;

static inline vocab_stage_result_t vocab_stage_running(bool locked)
{
    const vocab_stage_result_t r = { VOCAB_STAGE_RUNNING, locked, false, VOCAB_RESULT_NONE };
    return r;
}

static inline vocab_stage_result_t vocab_stage_done(void)
{
    const vocab_stage_result_t r = { VOCAB_STAGE_DONE, false, false, VOCAB_RESULT_NONE };
    return r;
}

// Returns `r` with the outcome attached.
static inline vocab_stage_result_t vocab_stage_with_outcome(vocab_stage_result_t r,
                                                            vocab_result_t result)
{
    r.outcome_ready = true;
    r.result = result;
    return r;
}

// Number of answer presses after which a Stage 3 card reveals (spec 7.4, Appendix A).
static inline uint8_t vocab_stage_s3_max_misses(vocab_purpose_t purpose)
{
    return purpose == VOCAB_PURPOSE_LEARN ? (uint8_t)VOCAB_S3_MAX_MISSES_LEARN
                                          : (uint8_t)VOCAB_S3_MAX_MISSES_CHECK;
}
