// main/vocab_stage1.c - Stage 1: first-encounter two-choice meaning card (spec 5.1, 5.3).
//
// See vocab_stage1.h for the behavior and vocab_stage.h for the stage contract. Pure state
// machine: no allocation, no static mutable state, no clock reads (time arrives as `now`),
// one RNG draw per card (begin). The stage region of the view is always rendered from the
// stage state by render(), so every update (and resume) shows a consistent card.
//
// Owner: Stage 1 developer.
#include "vocab_stage1.h"

#include <string.h>

#include "vocab_bank.h"
#include "vocab_config.h"
#include "vocab_lure.h"
#include "vocab_strings.h"

enum {
    PHASE_QUESTION = 0,  // both meanings shown, waiting for the first press
    PHASE_FEEDBACK,      // correct first press: locked for VOCAB_CORRECT_FEEDBACK_MS, then DONE
    PHASE_HOLD_LOCK,     // wrong first press: error-hold lock for VOCAB_ERROR_HOLD_MS
    PHASE_HOLD_WAIT,     // error-hold wait part: only the correct key finishes the card
    PHASE_DONE,
};

static bool phase_locked(uint8_t phase)
{
    return phase == PHASE_FEEDBACK || phase == PHASE_HOLD_LOCK;
}

static uint32_t phase_lock_ms(uint8_t phase)
{
    return phase == PHASE_FEEDBACK ? VOCAB_CORRECT_FEEDBACK_MS : VOCAB_ERROR_HOLD_MS;
}

static vocab_stage_result_t status(const vocab_stage1_t *st)
{
    return st->phase == PHASE_DONE ? vocab_stage_done()
                                   : vocab_stage_running(phase_locked(st->phase));
}

// A label text is usable when it is present and fits a view label without truncation.
static bool fits_label(const char *text)
{
    return text && text[0] != '\0' && strlen(text) < VOCAB_VIEW_TEXT_CAP;
}

static void arm_lock(vocab_stage1_t *st, vocab_ms_t now, uint32_t ms, vocab_outbox_t *out)
{
    st->lock_end = now + ms;
    vocab_out_timer_start(out, VOCAB_TIMER_STAGE, ms);
}

// Writes the whole stage region for the current phase (spec 11.5 rows "Stage 1",
// "Stage 1 correct", "Error hold (Stage 1)" and the inserted-card badges). Animation
// counters are never touched here; card.banner is session-owned and stays as it is.
static void render(const vocab_stage1_t *st, vocab_view_t *view)
{
    vocab_view_card_t *card = &view->card;
    const bool hold = st->phase == PHASE_HOLD_LOCK || st->phase == PHASE_HOLD_WAIT;
    const vocab_side_t other = vocab_side_other(st->correct);

    card->frame = true;
    vocab_view_show(&card->word, st->entry->text, VOCAB_RGB_TEXT);
    if (st->show_ipa) {
        vocab_view_show(&card->ipa, st->entry->ipa, VOCAB_RGB_DIM);
    } else {
        vocab_view_hide(&card->ipa);
    }
    vocab_view_hide(&card->meaning);
    vocab_view_hide(&card->sentence);
    vocab_view_hide(&card->example);
    vocab_view_hide(&card->s3_meaning);
    memset(&card->skeleton, 0, sizeof(card->skeleton));

    if (st->phase == PHASE_FEEDBACK) {
        vocab_view_show(&card->prompt, VOCAB_STR_CORRECT, VOCAB_RGB_SUCCESS);
    } else if (hold) {
        vocab_view_show(&card->prompt, VOCAB_STR_HOLD_MEANING, VOCAB_RGB_YELLOW);
    } else {
        vocab_view_show(&card->prompt, VOCAB_STR_S1_PROMPT, VOCAB_RGB_DIM);
    }

    // Chinese meanings only (text_en = false): English lures are never shown (AC-27).
    vocab_view_set_panel(view, st->correct, st->entry->zh,
                         st->phase == PHASE_QUESTION ? VOCAB_PANEL_S1 : VOCAB_PANEL_CORRECT,
                         false);
    if (hold) {
        vocab_view_hide_panel(view, other);
    } else {
        vocab_view_set_panel(view, other, st->lure->zh, VOCAB_PANEL_S1, false);
    }
    view->panel[st->correct].nudge_repeat = st->phase == PHASE_HOLD_WAIT;

    view->badge =
        hold ? VOCAB_BADGE_REMEMBER : vocab_view_card_badge(st->purpose, VOCAB_FORMAT_S1);
    if (st->phase == PHASE_HOLD_LOCK) {
        view->hint.kind = VOCAB_HINT_NONE;
    } else if (st->phase == PHASE_HOLD_WAIT) {
        view->hint.kind = st->correct == VOCAB_SIDE_UP ? VOCAB_HINT_PRESS_UP : VOCAB_HINT_PRESS_DOWN;
    } else {
        view->hint.kind = VOCAB_HINT_REPLAY;
    }
}

vocab_stage_result_t vocab_stage1_begin(vocab_stage1_t *st, const vocab_stage_input_t *in,
                                        vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out)
{
    (void)now;
    if (!st) return vocab_stage_done();
    memset(st, 0, sizeof(*st));
    st->phase = PHASE_DONE;  // until the input proves usable
    if (!in || !view || !out || !in->rng) return vocab_stage_done();

    const vocab_entry_t *entry = vocab_bank_at(in->bank, in->index);
    const vocab_entry_t *lure =
        in->lure_index == in->index ? NULL : vocab_bank_at(in->bank, in->lure_index);
    if (!entry || !lure || !fits_label(entry->text) || !fits_label(entry->zh) ||
        !fits_label(lure->zh) || !vocab_lure_acceptable(entry, lure, true)) {
        return vocab_stage_done();
    }

    st->entry = entry;
    st->lure = lure;
    st->purpose = in->purpose;
    st->allow_too_easy = in->allow_too_easy && in->purpose == VOCAB_PURPOSE_LEARN;
    st->show_ipa = vocab_entry_has_ipa(entry) && fits_label(entry->ipa);
    st->correct = vocab_rng_bit(in->rng) ? VOCAB_SIDE_DOWN : VOCAB_SIDE_UP;
    st->phase = PHASE_QUESTION;
    render(st, view);
    vocab_out_play_word(out, entry->uid);
    return status(st);
}

// UP/DOWN/OK clicks. Keys are ignored in the lock windows (spec 3.2 lock rows).
static vocab_stage_result_t on_click(vocab_stage1_t *st, vocab_key_t key, vocab_ms_t now,
                                     vocab_view_t *view, vocab_outbox_t *out)
{
    if (st->phase != PHASE_QUESTION && st->phase != PHASE_HOLD_WAIT) return status(st);
    if (key == VOCAB_KEY_OK) {
        vocab_out_play_word(out, st->entry->uid);
        return status(st);
    }
    if (key != VOCAB_KEY_UP && key != VOCAB_KEY_DOWN) return status(st);
    const bool correct = key == vocab_side_key(st->correct);

    if (st->phase == PHASE_HOLD_WAIT) {
        // The outcome was fixed by the first press; only the correct key moves on.
        if (correct) {
            st->phase = PHASE_DONE;
            return vocab_stage_done();
        }
        view->anim.nudge_seq[st->correct]++;
        return status(st);
    }

    if (correct) {
        st->phase = PHASE_FEEDBACK;
        render(st, view);
        vocab_out_play_cue(out, VOCAB_CUE_CORRECT);
        vocab_out_new_epoch(out);
        arm_lock(st, now, VOCAB_CORRECT_FEEDBACK_MS, out);
        return vocab_stage_with_outcome(status(st), VOCAB_RESULT_CORRECT_FIRST);
    }

    // Generic error hold (spec 5.3): one view update hides the wrong meaning and marks the
    // correct one, then CUE_WRONG + word and a timer-enforced lock with epochs at both ends.
    st->phase = PHASE_HOLD_LOCK;
    render(st, view);
    vocab_out_play_cue_then_word(out, VOCAB_CUE_WRONG, st->entry->uid);
    vocab_out_new_epoch(out);
    arm_lock(st, now, VOCAB_ERROR_HOLD_MS, out);
    return vocab_stage_with_outcome(status(st), VOCAB_RESULT_WRONG);
}

static vocab_stage_result_t on_timer(vocab_stage1_t *st, vocab_timer_id_t timer, vocab_ms_t now,
                                     vocab_view_t *view, vocab_outbox_t *out)
{
    if (timer != VOCAB_TIMER_STAGE || !phase_locked(st->phase)) return status(st);
    if (!vocab_ms_reached(now, st->lock_end)) {
        // Delivered before the deadline: keep the lock and re-arm the rest (capped at a full
        // window in case the caller's clock went backwards).
        const uint32_t full = phase_lock_ms(st->phase);
        const uint32_t left = st->lock_end - now;
        arm_lock(st, now, left < full ? left : full, out);
        return status(st);
    }
    if (st->phase == PHASE_FEEDBACK) {
        st->phase = PHASE_DONE;
        return vocab_stage_done();
    }
    st->phase = PHASE_HOLD_WAIT;
    render(st, view);
    vocab_out_new_epoch(out);
    return status(st);
}

vocab_stage_result_t vocab_stage1_handle(vocab_stage1_t *st, const vocab_event_t *ev,
                                         vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out)
{
    if (!st) return vocab_stage_done();
    if (!ev || !view || !out || st->phase == PHASE_DONE) return status(st);

    switch (ev->kind) {
    case VOCAB_EV_CLICK:
        return on_click(st, ev->key, now, view, out);
    case VOCAB_EV_LONG:
        // "Too easy" (spec 4.8, O12): LEARN question only; DOWN/OK LONG are session keys.
        if (ev->key == VOCAB_KEY_UP && st->phase == PHASE_QUESTION && st->allow_too_easy) {
            st->phase = PHASE_DONE;
            return vocab_stage_with_outcome(vocab_stage_done(), VOCAB_RESULT_TOO_EASY);
        }
        return status(st);
    case VOCAB_EV_TIMER:
        return on_timer(st, ev->timer, now, view, out);
    case VOCAB_EV_NONE:
    default:
        return status(st);
    }
}

vocab_stage_result_t vocab_stage1_resume(vocab_stage1_t *st, vocab_ms_t now,
                                         vocab_view_t *view, vocab_outbox_t *out)
{
    if (!st) return vocab_stage_done();
    if (!view || !out || st->phase == PHASE_DONE) return status(st);
    render(st, view);
    if (phase_locked(st->phase)) arm_lock(st, now, phase_lock_ms(st->phase), out);
    return status(st);
}
