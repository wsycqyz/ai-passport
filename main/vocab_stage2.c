// main/vocab_stage2.c - Stage 2: flashcard with self-rating (spec 6); see vocab_stage2.h.
//
// One card: the front shows the word (or a cloze sentence) and auto-plays the clip; clicks
// are ignored for VOCAB_S2_FRONT_MIN_MS so the child tries to recall first, then any key
// flips. The back shows the meaning and the example: UP = "I know" (KNOW, confirm flash),
// DOWN = "forgot, test me later" (FORGOT, acknowledgement), OK = replay. Every face is drawn
// from the stage state; the session applies the outcomes and drives the timers.
// Pure: no clock reads, no allocation, no static mutable state, no RNG draws.
#include "vocab_stage2.h"

#include <stddef.h>
#include <string.h>

#include "vocab_bank.h"
#include "vocab_strings.h"

// vocab_stage2_t.phase
enum {
    PHASE_FRONT_WAIT = 0,  // front, minimum display time running: clicks are ignored
    PHASE_FRONT_READY,     // front, any click flips
    PHASE_BACK,            // back, waiting for the self-rating
    PHASE_CONFIRM,         // "I know" confirm flash (lock window)
    PHASE_FORGOT_ACK,      // "forgot" acknowledgement (lock window)
    PHASE_DONE,            // finished, or unusable input
};

static bool has_text(const char *s)
{
    return s && s[0] != '\0';
}

static bool on_front(const vocab_stage2_t *st)
{
    return st->phase == PHASE_FRONT_WAIT || st->phase == PHASE_FRONT_READY;
}

static bool in_lock(const vocab_stage2_t *st)
{
    return st->phase == PHASE_CONFIRM || st->phase == PHASE_FORGOT_ACK;
}

// Result describing the current state without a new outcome.
static vocab_stage_result_t status(const vocab_stage2_t *st)
{
    if (st->phase >= PHASE_DONE || !st->entry) return vocab_stage_done();
    return vocab_stage_running(in_lock(st));
}

// Spec 6.1 / 12.1 / AC-27: the cloze front may show example_cloze only when it is exactly
// `example` with the entry text (present once) replaced by VOCAB_CLOZE_BLANK and contains
// no other underscore. Then the blank is always four underscores (the answer's length never
// leaks) and every other character is bank English.
static bool cloze_sentence_ok(const vocab_entry_t *e)
{
    if (!has_text(e->example_cloze) || !has_text(e->example) || !has_text(e->text)) return false;
    const char *cloze = e->example_cloze;
    const char *blank = strstr(cloze, VOCAB_CLOZE_BLANK);
    if (!blank) return false;
    const size_t blank_len = strlen(VOCAB_CLOZE_BLANK);
    for (const char *p = cloze; *p != '\0'; p++) {
        if (*p == '_' && (p < blank || p >= blank + blank_len)) return false;
    }
    const size_t pre_len = (size_t)(blank - cloze);
    const size_t text_len = strlen(e->text);
    if (strncmp(e->example, cloze, pre_len) != 0) return false;
    const char *at = e->example + pre_len;
    if (strncmp(at, e->text, text_len) != 0) return false;
    if (strcmp(at + text_len, blank + blank_len) != 0) return false;
    // The text must not appear anywhere else, or the sentence would give the answer away.
    return strstr(e->example, e->text) == at && strstr(at + 1, e->text) == NULL;
}

// Hides every stage-region element that the current face may not use (spec 11.5: whatever
// a screen does not list is hidden). card.banner, the animation counters and all
// session-owned fields are left alone.
static void clear_region(vocab_view_t *view)
{
    vocab_view_card_t *card = &view->card;
    vocab_view_hide(&card->word);
    vocab_view_hide(&card->ipa);
    vocab_view_hide(&card->meaning);
    vocab_view_hide(&card->sentence);
    vocab_view_hide(&card->example);
    vocab_view_hide(&card->s3_meaning);
    vocab_view_hide(&card->prompt);
    memset(&card->skeleton, 0, sizeof(card->skeleton));
    vocab_view_hide_panel(view, VOCAB_SIDE_UP);
    vocab_view_hide_panel(view, VOCAB_SIDE_DOWN);
}

static vocab_badge_t face_badge(const vocab_stage2_t *st, bool back)
{
    if (back && st->purpose == VOCAB_PURPOSE_LEARN) return VOCAB_BADGE_SELF_RATE;
    return vocab_view_card_badge(st->purpose, VOCAB_FORMAT_S2);
}

// Draws the stage region of the current face (spec 6.1, 6.2, 11.5). Not called when DONE.
static void render(const vocab_stage2_t *st, vocab_view_t *view)
{
    const vocab_entry_t *e = st->entry;
    clear_region(view);
    view->card.frame = true;
    if (on_front(st)) {
        if (st->cloze_front) {
            vocab_view_show(&view->card.sentence, e->example_cloze, VOCAB_RGB_TEXT);
            vocab_view_show(&view->card.prompt, VOCAB_STR_S2_FRONT_CLOZE, VOCAB_RGB_YELLOW);
        } else {
            vocab_view_show(&view->card.word, e->text, VOCAB_RGB_TEXT);
            if (vocab_entry_has_ipa(e)) vocab_view_show(&view->card.ipa, e->ipa, VOCAB_RGB_DIM);
            vocab_view_show(&view->card.prompt, VOCAB_STR_S2_FRONT_WORD, VOCAB_RGB_YELLOW);
        }
        view->badge = face_badge(st, false);
        view->hint.kind = st->phase == PHASE_FRONT_WAIT ? VOCAB_HINT_THINK : VOCAB_HINT_FLIP;
        return;
    }
    // Back, also during the confirm flash and the forgot acknowledgement. IPA is hidden to
    // make room for the meaning (spec 6.2).
    vocab_view_show(&view->card.word, e->text, VOCAB_RGB_TEXT);
    if (has_text(e->zh)) vocab_view_show(&view->card.meaning, e->zh, VOCAB_RGB_YELLOW);
    if (st->phase == PHASE_FORGOT_ACK) {
        // The prompt line overlaps the example line (spec 11.4): the ack replaces it.
        vocab_view_show(&view->card.prompt, VOCAB_STR_S2_FORGOT_ACK, VOCAB_RGB_WARN);
    } else if (has_text(e->example)) {
        vocab_view_show(&view->card.example, e->example, VOCAB_RGB_SOFT);
    }
    vocab_view_set_panel(view, VOCAB_SIDE_UP, VOCAB_STR_S2_KNOW, VOCAB_PANEL_KNOW, false);
    vocab_view_set_panel(view, VOCAB_SIDE_DOWN, VOCAB_STR_S2_FORGOT, VOCAB_PANEL_FORGOT, false);
    view->badge = face_badge(st, true);
    view->hint.kind = VOCAB_HINT_REPLAY;
}

vocab_stage_result_t vocab_stage2_begin(vocab_stage2_t *st, const vocab_stage_input_t *in,
                                        vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out)
{
    if (!st) return vocab_stage_done();
    memset(st, 0, sizeof(*st));
    st->phase = PHASE_DONE;
    const vocab_entry_t *e = in ? vocab_bank_at(in->bank, in->index) : NULL;
    if (!e || !has_text(e->text)) return vocab_stage_done();  // unusable: the session skips

    st->entry = e;
    st->purpose = in->purpose;
    st->cloze_front = in->cloze_front && cloze_sentence_ok(e);
    st->allow_too_easy = in->allow_too_easy && in->purpose == VOCAB_PURPOSE_LEARN;
    st->front_deadline = now + VOCAB_S2_FRONT_MIN_MS;
    st->phase = PHASE_FRONT_WAIT;

    render(st, view);
    vocab_out_play_word(out, e->uid);
    vocab_out_timer_start(out, VOCAB_TIMER_STAGE, VOCAB_S2_FRONT_MIN_MS);
    return vocab_stage_running(false);
}

// Front -> back (spec 6.2): CUE_FLIP then the word clip; a new input epoch.
static vocab_stage_result_t flip(vocab_stage2_t *st, bool timer_pending, vocab_view_t *view,
                                 vocab_outbox_t *out)
{
    st->phase = PHASE_BACK;
    render(st, view);
    if (timer_pending) vocab_out_timer_cancel(out, VOCAB_TIMER_STAGE);
    vocab_out_play_cue_then_word(out, VOCAB_CUE_FLIP, st->entry->uid);
    vocab_out_new_epoch(out);
    return vocab_stage_running(false);
}

// UP on the back (O3): CONFIRM_FLASH on the UP panel with CUE_TICK, locked until the timer.
static vocab_stage_result_t know(vocab_stage2_t *st, vocab_view_t *view, vocab_outbox_t *out)
{
    st->phase = PHASE_CONFIRM;
    view->anim.confirm_seq[VOCAB_SIDE_UP]++;
    vocab_out_play_cue(out, VOCAB_CUE_TICK);
    vocab_out_new_epoch(out);
    vocab_out_timer_start(out, VOCAB_TIMER_STAGE, VOCAB_S2_CONFIRM_MS);
    return vocab_stage_with_outcome(vocab_stage_running(true), VOCAB_RESULT_KNOW);
}

// DOWN on the back (O4): orange acknowledgement with CUE_TICK, locked until the timer.
// The session inserts the TEST_LATER card two cards later, never immediately.
static vocab_stage_result_t forgot(vocab_stage2_t *st, vocab_view_t *view, vocab_outbox_t *out)
{
    st->phase = PHASE_FORGOT_ACK;
    render(st, view);
    vocab_out_play_cue(out, VOCAB_CUE_TICK);
    vocab_out_new_epoch(out);
    vocab_out_timer_start(out, VOCAB_TIMER_STAGE, VOCAB_S2_FORGOT_ACK_MS);
    return vocab_stage_with_outcome(vocab_stage_running(true), VOCAB_RESULT_FORGOT);
}

static vocab_stage_result_t on_click(vocab_stage2_t *st, vocab_key_t key, vocab_ms_t now,
                                     vocab_view_t *view, vocab_outbox_t *out)
{
    if (key != VOCAB_KEY_UP && key != VOCAB_KEY_DOWN && key != VOCAB_KEY_OK) return status(st);
    switch (st->phase) {
    case PHASE_FRONT_WAIT:
        // Ignored until the minimum has elapsed, even if its timer event is still queued.
        if (!vocab_ms_reached(now, st->front_deadline)) return status(st);
        return flip(st, true, view, out);
    case PHASE_FRONT_READY:
        return flip(st, false, view, out);
    case PHASE_BACK:
        if (key == VOCAB_KEY_UP) return know(st, view, out);
        if (key == VOCAB_KEY_DOWN) return forgot(st, view, out);
        vocab_out_play_word(out, st->entry->uid);  // OK = replay (spec 6.2)
        return status(st);
    default:
        return status(st);  // lock windows: keys are ignored
    }
}

static vocab_stage_result_t on_long(vocab_stage2_t *st, vocab_key_t key)
{
    // Too easy (O12): only on the front of a LEARN card; the back already shows the answer.
    if (key == VOCAB_KEY_UP && on_front(st) && st->allow_too_easy) {
        st->phase = PHASE_DONE;
        return vocab_stage_with_outcome(vocab_stage_done(), VOCAB_RESULT_TOO_EASY);
    }
    return status(st);
}

static vocab_stage_result_t on_timer(vocab_stage2_t *st, vocab_timer_id_t timer, vocab_view_t *view)
{
    if (timer != VOCAB_TIMER_STAGE) return status(st);
    switch (st->phase) {
    case PHASE_FRONT_WAIT:
        st->phase = PHASE_FRONT_READY;
        view->hint.kind = VOCAB_HINT_FLIP;
        return status(st);
    case PHASE_CONFIRM:
    case PHASE_FORGOT_ACK:
        st->phase = PHASE_DONE;
        return vocab_stage_done();
    default:
        return status(st);  // stale timer (e.g. after a flip at the deadline): ignored
    }
}

vocab_stage_result_t vocab_stage2_handle(vocab_stage2_t *st, const vocab_event_t *ev,
                                         vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out)
{
    if (!st) return vocab_stage_done();
    if (st->phase >= PHASE_DONE || !st->entry || !ev) return status(st);
    switch (ev->kind) {
    case VOCAB_EV_CLICK:
        return on_click(st, ev->key, now, view, out);
    case VOCAB_EV_LONG:
        return on_long(st, ev->key);
    case VOCAB_EV_TIMER:
        return on_timer(st, ev->timer, view);
    case VOCAB_EV_NONE:
    default:
        return status(st);
    }
}

vocab_stage_result_t vocab_stage2_resume(vocab_stage2_t *st, vocab_ms_t now,
                                         vocab_view_t *view, vocab_outbox_t *out)
{
    if (!st) return vocab_stage_done();
    if (st->phase >= PHASE_DONE || !st->entry) return status(st);
    // The session restored the view and cancelled the stage timers: redraw the face
    // (idempotent) and re-arm the timer the state needs. No audio, no outcome.
    render(st, view);
    switch (st->phase) {
    case PHASE_FRONT_WAIT:
        st->front_deadline = now + VOCAB_S2_FRONT_MIN_MS;
        vocab_out_timer_start(out, VOCAB_TIMER_STAGE, VOCAB_S2_FRONT_MIN_MS);
        break;
    case PHASE_CONFIRM:
        // Not reachable through the session (no pause during a lock); never get stuck.
        vocab_out_timer_start(out, VOCAB_TIMER_STAGE, VOCAB_S2_CONFIRM_MS);
        break;
    case PHASE_FORGOT_ACK:
        vocab_out_timer_start(out, VOCAB_TIMER_STAGE, VOCAB_S2_FORGOT_ACK_MS);
        break;
    default:
        break;
    }
    return status(st);
}
