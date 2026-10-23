// main/vocab_stage1.h - Stage 1: first-encounter two-choice meaning card (spec 5).
//
// Implements the common stage contract of vocab_stage.h (read it first). Used for LEARN
// cards at mastery 0 and for inserted TEST_LATER / SPOT_CHECK / TOO_EASY_CHECK cards in
// Stage 1 format. Pure; caller: the session on the app task; never blocks.
//
// Required behavior (spec 5.1, 5.3, 11.5):
//
// begin():
//   - Correct side: one bit from in->rng (UP or DOWN, about 50/50 over seeds, AC-13). This
//     is the only RNG draw of a card: handle() and resume() never draw.
//   - View: card.frame on; card.word = entry text (VOCAB_RGB_TEXT); card.ipa = entry IPA
//     (VOCAB_RGB_DIM; hidden when the entry has none); card.prompt = VOCAB_STR_S1_PROMPT
//     (VOCAB_RGB_DIM); panel[correct] = entry zh, panel[other] = zh of in->lure_index,
//     both VOCAB_PANEL_S1, text_en = false; badge = vocab_view_card_badge(purpose, S1)
//     (NEW / TEST_AGAIN / SPOT_CHECK / TOO_EASY); hint.kind = VOCAB_HINT_REPLAY.
//   - Out: PLAY_WORD(uid). Result: RUNNING, unlocked.
//   - Unusable input -> DONE without an outcome, no view change, no command, no RNG draw:
//     bank/index/lure_index invalid, lure_index == index, rng NULL, the entry text or
//     either meaning NULL/empty or too long for a view label (never truncated: a cut
//     English word would be a misspelling), or a lure that fails
//     vocab_lure_acceptable(entry, lure, true). An IPA too long for its label is hidden.
// Question state:
//   - Correct key (first press): chosen panel -> VOCAB_PANEL_CORRECT; prompt =
//     VOCAB_STR_CORRECT in VOCAB_RGB_SUCCESS; PLAY_CUE(CUE_CORRECT); NEW_EPOCH; arm
//     VOCAB_TIMER_STAGE for VOCAB_CORRECT_FEEDBACK_MS; outcome CORRECT_FIRST; locked.
//     Badge, hint and the other panel stay. Timer -> DONE.
//   - Wrong key (first press) = generic error hold: in the same call hide the wrong panel,
//     restyle the correct panel VOCAB_PANEL_CORRECT (text kept), badge REMEMBER, prompt =
//     VOCAB_STR_HOLD_MEANING in VOCAB_RGB_YELLOW, word and IPA stay, hint NONE;
//     PLAY_CUE_THEN_WORD(CUE_WRONG, uid); NEW_EPOCH; arm VOCAB_TIMER_STAGE for
//     VOCAB_ERROR_HOLD_MS; outcome WRONG; locked.
//   - Lock end (timer): hint PRESS_UP or PRESS_DOWN (the correct side); correct panel
//     nudge_repeat = true; NEW_EPOCH; unlocked (wait part).
//   - Wait part: correct key -> DONE (no second outcome); other key -> one NUDGE
//     (anim.nudge_seq[correct]++), nothing else; OK -> PLAY_WORD.
//   - OK click in the question state: PLAY_WORD (replay). No outcome.
//   - UP LONG in the question state of a LEARN card with in->allow_too_easy: outcome
//     TOO_EASY + DONE (no view change, no command). Otherwise UP LONG is ignored (inserted
//     cards even when allow_too_easy is set, wait part, locks).
// Locks and timers:
//   - Every key is ignored in the two lock windows (correct feedback, error-hold lock),
//     also when delivered to the stage directly (AC-15: the correct key at 1499 ms does
//     nothing).
//   - Only VOCAB_TIMER_STAGE is used. A lock ends only when its timer event arrives at or
//     after the deadline (`now` of the arming call + duration); an earlier delivery re-arms
//     the remainder, so the window is enforced by time, not only by event order. Timer
//     events outside a lock window and VOCAB_TIMER_STAGE_AUX events are ignored.
// resume(): re-renders the stage region from the stage state (identical to the region the
//   session restored) and returns the current lock status; no audio, epoch or outcome. The
//   session never pauses inside a lock window; if it does anyway, resume() re-arms a full
//   window (one TIMER_START) so the card cannot get stuck.
//
// Owner: Stage 1 developer (vocab_stage1.c/.h, vocab_lure.c/.h, tests/test_vocab_stage1.c,
// tests/test_vocab_lure.c). The struct fields below are private to the owner and may
// change freely; others treat vocab_stage1_t as opaque storage.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocab_stage.h"

typedef struct {
    // Private (Stage 1 owner).
    const vocab_entry_t *entry;  // the card's word (points into in->bank, valid for the card)
    const vocab_entry_t *lure;   // the lure entry; only its Chinese meaning is shown
    vocab_purpose_t purpose;
    bool allow_too_easy;         // in->allow_too_easy on a LEARN card
    bool show_ipa;               // the entry has an IPA line that fits its label
    vocab_side_t correct;        // side (= panel index) of the correct meaning
    uint8_t phase;               // PHASE_* in vocab_stage1.c
    vocab_ms_t lock_end;         // deadline of the running lock window
} vocab_stage1_t;

vocab_stage_result_t vocab_stage1_begin(vocab_stage1_t *st, const vocab_stage_input_t *in,
                                        vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out);

vocab_stage_result_t vocab_stage1_handle(vocab_stage1_t *st, const vocab_event_t *ev,
                                         vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out);

vocab_stage_result_t vocab_stage1_resume(vocab_stage1_t *st, vocab_ms_t now,
                                         vocab_view_t *view, vocab_outbox_t *out);
