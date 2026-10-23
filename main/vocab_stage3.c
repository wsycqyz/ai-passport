// main/vocab_stage3.c - Stage 3: phonics cloze card (spec 7.3, 7.4; see vocab_stage3.h).
//
// Phases:
//   QUESTION     waiting for an answer (first attempt, or the LEARN retry)    unlocked
//   RETRY_LOCK   LEARN miss 1: shake + VOCAB_S3_RETRY_LOCK_MS                   locked
//   SUCCESS      right chunk filled in + VOCAB_SUCCESS_HOLD_MS, then DONE       locked
//   REVEAL_LOCK  final miss: reveal + VOCAB_ERROR_HOLD_MS                       locked
//   REVEAL_WAIT  only the correct key finishes the card                         unlocked
//   DONE         the card is finished; every call returns DONE
// Every lock ends only with the VOCAB_TIMER_STAGE event armed when it started.
#include "vocab_stage3.h"

#include "vocab_bank.h"
#include "vocab_cloze.h"
#include "vocab_strings.h"

enum {
    PHASE_QUESTION = 0,
    PHASE_RETRY_LOCK,
    PHASE_SUCCESS,
    PHASE_REVEAL_LOCK,
    PHASE_REVEAL_WAIT,
    PHASE_DONE,
};

static bool is_lock_phase(uint8_t phase)
{
    return phase == PHASE_RETRY_LOCK || phase == PHASE_SUCCESS || phase == PHASE_REVEAL_LOCK;
}

static bool is_stage_timer(const vocab_event_t *ev)
{
    return ev->kind == VOCAB_EV_TIMER && ev->timer == VOCAB_TIMER_STAGE;
}

static bool is_click(const vocab_event_t *ev, vocab_key_t key)
{
    return ev->kind == VOCAB_EV_CLICK && ev->key == key;
}

// Starts a lock window of `ms` measured from `now`.
static void arm_lock(vocab_stage3_t *st, vocab_outbox_t *out, vocab_ms_t now, uint32_t ms)
{
    st->lock_until = now + ms;
    vocab_out_timer_start(out, VOCAB_TIMER_STAGE, ms);
}

static vocab_stage_result_t finish(vocab_stage3_t *st)
{
    st->phase = PHASE_DONE;
    return vocab_stage_done();
}

// Correct answer: the full correct word appears (spec 7.4 "Correct").
static vocab_stage_result_t on_correct(vocab_stage3_t *st, vocab_ms_t now, vocab_view_t *view,
                                       vocab_outbox_t *out)
{
    const vocab_result_t result = st->misses == 0u ? VOCAB_RESULT_CORRECT_FIRST
                                                   : VOCAB_RESULT_CORRECT_SECOND;
    st->phase = PHASE_SUCCESS;
    view->card.skeleton.filled = true;
    view->card.skeleton.blink = false;
    view->panel[st->correct].style = VOCAB_PANEL_CORRECT;
    vocab_view_show(&view->card.prompt, VOCAB_STR_S3_SUCCESS, VOCAB_RGB_SUCCESS);
    vocab_out_play_cue_then_word(out, VOCAB_CUE_CHEER, st->uid);
    vocab_out_new_epoch(out);
    arm_lock(st, out, now, VOCAB_SUCCESS_HOLD_MS);
    return vocab_stage_with_outcome(vocab_stage_running(true), result);
}

// LEARN miss 1 of 2: shake and retry; the blank stays empty, both options stay.
static vocab_stage_result_t on_retry(vocab_stage3_t *st, vocab_ms_t now, vocab_view_t *view,
                                     vocab_outbox_t *out)
{
    st->phase = PHASE_RETRY_LOCK;
    view->card.skeleton.blink = false;
    view->anim.shake_seq++;
    vocab_view_show(&view->card.prompt, VOCAB_STR_S3_RETRY, VOCAB_RGB_WARN);
    vocab_out_play_cue_then_word(out, VOCAB_CUE_WRONG, st->uid);
    vocab_out_new_epoch(out);
    arm_lock(st, out, now, VOCAB_S3_RETRY_LOCK_MS);
    return vocab_stage_running(true);
}

// Final miss: reveal the correct spelling, then the generic error hold (spec 5.3 steps 3-4).
static vocab_stage_result_t on_reveal(vocab_stage3_t *st, vocab_ms_t now, vocab_view_t *view,
                                      vocab_outbox_t *out)
{
    st->phase = PHASE_REVEAL_LOCK;
    vocab_view_hide_panel(view, vocab_side_other(st->correct));
    view->panel[st->correct].style = VOCAB_PANEL_CORRECT;
    view->panel[st->correct].nudge_repeat = false;
    view->card.skeleton.filled = true;
    view->card.skeleton.blink = false;
    view->badge = VOCAB_BADGE_REMEMBER;
    vocab_view_show(&view->card.prompt, VOCAB_STR_S3_REVEAL, VOCAB_RGB_YELLOW);
    view->hint.kind = VOCAB_HINT_NONE;
    vocab_out_play_cue_then_word(out, VOCAB_CUE_WRONG, st->uid);
    vocab_out_new_epoch(out);
    arm_lock(st, out, now, VOCAB_ERROR_HOLD_MS);
    return vocab_stage_with_outcome(vocab_stage_running(true), VOCAB_RESULT_WRONG);
}

static vocab_stage_result_t on_question(vocab_stage3_t *st, const vocab_event_t *ev,
                                        vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out)
{
    if (is_click(ev, VOCAB_KEY_OK)) {
        vocab_out_play_word(out, st->uid);
    } else if (is_click(ev, VOCAB_KEY_UP) || is_click(ev, VOCAB_KEY_DOWN)) {
        if (ev->key == vocab_side_key(st->correct)) return on_correct(st, now, view, out);
        st->misses++;
        return st->misses < st->max_misses ? on_retry(st, now, view, out)
                                           : on_reveal(st, now, view, out);
    }
    return vocab_stage_running(false);  // UP LONG, stray timers: ignored
}

static vocab_stage_result_t on_reveal_wait(vocab_stage3_t *st, const vocab_event_t *ev,
                                           vocab_view_t *view, vocab_outbox_t *out)
{
    if (is_click(ev, VOCAB_KEY_OK)) {
        vocab_out_play_word(out, st->uid);
    } else if (is_click(ev, VOCAB_KEY_UP) || is_click(ev, VOCAB_KEY_DOWN)) {
        if (ev->key == vocab_side_key(st->correct)) return finish(st);
        view->anim.nudge_seq[st->correct]++;  // the other key only nudges
    }
    return vocab_stage_running(false);
}

vocab_stage_result_t vocab_stage3_begin(vocab_stage3_t *st, const vocab_stage_input_t *in,
                                        vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out)
{
    if (!st) return vocab_stage_done();
    st->phase = PHASE_DONE;
    const vocab_entry_t *e = in ? vocab_bank_at(in->bank, in->index) : NULL;
    vocab_cloze_t cloze;
    char pre[VOCAB_VIEW_PART_CAP];
    char suf[VOCAB_VIEW_PART_CAP];
    if (!e || !in->rng || !view || !out || !vocab_cloze_from_entry(e, &cloze) ||
        !vocab_cloze_split(&cloze, pre, sizeof(pre), suf, sizeof(suf))) {
        return vocab_stage_done();  // unusable card: the session logs and skips it
    }

    st->bank = in->bank;
    st->index = in->index;
    st->uid = e->uid;
    st->purpose = in->purpose;
    st->correct = vocab_rng_bit(in->rng) ? VOCAB_SIDE_DOWN : VOCAB_SIDE_UP;
    st->misses = 0;
    st->max_misses = vocab_stage_s3_max_misses(in->purpose);
    st->phase = PHASE_QUESTION;
    st->lock_until = now;

    view->card.frame = true;
    vocab_view_show(&view->card.s3_meaning, e->zh, VOCAB_RGB_YELLOW);
    vocab_view_set_skeleton(view, pre, suf, cloze.right, cloze.wrong);
    vocab_view_show(&view->card.prompt, VOCAB_STR_S3_PROMPT, VOCAB_RGB_DIM);
    vocab_view_set_panel(view, st->correct, cloze.right, VOCAB_PANEL_S3, true);
    vocab_view_set_panel(view, vocab_side_other(st->correct), cloze.wrong, VOCAB_PANEL_S3, true);
    view->badge = vocab_view_card_badge(in->purpose, VOCAB_FORMAT_S3);
    view->hint.kind = VOCAB_HINT_REPLAY;
    vocab_out_play_word(out, st->uid);
    return vocab_stage_running(false);
}

vocab_stage_result_t vocab_stage3_handle(vocab_stage3_t *st, const vocab_event_t *ev,
                                         vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out)
{
    if (!st || st->phase >= PHASE_DONE) return vocab_stage_done();
    if (!ev || !view || !out) return vocab_stage_running(is_lock_phase(st->phase));

    switch (st->phase) {
    case PHASE_QUESTION:
        return on_question(st, ev, now, view, out);
    case PHASE_RETRY_LOCK:
        if (!is_stage_timer(ev)) return vocab_stage_running(true);
        st->phase = PHASE_QUESTION;
        view->card.skeleton.blink = true;
        vocab_out_new_epoch(out);
        return vocab_stage_running(false);
    case PHASE_SUCCESS:
        if (!is_stage_timer(ev)) return vocab_stage_running(true);
        return finish(st);
    case PHASE_REVEAL_LOCK:
        if (!is_stage_timer(ev)) return vocab_stage_running(true);
        st->phase = PHASE_REVEAL_WAIT;
        view->hint.kind = st->correct == VOCAB_SIDE_UP ? VOCAB_HINT_PRESS_UP
                                                       : VOCAB_HINT_PRESS_DOWN;
        view->panel[st->correct].nudge_repeat = true;
        vocab_out_new_epoch(out);
        return vocab_stage_running(false);
    case PHASE_REVEAL_WAIT:
    default:
        return on_reveal_wait(st, ev, view, out);
    }
}

vocab_stage_result_t vocab_stage3_resume(vocab_stage3_t *st, vocab_ms_t now,
                                         vocab_view_t *view, vocab_outbox_t *out)
{
    (void)view;
    if (!st || st->phase >= PHASE_DONE) return vocab_stage_done();
    if (!is_lock_phase(st->phase)) return vocab_stage_running(false);
    // Defensive: the session never pauses during a lock, but a lock must not get stuck.
    const uint32_t left = vocab_ms_reached(now, st->lock_until) ? 1u : st->lock_until - now;
    if (out) vocab_out_timer_start(out, VOCAB_TIMER_STAGE, left);
    return vocab_stage_running(true);
}
