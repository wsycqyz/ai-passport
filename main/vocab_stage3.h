// main/vocab_stage3.h - Stage 3: phonics cloze card (spec 7).
//
// Implements the common stage contract of vocab_stage.h (read it first). Used for LEARN
// cards at mastery 3-5 with cloze data and for SPOT_CHECK / TOO_EASY_CHECK cards in Stage 3
// format. The session only selects Stage 3 for entries with cloze data
// (vocab_entry_has_cloze). Pure; caller: the session on the app task; never blocks.
//
// Behavior (spec 7.3, 7.4, 8.3, 11.5, 11.6; AC-24, AC-25, AC-27):
//
// begin():
//   - Unusable input (no entry, no RNG, no or inconsistent cloze data - see
//     vocab_cloze_split) -> DONE without an outcome and without touching the view.
//   - Correct side: exactly one bit from in->rng (about 50/50 over seeds, AC-25).
//   - View: card.frame; card.s3_meaning = entry zh (VOCAB_RGB_YELLOW); skeleton via
//     vocab_view_set_skeleton(view, pre, suf, right, wrong) using vocab_cloze_split()
//     (blank empty and blinking; spaces of phrases kept, e.g. "ice cr" + blank + "m");
//     card.prompt = VOCAB_STR_S3_PROMPT (VOCAB_RGB_DIM); IPA and the word never shown;
//     panel[correct] = right chunk, panel[other] = wrong chunk, VOCAB_PANEL_S3,
//     text_en = true; badge = vocab_view_card_badge(purpose, S3) (SPELL / SPOT_CHECK /
//     TOO_EASY / TEST_AGAIN); hint REPLAY. Out: PLAY_WORD(uid). RUNNING, unlocked.
//   - Never display a wrong spelling: the wrong chunk appears only isolated in its panel
//     (and in skeleton.other for sizing); the blank shows nothing or the right chunk. The
//     skeleton strings never change during the card, so the row never re-flows.
// Misses allowed: vocab_stage_s3_max_misses(purpose) (LEARN 2, check cards 1).
//   - Correct (first press -> outcome CORRECT_FIRST; second press -> CORRECT_SECOND):
//     skeleton.filled = true, blink off; chosen panel VOCAB_PANEL_CORRECT (the other panel
//     stays); prompt = VOCAB_STR_S3_SUCCESS in VOCAB_RGB_SUCCESS;
//     PLAY_CUE_THEN_WORD(CUE_CHEER, uid); NEW_EPOCH; arm VOCAB_TIMER_STAGE for
//     VOCAB_SUCCESS_HOLD_MS; locked. Timer -> DONE.
//   - Miss that is not the last allowed one (LEARN miss 1): blank stays empty, blink off,
//     anim.shake_seq++; prompt = VOCAB_STR_S3_RETRY in VOCAB_RGB_WARN; both options stay;
//     PLAY_CUE_THEN_WORD(CUE_WRONG, uid); NEW_EPOCH; arm VOCAB_TIMER_STAGE for
//     VOCAB_S3_RETRY_LOCK_MS; no outcome yet; locked. Timer -> NEW_EPOCH, blink on again,
//     unlocked; the retry prompt stays for the second attempt.
//   - Final miss (LEARN miss 2, or check-card miss 1) = reveal + generic error hold: hide
//     the wrong panel; skeleton.filled = true (right chunk in green, full correct word),
//     blink off; correct panel VOCAB_PANEL_CORRECT; badge REMEMBER; prompt =
//     VOCAB_STR_S3_REVEAL in VOCAB_RGB_YELLOW; hint NONE; PLAY_CUE_THEN_WORD(CUE_WRONG, uid);
//     NEW_EPOCH; arm VOCAB_TIMER_STAGE for VOCAB_ERROR_HOLD_MS; outcome WRONG; locked.
//     Lock end: hint PRESS_UP/PRESS_DOWN, correct panel nudge_repeat, NEW_EPOCH, unlocked.
//     Wait part: correct key -> DONE (no second outcome); other key ->
//     anim.nudge_seq[correct]++ only; OK -> PLAY_WORD.
//   - OK click while waiting for an answer: PLAY_WORD (whole-word replay; segmented
//     phonics deferred).
//   - Ignored (no view change, no command): UP LONG in every state (never "too easy" in
//     Stage 3, AC-22), every key during a lock, TIMER events of VOCAB_TIMER_STAGE_AUX and
//     stage timers in states without a lock, every call after DONE (returns DONE).
// resume(): pauses happen only while unlocked, so nothing needs re-arming; returns the
//   current lock status. Defensive: in a lock state the stage timer is re-armed with the
//   remaining time (at least 1 ms) so a lock can never get stuck.
//
// Owner: Stage 3 developer (vocab_stage3.c/.h, vocab_cloze.c/.h, tests/test_vocab_stage3.c,
// tests/test_vocab_cloze.c, assets/wordlists/**). The struct fields below are private to
// the owner and may change freely.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocab_stage.h"

typedef struct {
    // Private (Stage 3 owner).
    const vocab_bank_t *bank;
    vocab_index_t index;
    vocab_uid_t uid;
    vocab_purpose_t purpose;
    vocab_side_t correct;    // panel holding the right chunk
    uint8_t misses;          // wrong answers so far
    uint8_t max_misses;      // vocab_stage_s3_max_misses(purpose)
    uint8_t phase;           // private phase (vocab_stage3.c)
    vocab_ms_t lock_until;   // deadline of the running lock (retry, success, reveal)
} vocab_stage3_t;

vocab_stage_result_t vocab_stage3_begin(vocab_stage3_t *st, const vocab_stage_input_t *in,
                                        vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out);

vocab_stage_result_t vocab_stage3_handle(vocab_stage3_t *st, const vocab_event_t *ev,
                                         vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out);

vocab_stage_result_t vocab_stage3_resume(vocab_stage3_t *st, vocab_ms_t now,
                                         vocab_view_t *view, vocab_outbox_t *out);
