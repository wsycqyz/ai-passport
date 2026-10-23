// main/vocab_stage2.h - Stage 2: flashcard with self-rating (spec 6).
//
// Implements the common stage contract of vocab_stage.h (read it first). Used only for
// LEARN cards (mastery 1-2, and 3-5 without cloze data, or mastery 0 without a lure).
// Pure; caller: the session on the app task; never blocks.
//
// Required behavior (spec 6, 11.5; implemented in vocab_stage2.c, tested in
// tests/test_vocab_stage2.c):
//
// begin() - front:
//   - Word front (in->cloze_front false): card.frame; card.word = text (VOCAB_RGB_TEXT);
//     card.ipa (VOCAB_RGB_DIM, hidden when none); card.prompt = VOCAB_STR_S2_FRONT_WORD in
//     VOCAB_RGB_YELLOW; panels hidden.
//   - Cloze front (in->cloze_front true): card.sentence = entry example_cloze
//     (VOCAB_RGB_TEXT; always "____" for the target, so the length does not leak);
//     card.word and card.ipa hidden; card.prompt = VOCAB_STR_S2_FRONT_CLOZE in yellow.
//   - badge RECALL; hint.kind THINK; out: PLAY_WORD(uid), arm VOCAB_TIMER_STAGE for
//     VOCAB_S2_FRONT_MIN_MS. Result RUNNING, NOT locked (UP LONG / OK LONG stay live).
// Front:
//   - Clicks before the front timer fired are ignored (no change).
//   - Front timer: hint.kind FLIP (no epoch needed).
//   - Afterwards UP, DOWN or OK click flips to the back.
//   - UP LONG (any time on the front) with in->allow_too_easy: outcome TOO_EASY + DONE.
// Flip -> back:
//   - card.word = text (VOCAB_RGB_TEXT); IPA hidden; sentence hidden; card.meaning = zh
//     (VOCAB_RGB_YELLOW); card.example = example (VOCAB_RGB_SOFT); prompt hidden;
//     panel UP = VOCAB_STR_S2_KNOW (VOCAB_PANEL_KNOW), panel DOWN = VOCAB_STR_S2_FORGOT
//     (VOCAB_PANEL_FORGOT); badge SELF_RATE; hint REPLAY.
//   - out: PLAY_CUE_THEN_WORD(CUE_FLIP, uid), NEW_EPOCH. Unlocked.
// Back:
//   - UP (I know): anim.confirm_seq[0]++ (CONFIRM_FLASH), PLAY_CUE(CUE_TICK), NEW_EPOCH,
//     arm VOCAB_TIMER_STAGE for VOCAB_S2_CONFIRM_MS; outcome KNOW; locked. Timer -> DONE.
//   - DOWN (forgot, test me later): hide card.example, card.prompt = VOCAB_STR_S2_FORGOT_ACK
//     in VOCAB_RGB_WARN, PLAY_CUE(CUE_TICK), NEW_EPOCH, arm VOCAB_TIMER_STAGE for
//     VOCAB_S2_FORGOT_ACK_MS; outcome FORGOT; locked. Timer -> DONE. (The session inserts
//     the TEST_LATER card two cards later; never immediately.)
//   - OK: PLAY_WORD (replay). UP LONG ignored on the back.
// resume(): if the front minimum had not elapsed, show hint THINK again and re-arm the
//   front timer for the full VOCAB_S2_FRONT_MIN_MS; otherwise nothing.
//
// Implementation notes (Stage 2 owner; clarifications within the contract above):
// - Cloze front guard (spec 6.1, 12.1, AC-27): the cloze front is used only when
//   in->cloze_front is set AND example_cloze is exactly `example` with the entry text
//   (present once) replaced by VOCAB_CLOZE_BLANK, with no other underscore. Otherwise the
//   card silently uses the word front, so the sentence always shows exactly "____" and
//   never any English that is not bank text.
// - Front minimum: the front also counts as ready once `now` reaches the minimum's
//   deadline, even if the timer event has not been delivered yet (a click and the timer
//   due at the same instant). Such a flip appends TIMER_CANCEL(VOCAB_TIMER_STAGE) before
//   its commands. Stale stage timers on the back and VOCAB_TIMER_STAGE_AUX are ignored.
// - Too easy: accepted only with in->allow_too_easy AND purpose LEARN (spec 4.8, AC-22);
//   the TOO_EASY result carries no commands (the session shows the acknowledgement).
// - Badges: LEARN cards show RECALL (front) and SELF_RATE (back). If the session ever runs
//   an inserted card in Stage 2 format (fallback), both faces keep the purpose badge from
//   vocab_view_card_badge(); the results are still KNOW / FORGOT.
// - Every face is rendered from the stage state over the whole stage region (labels not
//   used by the face are hidden, the skeleton cleared); card.banner and session-owned
//   fields are never written. resume() re-renders the current face (idempotent) and, for
//   robustness only, re-arms the confirm / acknowledgement timer if called during those
//   windows (the session never pauses there), so the card can never get stuck.
// - Stage 2 never draws from in->rng.
//
// Owner: Stage 2 developer (vocab_stage2.c/.h, vocab_anticheat.c/.h,
// tests/test_vocab_stage2.c, tests/test_vocab_anticheat.c). The struct fields below are
// private to the owner and may change freely.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocab_stage.h"

typedef struct {
    // Private (Stage 2 owner); others treat vocab_stage2_t as opaque storage.
    const vocab_entry_t *entry;  // the card's bank entry (const flash data); NULL = unusable
    vocab_purpose_t purpose;
    vocab_ms_t front_deadline;   // end of the front minimum (absolute ms, wraps)
    bool cloze_front;            // the cloze-sentence front is shown (validated)
    bool allow_too_easy;         // UP LONG on the front ends the card with TOO_EASY
    uint8_t phase;               // vocab_stage2.c state
} vocab_stage2_t;

vocab_stage_result_t vocab_stage2_begin(vocab_stage2_t *st, const vocab_stage_input_t *in,
                                        vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out);

vocab_stage_result_t vocab_stage2_handle(vocab_stage2_t *st, const vocab_event_t *ev,
                                         vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out);

vocab_stage_result_t vocab_stage2_resume(vocab_stage2_t *st, vocab_ms_t now,
                                         vocab_view_t *view, vocab_outbox_t *out);
